cmake_minimum_required(VERSION 3.25)
project(MeguvizMemoryJniCost LANGUAGES CXX)
find_package(JNI REQUIRED)
find_package(Java REQUIRED COMPONENTS Runtime Development)
find_package(Threads REQUIRED)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Plain host JNI build: same single-DLL shape as tests/managed_host's
# mgmem_managed_plain (no MGMEM_MANAGED_TESTING hooks). Production code is
# compiled read-only from the tree; nothing here modifies it.
add_library(mgmem_pressure_jni SHARED "${ROOT}/src/mgmem.cpp"
  "${ROOT}/adapters/android/native/pressure.cpp"
  "${ROOT}/adapters/android/native/borrow_jni.cpp"
  "${ROOT}/adapters/android/native/workspace_jni.cpp")
target_compile_features(mgmem_pressure_jni PRIVATE cxx_std_17)
target_include_directories(mgmem_pressure_jni PRIVATE "${ROOT}/include" ${JNI_INCLUDE_DIRS})
target_link_libraries(mgmem_pressure_jni PRIVATE Threads::Threads)
if(MSVC)
  target_compile_options(mgmem_pressure_jni PRIVATE /W4 /permissive- /utf-8 /O2)
else()
  target_compile_options(mgmem_pressure_jni PRIVATE -Wall -Wextra -Wpedantic -O2)
endif()

set(JAVA_CLASSES "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(JAVA_SOURCE "${ROOT}/adapters/android/java/io/github/jichuo1/meguviz/memory/android")
set(BENCH_JAVA "${CMAKE_CURRENT_LIST_DIR}/JniCostBench.java")
add_custom_command(OUTPUT "${JAVA_CLASSES}/io/github/jichuo1/meguviz/memory/android/JniCostBench.class"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${JAVA_CLASSES}"
  COMMAND ${Java_JAVAC_EXECUTABLE} --release 8 -d "${JAVA_CLASSES}"
    "${JAVA_SOURCE}/NativeMemoryLease.java" "${JAVA_SOURCE}/ManagedMemoryDomain.java" "${BENCH_JAVA}"
  DEPENDS "${JAVA_SOURCE}/NativeMemoryLease.java" "${JAVA_SOURCE}/ManagedMemoryDomain.java" "${BENCH_JAVA}"
  VERBATIM)
add_custom_target(jni_cost_classes ALL DEPENDS
  "${JAVA_CLASSES}/io/github/jichuo1/meguviz/memory/android/JniCostBench.class")

add_custom_target(run_jni_cost
  COMMAND ${Java_JAVA_EXECUTABLE} -Xmx512m
    "-Djava.library.path=$<TARGET_FILE_DIR:mgmem_pressure_jni>" -cp "${JAVA_CLASSES}"
    io.github.jichuo1.meguviz.memory.android.JniCostBench
  DEPENDS mgmem_pressure_jni jni_cost_classes VERBATIM)
