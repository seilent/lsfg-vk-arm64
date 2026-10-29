set(LAYER_SOURCES
    "src/entrypoint.cpp"
    "src/instance.cpp"
    "src/running_signal.cpp"
    "src/swapchain.cpp")

add_library(lsfg-vk-layer SHARED ${LAYER_SOURCES})

target_link_libraries(lsfg-vk-layer
    PUBLIC lsfg-vk-common
    PUBLIC lsfg-vk-backend)

target_compile_options(lsfg-vk-layer PRIVATE
    -Wno-unknown-warning-option
    -Wno-unsafe-buffer-usage) # Array indexing

target_link_options(lsfg-vk-layer PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/lsfg-vk-layer.map")

set_target_properties(lsfg-vk-layer PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lsfg-vk-layer.map")

set(LSFGVK_LAYER_ENABLE_ENVIRONMENT_JSON "")
if(LSFGVK_LAYER_ENABLE_ENVIRONMENT)
    if(NOT LSFGVK_LAYER_ENABLE_ENVIRONMENT MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "LSFGVK_LAYER_ENABLE_ENVIRONMENT is not a valid variable name")
    endif()
    set(LSFGVK_LAYER_ENABLE_ENVIRONMENT_JSON
        ",\n    \"enable_environment\": {\n      \"${LSFGVK_LAYER_ENABLE_ENVIRONMENT}\": \"1\"\n    }")
endif()

configure_file(
    "VkLayer_LSFGVK_frame_generation.json.in"
    "VkLayer_LSFGVK_frame_generation.json"
    @ONLY)

install(TARGETS lsfg-vk-layer
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_LSFGVK_frame_generation.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d")
