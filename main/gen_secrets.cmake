# Writes OUT (secrets_env.h) from the optional WIFI_SSID, WIFI_PASS and HA_TOKEN environment
# variables: build-time defaults for the settings the board keeps in NVS (provision.c).
# Unset ones are left out; the setup portal asks for them. Runs on every build; the file
# only changes when a value changes.
set(body "/* Generated from environment variables at build time. Do not edit or commit. */\n#pragma once\n")
foreach(name WIFI_SSID WIFI_PASS HA_TOKEN)
    if(NOT DEFINED ENV{${name}} OR "$ENV{${name}}" STREQUAL "")
        continue()
    endif()
    set(v "$ENV{${name}}")
    string(REPLACE "\\" "\\\\" v "${v}")
    string(REPLACE "\"" "\\\"" v "${v}")
    string(APPEND body "#define ${name} \"${v}\"\n")
endforeach()
file(WRITE "${OUT}.tmp" "${body}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
