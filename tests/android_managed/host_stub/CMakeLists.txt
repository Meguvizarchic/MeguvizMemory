cmake_minimum_required(VERSION 3.25)
project(MeguvizManagedAndroidHostProbe LANGUAGES CXX)
find_package(JNI REQUIRED)
find_package(Java REQUIRED COMPONENTS Runtime Development)
find_package(Threads REQUIRED)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
# Single test DLL only: this does not reproduce the Android AAR runtime closure.
add_library(mgmem_pressure_jni SHARED "${ROOT}/src/mgmem.cpp"
  "${ROOT}/adapters/android/native/pressure.cpp" "${ROOT}/adapters/android/native/borrow_jni.cpp"
  "${ROOT}/adapters/android/native/workspace_jni.cpp")
target_compile_features(mgmem_pressure_jni PRIVATE cxx_std_17)
target_include_directories(mgmem_pressure_jni PRIVATE "${ROOT}/include" ${JNI_INCLUDE_DIRS})
target_link_libraries(mgmem_pressure_jni PRIVATE Threads::Threads)
set(JAVA_ROOT "${ROOT}/adapters/android/java/io/github/jichuo1/meguviz/memory/android")
set(STUB "${ROOT}/tests/managed_host/android_stub")
set(BORROW_STUB "${ROOT}/tests/android_borrow/host_stub")
set(CLASSES "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(SOURCES "${CMAKE_CURRENT_LIST_DIR}/android/app/Instrumentation.java"
  "${BORROW_STUB}/android/os/Bundle.java" "${BORROW_STUB}/android/os/Looper.java"
  "${BORROW_STUB}/org/json/JSONObject.java" "${STUB}/android/content/Context.java"
  "${STUB}/android/content/ComponentCallbacks2.java" "${STUB}/android/content/res/Configuration.java"
  "${JAVA_ROOT}/NativeMemoryLease.java" "${JAVA_ROOT}/ManagedMemoryDomain.java"
  "${JAVA_ROOT}/ManagedPressureBackend.java" "${JAVA_ROOT}/MemoryPressureAdapter.java"
  "${ROOT}/tests/android_managed/ManagedInstrumentation.java"
  "${CMAKE_CURRENT_LIST_DIR}/HostProbe.java" "${CMAKE_CURRENT_LIST_DIR}/FaultProbe.java")
add_custom_command(OUTPUT "${CLASSES}/HostProbe.class" "${CLASSES}/FaultProbe.class"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CLASSES}"
  COMMAND ${Java_JAVAC_EXECUTABLE} --release 8 -d "${CLASSES}" ${SOURCES}
  DEPENDS ${SOURCES} VERBATIM)
add_custom_target(managed_probe_classes ALL DEPENDS "${CLASSES}/HostProbe.class" "${CLASSES}/FaultProbe.class")
enable_testing()
add_test(NAME android_managed_host_stub COMMAND ${Java_JAVA_EXECUTABLE}
  "-Djava.library.path=$<TARGET_FILE_DIR:mgmem_pressure_jni>" -cp "${CLASSES}" HostProbe)
add_test(NAME android_managed_fault_host_stub COMMAND ${Java_JAVA_EXECUTABLE} -cp "${CLASSES}" FaultProbe)
add_test(NAME android_managed_copy_arena_host_stub COMMAND ${Java_JAVA_EXECUTABLE}
  "-Djava.library.path=$<TARGET_FILE_DIR:mgmem_pressure_jni>" -cp "${CLASSES}" HostProbe copy-arena-v1)
set_tests_properties(android_managed_host_stub android_managed_fault_host_stub android_managed_copy_arena_host_stub PROPERTIES TIMEOUT 60)
