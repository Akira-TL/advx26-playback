##
# @file CMakeLists.txt
# @brief TuyaOpen application target for the MOB custom display.
#

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

aux_source_directory(${APP_PATH}/src APP_SRC)
set(APP_INC ${APP_PATH}/include)

set(TINYH264_UPSTREAM_DIR ${APP_PATH}/third_party/tinyh264)
set(TINYH264_PATCH_FILE ${APP_PATH}/third_party/patches/tinyh264-embedded.patch)
set(TINYH264_BUILD_ROOT ${CMAKE_CURRENT_BINARY_DIR}/tinyh264-patched)
set(TINYH264_BUILD_NATIVE ${TINYH264_BUILD_ROOT}/native)

if(NOT EXISTS ${TINYH264_UPSTREAM_DIR}/native/h264bsd_decoder.c)
    message(FATAL_ERROR "tinyh264 submodule is missing; initialize submodules recursively")
endif()

find_program(PATCH_EXECUTABLE patch REQUIRED)
file(GLOB TINYH264_UPSTREAM_FILES CONFIGURE_DEPENDS
    ${TINYH264_UPSTREAM_DIR}/native/*.c
    ${TINYH264_UPSTREAM_DIR}/native/*.h
)
file(REMOVE_RECURSE ${TINYH264_BUILD_ROOT})
file(MAKE_DIRECTORY ${TINYH264_BUILD_ROOT})
file(COPY ${TINYH264_UPSTREAM_DIR}/native DESTINATION ${TINYH264_BUILD_ROOT})
execute_process(
    COMMAND ${PATCH_EXECUTABLE} --batch --forward -p1 -d ${TINYH264_BUILD_ROOT} -i ${TINYH264_PATCH_FILE}
    RESULT_VARIABLE TINYH264_PATCH_RESULT
    OUTPUT_VARIABLE TINYH264_PATCH_OUTPUT
    ERROR_VARIABLE TINYH264_PATCH_ERROR
)
if(NOT TINYH264_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR
        "failed to prepare patched tinyh264 sources:\n${TINYH264_PATCH_OUTPUT}${TINYH264_PATCH_ERROR}"
    )
endif()

file(GLOB TINYH264_SRC ${TINYH264_BUILD_NATIVE}/*.c)
list(APPEND APP_SRC ${TINYH264_SRC})
set_source_files_properties(${TINYH264_SRC} PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")

set(APP_OPTIONS "-W")
list(APPEND APP_OPTIONS "-Wall" "-Wextra" "-DLV_LVGL_H_INCLUDE_SIMPLE")

add_library(${EXAMPLE_LIB})
message(STATUS "EXAMPLE_LIB:${APP_PATH}")

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRC}
)

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
        ${APP_PATH}/src/internal
        ${TINYH264_BUILD_NATIVE}
)

target_compile_options(${EXAMPLE_LIB}
    PRIVATE
        ${APP_OPTIONS}
)
