cmake_minimum_required(VERSION 3.25)
project(MeguvizMemoryAhbGlesUpload VERSION 1.2.1 LANGUAGES C CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

if(ANDROID)
  if(ANDROID_PLATFORM MATCHES "([0-9]+)$" AND CMAKE_MATCH_1 LESS 26)
    message(FATAL_ERROR "AHardwareBuffer requires Android API 26 or newer")
  endif()
  find_package(MeguvizMemoryGpuAhbBridge CONFIG REQUIRED)
  add_library(mgmem_ahb_gles_upload STATIC src/ahb_upload.cpp src/platform_gl.cpp src/platform_fd_android.cpp)
  target_compile_features(mgmem_ahb_gles_upload PUBLIC cxx_std_17)
  target_include_directories(mgmem_ahb_gles_upload PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(mgmem_ahb_gles_upload PUBLIC MeguvizMemoryGpuAhbBridge::gpu_ahb EGL GLESv3 Threads::Threads)
  set_target_properties(mgmem_ahb_gles_upload PROPERTIES EXPORT_NAME upload CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
  add_library(MeguvizMemoryAhbGlesUpload::upload ALIAS mgmem_ahb_gles_upload)
  install(TARGETS mgmem_ahb_gles_upload EXPORT MeguvizMemoryAhbGlesUploadTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  option(MGMEM_AHB_GLES_GUARDED "Build opt-in guarded producer queue integration" OFF)
  if(MGMEM_AHB_GLES_GUARDED)
    include(CheckCXXSourceCompiles)
    set(_mgmem_guard_saved_libraries "${CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_LIBRARIES MeguvizMemoryGpuAhbBridge::gpu_ahb)
    unset(_mgmem_has_guarded_pack CACHE)
    check_cxx_source_compiles("#include <meguviz/compute_android.h>
#include <mgmem/gpu_ahb.h>
#include <stdint.h>
extern \"C\" MgcStatus mgc_android_set_guarded_producer_fence(MgcLease *, int32_t, uint64_t *);
extern \"C\" MgcStatus mgc_android_finish_guarded_producer(MgcLease *, uint64_t, MgcStatus);
extern \"C\" mgmem_status mgmem_gpu_ahb_guard_register_internal(mgmem_gpu_ahb *, int32_t,
  int32_t (*)(MgcLease *, int32_t, uint64_t *), uint64_t *, int32_t *);
extern \"C\" mgmem_status mgmem_gpu_ahb_guard_retire_verified_internal(mgmem_gpu_ahb **, int32_t);
int main() { uint64_t g=0; return mgc_android_set_guarded_producer_fence(nullptr,-1,&g) +
  mgc_android_finish_guarded_producer(nullptr,g,MGC_SUCCESS) +
  mgmem_gpu_ahb_guard_register_internal(nullptr,-1,nullptr,&g,nullptr) +
  mgmem_gpu_ahb_guard_retire_verified_internal(nullptr,-1); }" _mgmem_has_guarded_pack)
    set(CMAKE_REQUIRED_LIBRARIES "${_mgmem_guard_saved_libraries}")
    unset(_mgmem_guard_saved_libraries)
    if(NOT _mgmem_has_guarded_pack)
      message(FATAL_ERROR "Guarded integration requires both guarded Compute symbols and the updated GPU AHB bridge")
    endif()
    add_library(mgmem_ahb_gles_guarded STATIC src/ahb_upload_guarded.cpp)
    target_compile_features(mgmem_ahb_gles_guarded PUBLIC cxx_std_17)
    target_compile_definitions(mgmem_ahb_gles_guarded PRIVATE MGMEM_GUARDED_REQUIRE_SDK_ENUMS)
    target_include_directories(mgmem_ahb_gles_guarded PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(mgmem_ahb_gles_guarded PUBLIC mgmem_ahb_gles_upload)
    set_target_properties(mgmem_ahb_gles_guarded PROPERTIES EXPORT_NAME guarded CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
    add_library(MeguvizMemoryAhbGlesUpload::guarded ALIAS mgmem_ahb_gles_guarded)
    install(TARGETS mgmem_ahb_gles_guarded EXPORT MeguvizMemoryAhbGlesUploadTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  endif()
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT MeguvizMemoryAhbGlesUploadTargets NAMESPACE MeguvizMemoryAhbGlesUpload::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryAhbGlesUpload)
  configure_package_config_file(cmake/MeguvizMemoryAhbGlesUploadConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryAhbGlesUploadConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryAhbGlesUpload)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryAhbGlesUploadConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryAhbGlesUploadConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryAhbGlesUploadConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryAhbGlesUpload)
else()
  enable_testing()
  add_executable(mgmem_ahb_upload_tests tests/ahb_upload_tests.cpp src/ahb_upload.cpp)
  target_compile_features(mgmem_ahb_upload_tests PRIVATE cxx_std_17)
  target_include_directories(mgmem_ahb_upload_tests PRIVATE include src ../compute_gpu_ahb/include ../../include)
  add_test(NAME ahb_upload_flow COMMAND mgmem_ahb_upload_tests)
  foreach(mode IN ITEMS quarantine fence hard cleanup retire cancel_release sizes ready_error producer_error ready_unverified producer_unverified)
    add_test(NAME ahb_upload_${mode} COMMAND mgmem_ahb_upload_tests ${mode})
  endforeach()
  add_executable(mgmem_ahb_platform_tests tests/platform_gl_tests.cpp src/platform_gl.cpp)
  target_compile_features(mgmem_ahb_platform_tests PRIVATE cxx_std_17)
  target_compile_definitions(mgmem_ahb_platform_tests PRIVATE MGMEM_AHB_GLES_TESTING)
  target_link_libraries(mgmem_ahb_platform_tests PRIVATE Threads::Threads)
  target_include_directories(mgmem_ahb_platform_tests PRIVATE tests/fake include src ../compute_gpu_ahb/include ../../include)
  foreach(mode IN ITEMS success unsupported oom lost restore dup delete switch setup token unpack)
    add_test(NAME ahb_platform_${mode} COMMAND mgmem_ahb_platform_tests ${mode})
  endforeach()
  add_executable(mgmem_ahb_fd_tests tests/platform_fd_tests.cpp src/platform_fd_android.cpp)
  target_compile_features(mgmem_ahb_fd_tests PRIVATE cxx_std_17)
  target_include_directories(mgmem_ahb_fd_tests PRIVATE tests/fake include src ../compute_gpu_ahb/include ../../include)
  add_test(NAME ahb_fd_status COMMAND mgmem_ahb_fd_tests)
  add_executable(mgmem_sync_file_wait_tests tests/sync_file_wait_tests.cpp)
  target_compile_features(mgmem_sync_file_wait_tests PRIVATE cxx_std_17)
  target_include_directories(mgmem_sync_file_wait_tests PRIVATE tests/fake)
  add_test(NAME sync_file_wait_deadline COMMAND mgmem_sync_file_wait_tests)
endif()
