#SPDX-License-Identifier: Apache-2.0
#
# Auto-apply zephyr_nuvoton fix patches before find_package(Zephyr).
# Shared by the NPCM400 applications: patches common to all of them live
# in meta-nuvoton/common/patches, application-only ones in <app>/patches.

file(GLOB _PATCH_FILES
    "${CMAKE_CURRENT_LIST_DIR}/patches/*.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
list(LENGTH _PATCH_FILES _NUM_PATCHES)

if(_NUM_PATCHES GREATER 0)
    # Determine zephyr_nuvoton path
    if(DEFINED ENV{ZEPHYR_BASE})
        set(_ZEPHYR_DIR "$ENV{ZEPHYR_BASE}")
    else()
        get_filename_component(_ZEPHYR_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../zephyr_nuvoton" ABSOLUTE)
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
