option(PHOSCHAT_REQUIRE_TELEGRAM_APP_CREDENTIALS
    "Require the official Telegram application identity for this build" OFF)
set(PHOSCHAT_TELEGRAM_CREDENTIALS_FILE "" CACHE STRING
    "Private JSON path, relative to the source directory; empty reads the environment")

function(phoschat_configure_telegram_credentials output_directory)
    # Values deliberately stay out of the CMake cache and compiler command line.
    # The generated header is a build input, never an installed source artifact.
    set(PHOSCHAT_TELEGRAM_API_ID "$ENV{PHOSCHAT_TELEGRAM_API_ID}")
    set(PHOSCHAT_TELEGRAM_API_HASH "$ENV{PHOSCHAT_TELEGRAM_API_HASH}")
    if(NOT PHOSCHAT_TELEGRAM_CREDENTIALS_FILE STREQUAL "")
        if(NOT PHOSCHAT_TELEGRAM_API_ID STREQUAL "" OR NOT PHOSCHAT_TELEGRAM_API_HASH STREQUAL "")
            message(FATAL_ERROR "Choose one Telegram credential input: environment or private JSON file.")
        endif()
        get_filename_component(credentials_file "${PHOSCHAT_TELEGRAM_CREDENTIALS_FILE}"
            ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${credentials_file}" OR IS_DIRECTORY "${credentials_file}")
            message(FATAL_ERROR "The private Telegram credential file is unavailable.")
        endif()
        file(SIZE "${credentials_file}" credentials_size)
        if(credentials_size GREATER 4096)
            message(FATAL_ERROR "The private Telegram credential file is invalid.")
        endif()
        file(READ "${credentials_file}" credentials_json)
        string(JSON PHOSCHAT_TELEGRAM_API_ID ERROR_VARIABLE id_error GET "${credentials_json}" api_id)
        string(JSON PHOSCHAT_TELEGRAM_API_HASH ERROR_VARIABLE hash_error GET "${credentials_json}" api_hash)
        string(JSON id_type ERROR_VARIABLE id_type_error TYPE "${credentials_json}" api_id)
        string(JSON hash_type ERROR_VARIABLE hash_type_error TYPE "${credentials_json}" api_hash)
        if(id_error OR hash_error OR id_type_error OR hash_type_error
            OR NOT id_type STREQUAL "STRING" OR NOT hash_type STREQUAL "STRING")
            message(FATAL_ERROR "The private Telegram credential file is invalid.")
        endif()
    endif()

    if(PHOSCHAT_TELEGRAM_API_ID STREQUAL "" AND PHOSCHAT_TELEGRAM_API_HASH STREQUAL "")
        if(PHOSCHAT_REQUIRE_TELEGRAM_APP_CREDENTIALS)
            message(FATAL_ERROR
                "Official builds require TELEGRAM_API_ID and TELEGRAM_API_HASH repository secrets.")
        endif()
        set(PHOSCHAT_TELEGRAM_API_ID 0)
    else()
        string(LENGTH "${PHOSCHAT_TELEGRAM_API_ID}" id_length)
        string(LENGTH "${PHOSCHAT_TELEGRAM_API_HASH}" hash_length)
        if(NOT PHOSCHAT_TELEGRAM_API_ID MATCHES "^[1-9][0-9]*$" OR id_length GREATER 10
            OR NOT PHOSCHAT_TELEGRAM_API_HASH MATCHES "^[0-9a-fA-F]+$" OR NOT hash_length EQUAL 32)
            message(FATAL_ERROR "Telegram application credentials are missing or invalid.")
        endif()
        if(PHOSCHAT_TELEGRAM_API_ID GREATER 2147483647)
            message(FATAL_ERROR "Telegram application credentials are missing or invalid.")
        endif()
    endif()

    file(MAKE_DIRECTORY "${output_directory}")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/telegram_build_credentials.h.in"
        "${output_directory}/telegram_build_credentials.h" @ONLY)
    file(CHMOD "${output_directory}/telegram_build_credentials.h" PERMISSIONS OWNER_READ OWNER_WRITE)
endfunction()
