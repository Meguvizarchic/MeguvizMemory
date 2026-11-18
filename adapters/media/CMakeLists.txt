cmake_minimum_required(VERSION 3.25)
project(MeguvizMemoryMediaReadBridge VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(MeguvizMemoryComputeReadBridge CONFIG REQUIRED)
find_package(MeguvizMedia CONFIG REQUIRED)

add_library(mgmem_media_read_bridge STATIC src/media_read_bridge.cpp)
target_compile_features(mgmem_media_read_bridge PUBLIC cxx_std_17)
target_include_directories(mgmem_media_read_bridge PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(mgmem_media_read_bridge PUBLIC MeguvizMemoryComputeReadBridge::read_bridge MeguvizMedia::core)
set_target_properties(mgmem_media_read_bridge PROPERTIES EXPORT_NAME read_bridge CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
add_library(MeguvizMemoryMediaReadBridge::read_bridge ALIAS mgmem_media_read_bridge)

# Older Compute bridges support SYSTEM imports without an Arena target.
if(TARGET MeguvizMemoryComputeReadBridge::arena_read_bridge)
  add_library(mgmem_media_arena_read_bridge STATIC src/media_arena_read_bridge.cpp)
  target_compile_features(mgmem_media_arena_read_bridge PUBLIC cxx_std_17)
  target_link_libraries(mgmem_media_arena_read_bridge PUBLIC mgmem_media_read_bridge MeguvizMemoryComputeReadBridge::arena_read_bridge)
  set_target_properties(mgmem_media_arena_read_bridge PROPERTIES EXPORT_NAME arena_read_bridge CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
  add_library(MeguvizMemoryMediaReadBridge::arena_read_bridge ALIAS mgmem_media_arena_read_bridge)
  install(TARGETS mgmem_media_arena_read_bridge EXPORT MeguvizMemoryMediaReadBridgeTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(TARGETS mgmem_media_read_bridge EXPORT MeguvizMemoryMediaReadBridgeTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT MeguvizMemoryMediaReadBridgeTargets NAMESPACE MeguvizMemoryMediaReadBridge::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryMediaReadBridge)
configure_package_config_file(cmake/MeguvizMemoryMediaReadBridgeConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryMediaReadBridgeConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryMediaReadBridge)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryMediaReadBridgeConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryMediaReadBridgeConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/MeguvizMemoryMediaReadBridgeConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MeguvizMemoryMediaReadBridge)

option(MGMEM_MEDIA_BRIDGE_BUILD_TESTS "Build Media frame bridge tests" ON)
if(MGMEM_MEDIA_BRIDGE_BUILD_TESTS)
  enable_language(C)
  enable_testing()
  add_executable(mgmem_media_read_bridge_tests tests/media_read_bridge_tests.cpp)
  target_compile_features(mgmem_media_read_bridge_tests PRIVATE cxx_std_17)
  if(TARGET mgmem_media_arena_read_bridge)
    target_compile_definitions(mgmem_media_read_bridge_tests PRIVATE MGMEM_MEDIA_TEST_ARENA)
    target_link_libraries(mgmem_media_read_bridge_tests PRIVATE mgmem_media_arena_read_bridge)
  else()
    target_link_libraries(mgmem_media_read_bridge_tests PRIVATE mgmem_media_read_bridge)
  endif()
  add_test(NAME media_read_bridge COMMAND mgmem_media_read_bridge_tests)
  set_tests_properties(media_read_bridge PROPERTIES TIMEOUT 30)
  add_executable(mgmem_media_short_c tests/short_names.c)
  target_compile_features(mgmem_media_short_c PRIVATE c_std_11)
  target_link_libraries(mgmem_media_short_c PRIVATE mgmem_media_read_bridge)
  add_executable(mgmem_media_short_cpp tests/short_names_cpp.cpp)
  target_compile_features(mgmem_media_short_cpp PRIVATE cxx_std_17)
  target_link_libraries(mgmem_media_short_cpp PRIVATE mgmem_media_read_bridge)
  if(TARGET mgmem_media_arena_read_bridge)
    target_compile_definitions(mgmem_media_short_c PRIVATE MGMEM_TEST_SHORT_ARENA)
    target_compile_definitions(mgmem_media_short_cpp PRIVATE MGMEM_TEST_SHORT_ARENA)
  endif()
  add_test(NAME media_short_c COMMAND mgmem_media_short_c)
  add_test(NAME media_short_cpp COMMAND mgmem_media_short_cpp)
  set_tests_properties(media_short_c media_short_cpp PROPERTIES TIMEOUT 30)
endif()
