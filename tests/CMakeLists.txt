cmake_minimum_required(VERSION 3.13)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(LSFGVK_TEST_FIXTURES "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(LSFGVK_TEST_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/scratch")
set(LSFGVK_TEST_DLLS
    truncated no-resources no-rcdata missing-id not-spirv short-blob
    odd-blob bad-fp16 no-fp32 no-fp16 no-perf bad-entry)
list(TRANSFORM LSFGVK_TEST_DLLS PREPEND "${LSFGVK_TEST_FIXTURES}/")
list(TRANSFORM LSFGVK_TEST_DLLS APPEND ".dll")

add_executable(lsfg-vk-shader-ids "shader_ids.cpp")
target_link_libraries(lsfg-vk-shader-ids PRIVATE lsfg-vk-common)

add_custom_command(
    OUTPUT "${LSFGVK_TEST_FIXTURES}/good.dll"
    BYPRODUCTS ${LSFGVK_TEST_DLLS} "${CMAKE_CURRENT_BINARY_DIR}/shader_ids.json"
    COMMAND lsfg-vk-shader-ids "${CMAKE_CURRENT_BINARY_DIR}/shader_ids.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make-test-dll.py"
        "${CMAKE_CURRENT_BINARY_DIR}/shader_ids.json" "${LSFGVK_TEST_FIXTURES}"
    DEPENDS lsfg-vk-shader-ids "make-test-dll.py"
    VERBATIM)
add_custom_target(lsfg-vk-test-fixtures ALL
    DEPENDS "${LSFGVK_TEST_FIXTURES}/good.dll")

add_executable(lsfg-vk-config-test "config.cpp")
target_include_directories(lsfg-vk-config-test
    PRIVATE "../lsfg-vk-backend/src")
target_link_libraries(lsfg-vk-config-test PRIVATE lsfg-vk-backend lsfg-vk-common)
add_dependencies(lsfg-vk-config-test lsfg-vk-test-fixtures)

add_executable(lsfg-vk-running-signal-test
    "running_signal.cpp"
    "../lsfg-vk-layer/src/running_signal.cpp")
target_include_directories(lsfg-vk-running-signal-test
    PRIVATE "../lsfg-vk-layer/src")
target_link_libraries(lsfg-vk-running-signal-test PRIVATE lsfg-vk-common)
target_link_options(lsfg-vk-running-signal-test
    PRIVATE "LINKER:--wrap=openat" "LINKER:--wrap=flock")

set_target_properties(lsfg-vk-shader-ids lsfg-vk-config-test lsfg-vk-running-signal-test PROPERTIES
    CXX_CLANG_TIDY "")

add_test(NAME lsfg-vk-config-test
    COMMAND lsfg-vk-config-test "${LSFGVK_TEST_FIXTURES}" "${LSFGVK_TEST_SCRATCH}/config")
add_test(NAME lsfg-vk-running-signal-test
    COMMAND lsfg-vk-running-signal-test "${LSFGVK_TEST_SCRATCH}/running-signal")
add_test(NAME lsfg-vk-manifest-test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/manifest.py"
        "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}" "${CMAKE_CXX_COMPILER}"
        "${PROJECT_SOURCE_DIR}" "${LSFGVK_TEST_SCRATCH}/manifest")
set_tests_properties(lsfg-vk-config-test lsfg-vk-running-signal-test lsfg-vk-manifest-test PROPERTIES
    TIMEOUT 120)

if(TARGET lsfg-vk-layer)
    add_test(NAME lsfg-vk-layer-exports-test
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/check-layer-exports.sh" "$<TARGET_FILE:lsfg-vk-layer>" "${CMAKE_NM}")
endif()

find_program(LSFGVK_LOCALEDEF localedef)
if(LSFGVK_LOCALEDEF AND EXISTS "/usr/share/i18n/locales/de_DE")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/locale")
    add_test(NAME lsfg-vk-locale-setup
        COMMAND "${LSFGVK_LOCALEDEF}" -i de_DE -f UTF-8 "${CMAKE_CURRENT_BINARY_DIR}/locale/de_DE.UTF-8")
    add_test(NAME lsfg-vk-config-locale-test
        COMMAND lsfg-vk-config-test "${LSFGVK_TEST_FIXTURES}" "${LSFGVK_TEST_SCRATCH}/config-locale" de_DE.UTF-8)
    set_tests_properties(lsfg-vk-locale-setup PROPERTIES
        FIXTURES_SETUP lsfg-vk-locale)
    set_tests_properties(lsfg-vk-config-locale-test PROPERTIES
        FIXTURES_REQUIRED lsfg-vk-locale
        ENVIRONMENT "LOCPATH=${CMAKE_CURRENT_BINARY_DIR}/locale"
        TIMEOUT 120)
else()
    message(WARNING "lsfg-vk: no localedef or de_DE locale source, lsfg-vk-config-locale-test is not registered")
endif()
