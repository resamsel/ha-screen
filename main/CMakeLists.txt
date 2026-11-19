idf_component_register(SRCS "main.c" "ha_client.c" "bg_dark.c" "bg_light.c" "layout.c" "layout_store.c" "dash_fetch.c" "names.c"
                         "fonts/lv_font_latin_12.c" "fonts/lv_font_latin_14.c" "fonts/lv_font_latin_16.c" "fonts/lv_font_latin_22.c"
                         "fonts/lv_font_clock_48.c" "fonts/lv_font_clock_72.c"
                    INCLUDE_DIRS ".")

# Secrets come from environment variables at build time (see gen_secrets.cmake)
set(SECRETS_H "${CMAKE_CURRENT_BINARY_DIR}/secrets_env.h")
add_custom_target(secrets_env
    COMMAND ${CMAKE_COMMAND} -DOUT=${SECRETS_H} -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_secrets.cmake
    BYPRODUCTS ${SECRETS_H}
    COMMENT "Reading secrets from the environment")
add_dependencies(${COMPONENT_LIB} secrets_env)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
