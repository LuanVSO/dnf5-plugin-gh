set(stage "${BUILD_DIR}/test-config-install")
set(config "${stage}${CONFIG_DIR}/gh.conf")
file(REMOVE "${config}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${stage}"
        "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --component plugin_config
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Failed to install plugin configuration: ${result}")
endif()
execute_process(COMMAND stat -c %a "${config}" OUTPUT_VARIABLE mode
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT mode STREQUAL "644")
    message(FATAL_ERROR "Plugin configuration permissions are '${mode}', expected 644")
endif()
