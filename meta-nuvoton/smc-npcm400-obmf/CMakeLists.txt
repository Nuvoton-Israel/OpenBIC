#SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

# --- Auto-apply fix patches to zephyr_nuvoton before build ---
set(_PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/patches")
file(GLOB _PATCH_FILES "${_PATCH_DIR}/*.patch")
list(LENGTH _PATCH_FILES _NUM_PATCHES)

if(_NUM_PATCHES GREATER 0)
    # Determine zephyr_nuvoton path
    if(DEFINED ENV{ZEPHYR_BASE})
        set(_ZEPHYR_DIR "$ENV{ZEPHYR_BASE}")
    else()
        get_filename_component(_ZEPHYR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr_nuvoton" ABSOLUTE)
    endif()

    # Apply each patch only if no existing commit already carries its Subject,
    # so newly added patches still get applied once earlier ones have landed.
    # (Context-based reverse-apply checks are unreliable here because later
    # patches keep touching the same files, e.g. subsys/usb/class/*.)
    if(EXISTS "${_ZEPHYR_DIR}/.git")
        list(SORT _PATCH_FILES)
        message(STATUS "[apply_fix_patches] Checking ${_NUM_PATCHES} patch(es) against ${_ZEPHYR_DIR}...")
        foreach(_PF ${_PATCH_FILES})
            get_filename_component(_PN "${_PF}" NAME)

            execute_process(COMMAND git mailinfo /dev/null /dev/null
                INPUT_FILE "${_PF}"
                WORKING_DIRECTORY "${_ZEPHYR_DIR}"
                OUTPUT_VARIABLE _MAILINFO ERROR_QUIET)
            string(REGEX MATCH "Subject: ([^\n]*)" _ "${_MAILINFO}")
            set(_SUBJECT "${CMAKE_MATCH_1}")

            set(_ALREADY_APPLIED "")
            if(_SUBJECT)
                execute_process(COMMAND git log --oneline "--grep=${_SUBJECT}" --fixed-strings
                    WORKING_DIRECTORY "${_ZEPHYR_DIR}"
                    OUTPUT_VARIABLE _ALREADY_APPLIED OUTPUT_STRIP_TRAILING_WHITESPACE)
            endif()

            if(NOT _ALREADY_APPLIED STREQUAL "")
                message(STATUS "[apply_fix_patches]   ${_PN} (already applied, skip)")
                continue()
            endif()

            message(STATUS "[apply_fix_patches]   ${_PN}")
            execute_process(COMMAND git am "${_PF}"
                WORKING_DIRECTORY "${_ZEPHYR_DIR}"
                RESULT_VARIABLE _RES ERROR_VARIABLE _ERR)
            if(NOT _RES EQUAL 0)
                execute_process(COMMAND git am --abort
                    WORKING_DIRECTORY "${_ZEPHYR_DIR}" ERROR_QUIET)
                message(FATAL_ERROR "[apply_fix_patches] Failed: ${_PN}\n${_ERR}")
            endif()
        endforeach()
        message(STATUS "[apply_fix_patches] All patches up to date!")
    endif()
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smc-npcm400-obmf)

set(common_path ../../common)
set(nuvoton_common_path ../common)

FILE(GLOB app_sources CONFIGURE_DEPENDS src/ipmi/*.c src/platform/*.c src/lib/*.c src/shell/*.c)
FILE(GLOB common_sources CONFIGURE_DEPENDS ${common_path}/service/*.c ${common_path}/service/*/*.c ${common_path}/hal/*.c ${common_path}/shell/*.c ${common_path}/shell/commands/*.c)
# No IPMI or sensor service on this platform
list(FILTER common_sources EXCLUDE REGEX "/service/ipmi/")
list(FILTER common_sources EXCLUDE REGEX "/service/sensor/")
list(FILTER common_sources EXCLUDE REGEX "/shell/commands/sensor_shell\\.c$")
FILE(GLOB nuvoton_common_sources CONFIGURE_DEPENDS ${nuvoton_common_path}/service/*/*.c)

target_sources(app PRIVATE ${common_path}/dev/snoop_npcm.c)
target_sources(app PRIVATE ${common_path}/dev/fru.c)
target_sources(app PRIVATE ${common_path}/dev/eeprom.c)
target_sources(app PRIVATE ${common_path}/dev/guid.c)
target_sources(app PRIVATE ${app_sources})
target_sources(app PRIVATE ${common_sources})
target_sources(app PRIVATE ${nuvoton_common_sources})

# Common Lib
target_sources(app PRIVATE ${common_path}/lib/libutil.c)
target_sources(app PRIVATE ${common_path}/lib/power_status.c)
target_sources(app PRIVATE ${common_path}/lib/timer.c)
target_sources(app PRIVATE ${common_path}/lib/util_pmbus.c)
target_sources(app PRIVATE ${common_path}/lib/util_spi.c)
target_sources(app PRIVATE ${common_path}/lib/util_sys.c)
target_sources(app PRIVATE ${common_path}/lib/util_worker.c)

target_include_directories(app PRIVATE ${ZEPHYR_BASE}/include/portability)

# Common Includes
target_include_directories(app PRIVATE ${common_path})
target_include_directories(app PRIVATE ${common_path}/include)
target_include_directories(app PRIVATE ${common_path}/dev/include)
target_include_directories(app PRIVATE ${common_path}/hal)
target_include_directories(app PRIVATE ${common_path}/lib)
target_include_directories(app PRIVATE ${common_path}/service/host)
target_include_directories(app PRIVATE ${common_path}/service/ipmb)
target_include_directories(app PRIVATE ${common_path}/service/ipmi/include)
target_include_directories(app PRIVATE ${common_path}/service/logging)
target_include_directories(app PRIVATE ${common_path}/service/mctp)
target_include_directories(app PRIVATE ${common_path}/service/pldm)
target_include_directories(app PRIVATE ${common_path}/service/cci)
target_include_directories(app PRIVATE ${common_path}/service/sensor)
target_include_directories(app PRIVATE ${common_path}/service/usb)
target_include_directories(app PRIVATE ${common_path}/shell/commands)
target_include_directories(app PRIVATE ${common_path}/shell)

# Nuvoton Common Includes
target_include_directories(app PRIVATE ${nuvoton_common_path}/service/obmf)

# Platform Specific Includes
target_include_directories(app PRIVATE src/lib)
target_include_directories(app PRIVATE src/platform)

# Fail build if there are any warnings
target_compile_options(app PRIVATE -Werror)
