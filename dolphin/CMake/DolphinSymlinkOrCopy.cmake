if(NOT DEFINED target OR NOT DEFINED link)
    message(FATAL_ERROR "`target` and `link` must be defined")
endif()

# Check if force copy mode is enabled
if(DOLPHIN_FORCE_COPY_DATA)
    # Always use copying instead of symlinks
    message(STATUS "Copying ${target} to ${link} (DOLPHIN_FORCE_COPY_DATA=ON)")

    file(REMOVE_RECURSE "${link}")
    if(IS_DIRECTORY "${target}")
        file(COPY "${target}/." DESTINATION "${link}")
    else()
        file(COPY_FILE "${target}" "${link}" ONLY_IF_DIFFERENT)
    endif()
else()
    # Try to create symlink, fall back to copying if it fails
    file(CREATE_LINK "${target}" "${link}" RESULT result SYMBOLIC)
    if(result EQUAL 0)
        message(STATUS "Symlinked ${target} to ${link}")
        return()
    endif()

    if(WIN32)
        set(hint "On Windows, enable Developer Mode to allow symlinks (https://learn.microsoft.com/windows/advanced-settings/developer-mode).")
    endif()
    message(WARNING "Symlink failed (${result}). Falling back to copying. ${hint}")

    file(REMOVE_RECURSE "${link}")
    if(IS_DIRECTORY "${target}")
        file(COPY "${target}/." DESTINATION "${link}")
    else()
        file(COPY_FILE "${target}" "${link}" ONLY_IF_DIFFERENT)
    endif()
endif()
