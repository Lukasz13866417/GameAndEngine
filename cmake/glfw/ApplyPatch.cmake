function(vng_patch_glfw source)
    find_package(Git REQUIRED)
    set(patch "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/smooth-scroll.patch")
    # Idempotent on reconfigure. Refuse unknown/locally modified source instead
    # of overwriting it or silently building an unpatched input backend.
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${source}" RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
    if(applied EQUAL 0)
        return()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${patch}"
        WORKING_DIRECTORY "${source}" RESULT_VARIABLE compatible OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT compatible EQUAL 0)
        message(FATAL_ERROR "VNG's smooth-scroll patch requires clean GLFW 3.4 sources: ${error}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
        WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
endfunction()
