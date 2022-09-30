// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/tensorprotoutils.h"
#include "core/graph/graph_utils.h"
#include "core/optimizer/initializer.h"
#include "core/optimizer/constant_sharing.h"
#include "core/optimizer/utils.h"

namespace onnxruntime {

namespace {

// Supports limited data types.
static constexpr std::array supported_data_types{
    ONNX_NAMESPACE::TensorProto_DataType_FLOAT,
    ONNX_NAMESPACE::TensorProto_DataType_FLOAT16,
    ONNX_NAMESPACE::TensorProto_DataType_DOUBLE,
    ONNX_NAMESPACE::TensorProto_DataType_INT32,
    ONNX_NAMESPACE::TensorProto_DataType_INT64,
};

bool IsSupportedDataType(int32_t data_type) {
  return std::find(std::begin(supported_data_types), std::end(supported_data_types), data_type) !=
         std::end(supported_data_types);
}

bool IsValidSingleValueShape(const ONNX_NAMESPACE::TensorShapeProto* input_shape) {
  if (input_shape == nullptr) return false;

  size_t dim_size = static_cast<size_t>(input_shape->dim_size());
  return dim_size == 0 ||
         (dim_size == 1 && utils::HasDimValue(input_shape->dim(0)) && input_shape->dim(0).dim_value() == 1);
}

static constexpr char SHARED_INITIALIZER_PREFIX[] = "ortshared_";
bool IsSharedInitializer(std::string_view initializer_name) {
  return initializer_name.rfind(SHARED_INITIALIZER_PREFIX, 0) == 0;
}

bool PrepareInputPortsToReplace(Graph& graph, const NodeArg* origin_initializer_node_arg,
                                InlinedHashMap<const Node*, InlinedVector<int>>& consumer_node_to_input_ports_map) {
  std::vector<const Node*> consumers = graph.GetConsumerNodes(origin_initializer_node_arg->Name());

  // If usage is from subgraph, skip it now, can be extended to support if there is a need.
  bool found_subgraph_usage = false;
  for (const Node* const_node : consumers) {
    for (int i = 0; i < static_cast<int>(const_node->ImplicitInputDefs().size()); ++i) {
      if (const_node->ImplicitInputDefs()[i] == origin_initializer_node_arg) {
        found_subgraph_usage = true;
        break;
      }
    }

    if (found_subgraph_usage) {
      break;
    }

    // Iterate all input defs to replace those that are equal to origin_initializer_node_arg,
    // Then it would be safe to remove the consumer node aferwards.
    for (int i = 0; i < static_cast<int>(const_node->InputDefs().size()); ++i) {
      if (const_node->InputDefs()[i] == origin_initializer_node_arg) {
        consumer_node_to_input_ports_map[const_node].push_back(i);
      }
    }
  }

  return found_subgraph_usage;
}

// Replace all consumer nodes to use shared initializers.
void ReplaceInputsToUseSharedInitializer(Graph& graph,
                                         const InlinedHashMap<const Node*, InlinedVector<int>>&
                                             consumer_node_to_input_ports_map,
                                         const NodeArg* origin_initializer_node_arg,
                                         NodeArg* shared_initializer_node_arg) {
  for (auto it = consumer_node_to_input_ports_map.begin(), end = consumer_node_to_input_ports_map.end();
       it != end; ++it) {
    Node* node = graph.GetNode(it->first->Index());
    // Iterate all input defs to replace those that are equal to origin_initializer_node_arg,
    // Then it would be safe to remove the consumer node.
    for (int input_index : it->second) {
      graph_utils::ReplaceNodeInput(*node, input_index, *shared_initializer_node_arg);
    }
    graph.RemoveConsumerNode(origin_initializer_node_arg->Name(), node);

    //  Add consumer ref count for shared scalar initializer.
    std::vector<const Node*> consumers = graph.GetConsumerNodes(shared_initializer_node_arg->Name());
    if (std::find(consumers.begin(), consumers.end(), node) == consumers.end()) {
      graph.AddConsumerNode(shared_initializer_node_arg->Name(), node);
    }
  }

  // Remove the initializer if no other consumer nodes.
  if (graph.GetConsumerNodes(origin_initializer_node_arg->Name()).size() == 0) {
    graph.RemoveInitializedTensor(origin_initializer_node_arg->Name());
  }
}

#define DispatchOnONNXTensorProtoDataTypeWithReturn(data_type, retval, function, ...) \
  switch (data_type) {                                                                \
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:                                  \
      retval = function<float>(__VA_ARGS__);                                          \
      break;                                                                          \
    case ONNX_NAMESPACE::TensorProto_DataType_DOUBLE:                                 \
      retval = function<double>(__VA_ARGS__);                                         \
      break;                                                                          \
    case ONNX_NAMESPACE::TensorProto_DataType_INT32:                                  \
      retval = function<int32_t>(__VA_ARGS__);                                        \
      break;                                                                          \
    case ONNX_NAMESPACE::TensorProto_DataType_INT64:                                  \
      retval = function<int64_t>(__VA_ARGS__);                                        \
      break;                                                                          \
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT16:                                \
      retval = function<MLFloat16>(__VA_ARGS__);                                      \
      break;                                                                          \
    default:                                                                          \
      ORT_ENFORCE(false, "Unknown tensor type of ", data_type);                       \
  }

template <typename T>
size_t GetOrAddValueIntoConstantStore(const onnxruntime::Initializer& initializer,
                                      InlinedVector<std::variant<int32_t, int64_t, float, double>>&
                                          const_value_store) {
  std::variant<int32_t, int64_t, float, double> value;
  if (std::is_same<T, MLFloat16>::value) {
    value = math::halfToFloat(initializer.data<MLFloat16>()->val);
  } else {
    value = *initializer.data<T>();
  }

  auto it = std::find(const_value_store.begin(), const_value_store.end(), value);
  if (it == const_value_store.end()) {
    const_value_store.push_back(value);
    return const_value_store.size() - 1;
  }
  return it - const_value_store.begin();
}

}  // namespace

Status ConstantSharing::ApplyImpl(Graph& graph, bool& modified, int /*graph_level*/,
                                  const logging::Logger& /*logger*/) const {
  // Accumulated map from type/value/rank to initializer:
  //   The key is a string representation of initializer's data type, value and rank.
  //   The value is newly created initializer NodeArg* to be shared.
  InlinedHashMap<std::string, NodeArg*> pattern_key_to_shared_arg_map;
  const InitializedTensorSet& initialized_tensor_set = graph.GetAllInitializedTensors();
  InlinedVector<std::string> original_initializer_names;
  original_initializer_names.reserve(initialized_tensor_set.size());
  for (const auto& entry : initialized_tensor_set) {
    // Ignore if the initializer already handled, or not a constant initializer.
    if (IsSharedInitializer(entry.first) ||
        !graph_utils::IsConstantInitializer(graph, entry.first) ||
        excluded_initializers_.find(entry.first) != excluded_initializers_.end()) {
      continue;
    }

    original_initializer_names.push_back(entry.first);
  }

  // We avoid using the scalar value directly in pattern_key because the value for example INT_MAX can be super big
  // and it will be hard to read. Instead, we use a unique id for each scalar value, and a map from the value to
  // unique id.
  InlinedVector<std::variant<int32_t, int64_t, float, double>> const_value_store;
  for (const auto& initializer_name : original_initializer_names) {
    NodeArg* origin_initializer_node_arg = graph.GetNodeArg(initializer_name);

    if (origin_initializer_node_arg == nullptr || !IsValidSingleValueShape(origin_initializer_node_arg->Shape())) {
      continue;
    }

    int32_t data_type = -1;
    // Ignore if not constant initializers.
    const ONNX_NAMESPACE::TensorProto* tensor_proto = graph.GetConstantInitializer(
        origin_initializer_node_arg->Name(), true);
    if (!tensor_proto || !IsSupportedDataType(tensor_proto->data_type())) {
      continue;
    }

    // A map used to collect those consumers who has inputs using origin_initializer_node_arg.
    //   The key is consumer Node pointer.
    //   The value is a list of indices for the consumer Nodes' input (that used origin_initializer_node_arg).
    InlinedHashMap<const Node*, InlinedVector<int>> consumer_node_to_input_ports_map;
    bool found_subgraph_usage = PrepareInputPortsToReplace(graph, origin_initializer_node_arg,
                                                           consumer_node_to_input_ports_map);
    if (found_subgraph_usage || consumer_node_to_input_ports_map.size() == 0) {
      continue;
    }

    size_t value_id = 0;
    onnxruntime::Initializer initializer{*tensor_proto, graph.ModelPath()};
    DispatchOnONNXTensorProtoDataTypeWithReturn(tensor_proto->data_type(), value_id,
                                                GetOrAddValueIntoConstantStore,
                                                initializer, const_value_store);

    // Construct a string by data type, value, and rank. Used as a key in pattern_key_to_shared_arg_map.
    const std::string pattern_key = MakeString(SHARED_INITIALIZER_PREFIX, value_id, "_", data_type, "_",
                                               origin_initializer_node_arg->Shape()->dim_size());

    // If there is no such existing scalar pattern, add a new one.
    if (pattern_key_to_shared_arg_map.find(pattern_key) == pattern_key_to_shared_arg_map.end()) {
      // Do a copy and rename the TensorProto.
      ONNX_NAMESPACE::TensorProto constant_tensor_proto_as_replacement(*tensor_proto);
      constant_tensor_proto_as_replacement.set_name(graph.GenerateNodeArgName(pattern_key));
      NodeArg& shared_scalar_initializer_node_arg = graph_utils::AddInitializer(graph,
                                                                                constant_tensor_proto_as_replacement);
      pattern_key_to_shared_arg_map[pattern_key] = &shared_scalar_initializer_node_arg;
    }

    ReplaceInputsToUseSharedInitializer(graph, consumer_node_to_input_ports_map, origin_initializer_node_arg,
                                        pattern_key_to_shared_arg_map[pattern_key]);

    modified = true;
  }

  return Status::OK();
}

}  // namespace onnxruntime
