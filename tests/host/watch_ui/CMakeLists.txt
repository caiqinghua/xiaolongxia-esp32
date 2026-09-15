cmake_minimum_required(VERSION 3.20)
project(audream_watch_ui_host LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(FIRMWARE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
add_subdirectory("${FIRMWARE_ROOT}/managed_components/lvgl__lvgl" lvgl)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(WATCH_TEST_LANGUAGE "en-US" CACHE STRING "Watch UI test locale")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/assets")
file(CREATE_LINK "${FIRMWARE_ROOT}/main/assets/locales" "${CMAKE_CURRENT_BINARY_DIR}/generated/assets/locales" SYMBOLIC)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${FIRMWARE_ROOT}/scripts/gen_lang.py"
    --language "${WATCH_TEST_LANGUAGE}" --strings-only --output "${CMAKE_CURRENT_BINARY_DIR}/generated/assets/lang_config.h"
    WORKING_DIRECTORY "${FIRMWARE_ROOT}"
    COMMAND_ERROR_IS_FATAL ANY)
set(CJSON_SOURCE_DIR "$ENV{CJSON_SOURCE_DIR}" CACHE PATH "Path to local cJSON sources")
if(NOT CJSON_SOURCE_DIR)
    set(CJSON_SOURCE_DIR "$ENV{HOME}/.espressif/v5.5.4/esp-idf/components/json/cJSON")
endif()
add_library(watch_json STATIC "${CJSON_SOURCE_DIR}/cJSON.c")
target_include_directories(watch_json PUBLIC "${CJSON_SOURCE_DIR}")
add_executable(watch_ui_test watch_ui_test.cc
    "${FIRMWARE_ROOT}/managed_components/78__xiaozhi-fonts/src/font_puhui_30_4.c"
    "${FIRMWARE_ROOT}/managed_components/78__xiaozhi-fonts/src/font_awesome_30_4.c"
    "${FIRMWARE_ROOT}/main/watch/watch_view.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_firmware_view.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_recording_library.cc"
    "${FIRMWARE_ROOT}/main/storage/recording_file_deletion.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_activity.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_wrist_control.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_activity_record.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_motion_task.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_assistant_state.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_push_to_talk.cc"
    "${FIRMWARE_ROOT}/main/audio/audio_session_coordinator.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_calendar.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_time_state.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_time_tools.cc"
    "${FIRMWARE_ROOT}/main/watch/watch_time_service.cc")
target_include_directories(watch_ui_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${FIRMWARE_ROOT}/main"
    "${FIRMWARE_ROOT}/managed_components/78__xiaozhi-fonts/include")
target_compile_options(watch_ui_test PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(watch_ui_test PRIVATE lvgl::lvgl watch_json)
