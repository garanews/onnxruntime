set(composable_kernel_URL https://github.com/ROCmSoftwarePlatform/composable_kernel.git)
set(composable_kernel_TAG e1a3fff67510be2af023b31587e411230b994631) # 2022-08-25 07:43:43 +0800

set(BUILD_DEV OFF)
set(PATCH ${PROJECT_SOURCE_DIR}/patches/composable_kernel/Fix_Clang_Build.patch)

include(FetchContent)
FetchContent_Declare(composable_kernel
  GIT_REPOSITORY ${composable_kernel_URL}
  GIT_TAG        ${composable_kernel_TAG}
  PATCH_COMMAND  git apply --reverse --check ${PATCH} || git apply --ignore-space-change --ignore-whitespace ${PATCH}
)

FetchContent_GetProperties(composable_kernel)
if(NOT composable_kernel_POPULATED)
  FetchContent_Populate(composable_kernel)
  add_subdirectory(${composable_kernel_SOURCE_DIR} ${composable_kernel_BINARY_DIR} EXCLUDE_FROM_ALL)

  add_library(onnxruntime_composable_kernel_includes INTERFACE)
  target_include_directories(onnxruntime_composable_kernel_includes INTERFACE
    ${composable_kernel_SOURCE_DIR}/include
    ${composable_kernel_SOURCE_DIR}/library/include)
endif()
