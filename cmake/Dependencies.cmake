include_guard(GLOBAL)

include(FetchContent)

function(vng_provide_glfw)
    if(TARGET vng_glfw_dependency)
        return()
    endif()

    find_package(glfw3 3.4 CONFIG QUIET)

    if(NOT TARGET glfw AND NOT TARGET glfw3::glfw)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(glfw)
    endif()

    add_library(vng_glfw_dependency INTERFACE)
    if(TARGET glfw3::glfw)
        target_link_libraries(vng_glfw_dependency INTERFACE glfw3::glfw)
    elseif(TARGET glfw)
        target_link_libraries(vng_glfw_dependency INTERFACE glfw)
    else()
        message(FATAL_ERROR "A compatible GLFW target was not provided")
    endif()
endfunction()

function(vng_provide_glad)
    if(TARGET vng_glad_dependency)
        return()
    endif()

    find_package(glad 2.0.8 CONFIG QUIET)

    add_library(vng_glad_dependency INTERFACE)
    if(TARGET glad::glad)
        include(CheckCSourceCompiles)
        set(CMAKE_REQUIRED_QUIET TRUE)
        set(CMAKE_REQUIRED_LIBRARIES glad::glad)
        unset(VNG_INSTALLED_GLAD_HAS_GL46_LOADER CACHE)
        check_c_source_compiles([[
            #include <glad/gl.h>
            #ifndef GL_VERSION_4_6
            #error "installed GLAD was not generated for OpenGL 4.6"
            #endif
            #ifdef glBegin
            #error "installed GLAD exposes the compatibility profile"
            #endif

            static GLADloadfunc vng_test_loader;

            int main(void)
            {
                return gladLoadGL(vng_test_loader) == 0;
            }
        ]] VNG_INSTALLED_GLAD_HAS_GL46_LOADER)
        if(NOT VNG_INSTALLED_GLAD_HAS_GL46_LOADER)
            message(FATAL_ERROR
                "The installed glad::glad target does not provide an OpenGL 4.6 core loader")
        endif()
        target_link_libraries(vng_glad_dependency INTERFACE glad::glad)
        return()
    elseif(TARGET glad)
        include(CheckCSourceCompiles)
        set(CMAKE_REQUIRED_QUIET TRUE)
        set(CMAKE_REQUIRED_LIBRARIES glad)
        unset(VNG_INSTALLED_GLAD_HAS_GL46_LOADER CACHE)
        check_c_source_compiles([[
            #include <glad/gl.h>
            #ifndef GL_VERSION_4_6
            #error "installed GLAD was not generated for OpenGL 4.6"
            #endif
            #ifdef glBegin
            #error "installed GLAD exposes the compatibility profile"
            #endif

            static GLADloadfunc vng_test_loader;

            int main(void)
            {
                return gladLoadGL(vng_test_loader) == 0;
            }
        ]] VNG_INSTALLED_GLAD_HAS_GL46_LOADER)
        if(NOT VNG_INSTALLED_GLAD_HAS_GL46_LOADER)
            message(FATAL_ERROR
                "The installed glad target does not provide an OpenGL 4.6 core loader")
        endif()
        target_link_libraries(vng_glad_dependency INTERFACE glad)
        return()
    endif()

    FetchContent_Declare(
        glad
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG v2.0.8
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR cmake
    )
    FetchContent_MakeAvailable(glad)
    glad_add_library(
        vng_glad
        STATIC
        REPRODUCIBLE
        LOADER
        API gl:core=4.6
    )
    target_link_libraries(vng_glad_dependency INTERFACE vng_glad)
endfunction()

function(vng_provide_catch2)
    if(TARGET Catch2::Catch2WithMain)
        return()
    endif()

    find_package(Catch2 3.8.1 CONFIG QUIET)
    if(TARGET Catch2::Catch2WithMain)
        return()
    endif()

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endfunction()
