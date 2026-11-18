cmake_minimum_required(VERSION 3.25)
project(MeguvizMemoryAhbReadBridge VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(MeguvizMemory CONFIG REQUIRED)
find_package(MeguvizCompute CONFIG REQUIRED)

if(ANDROID)
  # CMAKE_SYSTEM_VERSION can be "1" with the NDK's legacy toolchain.
  if(ANDROID_PLATFORM MATCHES "([0-9]+)$")
    if(CMAKE_MATCH_1 LESS 26)
      message(FATAL_ERROR "AHardwareBuffer requires Android API 26 or newer")
    endif()
  endif()
  include(CheckCXXSourceCompiles)
  set(_ahb_saved_libraries "${CMAKE_REQUIRED_LIBRARIES}")
  set(CMAKE_REQUIRED_LIBRARIES MeguvizMemory::mgmem MeguvizCompute::core android)
  unset(_ahb_dependencies_usable CACHE)
  check_cxx_source_compiles("#include <mgmem/mgmem.h>
#include <meguviz/compute_android.h>
int main() { int fd = -1; AHardwareBuffer *buffer = nullptr; MgcLeaseInfo info{}; return
  (int)mgmem_import_external_checked_v1(nullptr, nullptr, nullptr) + mgc_lease_info(nullptr, &info) +
  mgc_android_get_buffer(nullptr, &buffer) + mgc_android_release_to_external(nullptr, &fd) +
  mgc_android_set_acquire_fence(nullptr, fd); }" _ahb_dependencies_usable)
  set(CMAKE_REQUIRED_LIBRARIES "${_ahb_saved_libraries}")
  unset(_ahb_saved_libraries)
  if(NOT _ahb_dependencies_usable)
    message(FATAL_ERROR "AHB READ bridge requires Memory checked import and Compute Android external access symbols")
  endif()
  add_library(mgmem_ahb_read_bridge STATIC src/ahb_read.cpp)
  target_compile_features(mgmem_ahb_read_bridge PUBLIC cxx_std_17)
  target_include_directories(mgmem_ahb_read_bridge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(mgmem_ahb_read_bridge PUBLIC MeguvizMemory::mgmem MeguvizCompute::core android)
  set_target_properties(mgmem_ahb_read_bridge PROPERTIES EXPORT_NAME read_bridge CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
  add_library(MeguvizMemoryAhbReadBridge::read_bridge ALIAS mgmem_ahb_read_bridge)
  install(TARGETS mgmem_ahb_read_bridge EXPORT MeguvizMemoryAhbReadBridgeTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT MeguvizMemoryAhbReadBridgeTargets NAMESPACE MeguvizMemoryAhbReadBridge::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryAhbReadBridge)
  configure_package_config_file(cmake/MeguvizMemoryAhbReadBridgeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryAhbReadBridgeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryAhbReadBridge)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryAhbReadBridgeConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryAhbReadBridgeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryAhbReadBridgeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryAhbReadBridge)
else()
  enable_testing()
  enable_language(C)
  get_target_property(compute_headers MeguvizCompute::core INTERFACE_INCLUDE_DIRECTORIES)
  add_executable(mgmem_ahb_host_tests tests/ahb_host_tests.cpp src/ahb_read.cpp)
  target_compile_features(mgmem_ahb_host_tests PRIVATE cxx_std_17)
  target_compile_definitions(mgmem_ahb_host_tests PRIVATE MGMEM_AHB_TESTING)
  target_include_directories(mgmem_ahb_host_tests PRIVATE tests/fake include ${compute_headers})
  target_link_libraries(mgmem_ahb_host_tests PRIVATE MeguvizMemory::mgmem)
  add_test(NAME ahb_ownership COMMAND mgmem_ahb_host_tests)
  add_test(NAME ahb_slot_reuse COMMAND mgmem_ahb_host_tests --slot)
  add_test(NAME ahb_slot_quarantine COMMAND mgmem_ahb_host_tests --slot-failure)
  add_test(NAME ahb_wait_error_quarantine COMMAND mgmem_ahb_host_tests --wait-error)
  add_test(NAME ahb_slot_wait_error_quarantine COMMAND mgmem_ahb_host_tests --slot-wait-error)
  add_test(NAME ahb_wait_unverified_quarantine COMMAND mgmem_ahb_host_tests --wait-unverified)
  add_test(NAME ahb_slot_wait_unverified_quarantine COMMAND mgmem_ahb_host_tests --slot-wait-unverified)
  add_test(NAME ahb_write COMMAND mgmem_ahb_host_tests --write)
  add_test(NAME ahb_unconfirmed_unlock COMMAND mgmem_ahb_host_tests --unlock-failure)
  add_test(NAME ahb_unconfirmed_return COMMAND mgmem_ahb_host_tests --return-failure)
  add_test(NAME ahb_write_unconfirmed_unlock COMMAND mgmem_ahb_host_tests --write-unlock-failure)
  add_test(NAME ahb_write_unconfirmed_return COMMAND mgmem_ahb_host_tests --write-return-failure)
  set_tests_properties(ahb_ownership ahb_slot_reuse ahb_slot_quarantine ahb_wait_error_quarantine ahb_slot_wait_error_quarantine ahb_wait_unverified_quarantine ahb_slot_wait_unverified_quarantine ahb_write ahb_unconfirmed_unlock ahb_unconfirmed_return
    ahb_write_unconfirmed_unlock ahb_write_unconfirmed_return PROPERTIES TIMEOUT 30)
  add_executable(mgmem_ahb_short_c tests/short_names.c)
  target_include_directories(mgmem_ahb_short_c PRIVATE include)
  target_link_libraries(mgmem_ahb_short_c PRIVATE MeguvizMemory::mgmem)
  add_executable(mgmem_ahb_short_cpp tests/short_names_cpp.cpp)
  target_compile_features(mgmem_ahb_short_cpp PRIVATE cxx_std_17)
  target_include_directories(mgmem_ahb_short_cpp PRIVATE include)
  target_link_libraries(mgmem_ahb_short_cpp PRIVATE MeguvizMemory::mgmem)
  add_test(NAME ahb_short_c COMMAND mgmem_ahb_short_c)
  add_test(NAME ahb_short_cpp COMMAND mgmem_ahb_short_cpp)
endif()
