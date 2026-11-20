cmake_minimum_required(VERSION 3.25)
project(MeguvizMemoryGpuAhbBridge VERSION 1.2.2 LANGUAGES C CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(MeguvizMemory CONFIG REQUIRED)
find_package(MeguvizCompute CONFIG REQUIRED)

if(ANDROID)
  if(ANDROID_PLATFORM MATCHES "([0-9]+)$" AND CMAKE_MATCH_1 LESS 26)
    message(FATAL_ERROR "AHardwareBuffer requires Android API 26 or newer")
  endif()
  add_library(mgmem_gpu_ahb_bridge STATIC src/gpu_ahb.cpp)
  target_compile_features(mgmem_gpu_ahb_bridge PUBLIC cxx_std_17)
  target_include_directories(mgmem_gpu_ahb_bridge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(mgmem_gpu_ahb_bridge PUBLIC MeguvizMemory::mgmem MeguvizCompute::core android)
  set_target_properties(mgmem_gpu_ahb_bridge PROPERTIES EXPORT_NAME gpu_ahb CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
  add_library(MeguvizMemoryGpuAhbBridge::gpu_ahb ALIAS mgmem_gpu_ahb_bridge)
  install(TARGETS mgmem_gpu_ahb_bridge EXPORT MeguvizMemoryGpuAhbBridgeTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT MeguvizMemoryGpuAhbBridgeTargets NAMESPACE MeguvizMemoryGpuAhbBridge::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryGpuAhbBridge)
  configure_package_config_file(cmake/MeguvizMemoryGpuAhbBridgeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryGpuAhbBridgeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryGpuAhbBridge)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryGpuAhbBridgeConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryGpuAhbBridgeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryGpuAhbBridgeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryGpuAhbBridge)
else()
  enable_testing()
  get_target_property(compute_headers MeguvizCompute::core INTERFACE_INCLUDE_DIRECTORIES)
  add_executable(mgmem_gpu_ahb_tests tests/gpu_ahb_tests.cpp src/gpu_ahb.cpp)
  target_compile_features(mgmem_gpu_ahb_tests PRIVATE cxx_std_17)
  target_compile_definitions(mgmem_gpu_ahb_tests PRIVATE MGMEM_GPU_AHB_TESTING)
  target_include_directories(mgmem_gpu_ahb_tests PRIVATE include ${compute_headers})
  target_link_libraries(mgmem_gpu_ahb_tests PRIVATE MeguvizMemory::mgmem)
  add_test(NAME gpu_ahb_fence_lifetime COMMAND mgmem_gpu_ahb_tests)
  add_test(NAME gpu_ahb_hard_failure_quarantine COMMAND mgmem_gpu_ahb_tests --hard)
  add_test(NAME gpu_ahb_final_release_quarantine COMMAND mgmem_gpu_ahb_tests --release-failure)
  set_tests_properties(gpu_ahb_fence_lifetime gpu_ahb_hard_failure_quarantine gpu_ahb_final_release_quarantine PROPERTIES TIMEOUT 30)
endif()
