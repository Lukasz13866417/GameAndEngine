include_guard(GLOBAL)

include(FetchContent)

function(vng_provide_text_dependencies)
    if(TARGET vng_harfbuzz_dependency)
        return()
    endif()
    find_package(Freetype 2.10 QUIET)
    if(NOT TARGET Freetype::Freetype)
        set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
        FetchContent_Declare(freetype
            GIT_REPOSITORY https://github.com/freetype/freetype.git
            GIT_TAG VER-2-13-3 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(freetype)
        if(NOT TARGET Freetype::Freetype)
            add_library(Freetype::Freetype ALIAS freetype)
        endif()
    endif()

    add_library(vng_harfbuzz_dependency INTERFACE)
    find_package(harfbuzz CONFIG QUIET)
    if(TARGET harfbuzz::harfbuzz)
        target_link_libraries(vng_harfbuzz_dependency INTERFACE harfbuzz::harfbuzz)
        return()
    endif()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(VNG_HARFBUZZ QUIET IMPORTED_TARGET harfbuzz>=2.6)
        if(TARGET PkgConfig::VNG_HARFBUZZ)
            target_link_libraries(vng_harfbuzz_dependency INTERFACE PkgConfig::VNG_HARFBUZZ)
            return()
        endif()
    endif()
    set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(harfbuzz
        GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG 11.2.1 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(harfbuzz)
    target_link_libraries(vng_harfbuzz_dependency INTERFACE harfbuzz)
endfunction()

function(vng_provide_glfw)
    if(TARGET vng_glfw_dependency)
        return()
    endif()

    set(VNG_GLFW_SMOOTH_SCROLL FALSE CACHE INTERNAL "VNG's GLFW XI2 scroll adapter" FORCE)

    # GLFW 3.4's X11 path quantizes wheels into core button events. On Linux
    # use our pinned source + small XI2 patch so installed GLFW cannot silently
    # bring back the lost high-resolution input. Other platforms still prefer
    # installed packages; no public window/graphics dependency changes.
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(glfw3 3.4 CONFIG QUIET)
    endif()

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
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/glfw/ApplyPatch.cmake")
            vng_patch_glfw("${glfw_SOURCE_DIR}")
            target_include_directories(glfw PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/glfw")
            set(VNG_GLFW_SMOOTH_SCROLL TRUE CACHE INTERNAL "VNG's GLFW XI2 scroll adapter" FORCE)
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(WARNING "Using a parent-provided GLFW target: VNG's X11 fractional-scroll patch cannot be applied to it")
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
