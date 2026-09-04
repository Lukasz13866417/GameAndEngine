cmake_minimum_required(VERSION 3.25)

project(VibeEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VNG_BUILD_OPENGL "Build the OpenGL 4.6 backend" ON)
option(VNG_BUILD_WINDOW_GLFW "Build the GLFW window-system backend" ON)
option(VNG_BUILD_TESTS "Build Vibe Engine tests" ON)
option(VNG_BUILD_EXAMPLES "Build Vibe Engine examples" ON)
option(VNG_ENABLE_SANITIZERS "Enable address and undefined-behaviour sanitizers" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

function(vng_target_defaults target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_23)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        if(VNG_ENABLE_SANITIZERS)
            target_compile_options(${target_name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

add_library(vng_core INTERFACE)
add_library(vng::core ALIAS vng_core)
target_include_directories(vng_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(vng_core INTERFACE cxx_std_23)

add_library(vng_gfx INTERFACE)
add_library(vng::gfx ALIAS vng_gfx)
target_link_libraries(vng_gfx INTERFACE vng_core)

add_library(vng_analysis INTERFACE)
add_library(vng::analysis ALIAS vng_analysis)
target_link_libraries(vng_analysis INTERFACE vng_gfx)

add_library(vng_render INTERFACE)
add_library(vng::render ALIAS vng_render)
target_link_libraries(vng_render INTERFACE vng_gfx)

file(GLOB VNG_CONTENT_SOURCES CONFIGURE_DEPENDS src/content/*.cpp)
add_library(vng_content STATIC ${VNG_CONTENT_SOURCES})
add_library(vng::content ALIAS vng_content)
target_link_libraries(vng_content PUBLIC vng_gfx)
target_include_directories(vng_content PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
vng_target_defaults(vng_content)

file(GLOB VNG_SHADER_SOURCES CONFIGURE_DEPENDS src/shader/*.cpp)
add_library(vng_shader STATIC ${VNG_SHADER_SOURCES})
add_library(vng::shader ALIAS vng_shader)
target_link_libraries(vng_shader PUBLIC vng_gfx)
target_include_directories(vng_shader PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
vng_target_defaults(vng_shader)

# Shader compilation belongs to backend integrations and concrete renderers;
# the backend-neutral Renderer<Ticket> core remains independent of shaders.

file(GLOB VNG_GLSL_SOURCES CONFIGURE_DEPENDS src/glsl/*.cpp)
if(VNG_GLSL_SOURCES)
    add_library(vng_glsl STATIC ${VNG_GLSL_SOURCES})
else()
    add_library(vng_glsl INTERFACE)
endif()
add_library(vng::glsl ALIAS vng_glsl)
if(VNG_GLSL_SOURCES)
    target_link_libraries(vng_glsl PUBLIC vng_shader)
    target_include_directories(vng_glsl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    vng_target_defaults(vng_glsl)
else()
    target_link_libraries(vng_glsl INTERFACE vng_shader)
endif()

if(VNG_BUILD_OPENGL)
    vng_provide_glad()

    file(GLOB VNG_OPENGL_SOURCES CONFIGURE_DEPENDS src/opengl/*.cpp)
    add_library(vng_opengl STATIC ${VNG_OPENGL_SOURCES})
    add_library(vng::opengl ALIAS vng_opengl)
    target_include_directories(vng_opengl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(vng_opengl
        PUBLIC vng_gfx vng_render vng_glsl
        PRIVATE vng_glad_dependency)
    vng_target_defaults(vng_opengl)

    file(GLOB VNG_RENDER_OPENGL_SOURCES CONFIGURE_DEPENDS src/render/*.cpp)
    add_library(vng_render_opengl STATIC ${VNG_RENDER_OPENGL_SOURCES})
    add_library(vng::render_opengl ALIAS vng_render_opengl)
    target_include_directories(vng_render_opengl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(vng_render_opengl PUBLIC
        vng_render vng_analysis vng_glsl vng_opengl)
    vng_target_defaults(vng_render_opengl)
endif()

if(VNG_BUILD_WINDOW_GLFW)
    vng_provide_glfw()
    file(GLOB VNG_GLFW_SOURCES CONFIGURE_DEPENDS src/window/*.cpp)
    add_library(vng_window_glfw STATIC ${VNG_GLFW_SOURCES})
    add_library(vng::window_glfw ALIAS vng_window_glfw)
    target_include_directories(vng_window_glfw PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(vng_window_glfw PUBLIC vng_core PRIVATE vng_glfw_dependency)
    vng_target_defaults(vng_window_glfw)
endif()

if(VNG_BUILD_OPENGL AND VNG_BUILD_WINDOW_GLFW)
    add_library(vng_glfw_opengl STATIC src/integration/glfw_opengl.cpp)
    add_library(vng::glfw_opengl ALIAS vng_glfw_opengl)
    target_include_directories(vng_glfw_opengl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(vng_glfw_opengl PUBLIC
        vng_opengl vng_window_glfw PRIVATE vng_glfw_dependency)
    vng_target_defaults(vng_glfw_opengl)
endif()

if(VNG_BUILD_TESTS)
    enable_testing()
    vng_provide_catch2()

    add_executable(vng_gfx_tests
        tests/gfx/camera_tests.cpp
        tests/gfx/gfx_tests.cpp
        tests/gfx/mesh_tests.cpp)
    target_link_libraries(vng_gfx_tests PRIVATE vng_gfx Catch2::Catch2WithMain)
    vng_target_defaults(vng_gfx_tests)
    add_test(NAME vng_gfx_tests COMMAND vng_gfx_tests)

    add_executable(vng_render_tests
        tests/render/pipeline_tests.cpp
        tests/render/renderer_tests.cpp)
    target_link_libraries(vng_render_tests PRIVATE
        vng_render Catch2::Catch2WithMain)
    vng_target_defaults(vng_render_tests)
    add_test(NAME vng_render_tests COMMAND vng_render_tests)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis/analysis_tests.cpp")
        add_executable(vng_analysis_tests
            tests/analysis/analysis_tests.cpp
            tests/analysis/diagnosis_tests.cpp
            tests/analysis/evidence_tests.cpp)
        target_link_libraries(vng_analysis_tests PRIVATE
            vng_analysis Catch2::Catch2WithMain)
        vng_target_defaults(vng_analysis_tests)
        add_test(NAME vng_analysis_tests COMMAND vng_analysis_tests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/content/content_tests.cpp")
        add_executable(vng_content_tests tests/content/content_tests.cpp)
        target_link_libraries(vng_content_tests PRIVATE
            vng_content Catch2::Catch2WithMain)
        vng_target_defaults(vng_content_tests)
        add_test(NAME vng_content_tests COMMAND vng_content_tests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/shader/shader_tests.cpp")
        add_executable(vng_shader_tests tests/shader/shader_tests.cpp)
        target_link_libraries(vng_shader_tests PRIVATE vng_shader Catch2::Catch2WithMain)
        vng_target_defaults(vng_shader_tests)
        add_test(NAME vng_shader_tests COMMAND vng_shader_tests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/glsl/glsl_tests.cpp")
        add_executable(vng_glsl_tests tests/glsl/glsl_tests.cpp)
        target_link_libraries(vng_glsl_tests PRIVATE vng_glsl Catch2::Catch2WithMain)
        vng_target_defaults(vng_glsl_tests)
        add_test(NAME vng_glsl_tests COMMAND vng_glsl_tests)
    endif()

    if(VNG_BUILD_WINDOW_GLFW AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/window/glfw_tests.cpp")
        add_executable(vng_window_glfw_tests tests/window/glfw_tests.cpp)
        target_link_libraries(vng_window_glfw_tests PRIVATE
            vng_window_glfw vng_glfw_dependency Catch2::Catch2WithMain)
        vng_target_defaults(vng_window_glfw_tests)
        add_test(NAME vng_window_glfw_tests COMMAND vng_window_glfw_tests)
        set_tests_properties(vng_window_glfw_tests PROPERTIES
            SKIP_RETURN_CODE 77 LABELS window)
    endif()

    if(VNG_BUILD_OPENGL AND VNG_BUILD_WINDOW_GLFW AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/opengl/opengl_tests.cpp")
        add_executable(vng_glfw_opengl_tests
            tests/window/glfw_opengl_tests.cpp)
        target_link_libraries(vng_glfw_opengl_tests PRIVATE
            vng_glfw_opengl Catch2::Catch2WithMain)
        vng_target_defaults(vng_glfw_opengl_tests)
        add_test(NAME vng_glfw_opengl_tests COMMAND vng_glfw_opengl_tests)
        set_tests_properties(vng_glfw_opengl_tests PROPERTIES
            SKIP_RETURN_CODE 77 LABELS "window;opengl")

        add_executable(vng_opengl_tests
            tests/opengl/commands_tests.cpp
            tests/opengl/diagnostic_tests.cpp
            tests/opengl/frame_tests.cpp
            tests/opengl/graphics_pipeline_tests.cpp
            tests/opengl/gpu_mesh_tests.cpp
            tests/opengl/opengl_tests.cpp
            tests/opengl/shader_driver_tests.cpp)
        target_link_libraries(vng_opengl_tests PRIVATE
            vng_opengl vng_glfw_opengl vng_glsl Catch2::Catch2WithMain)
        vng_target_defaults(vng_opengl_tests)
        add_test(NAME vng_opengl_tests COMMAND vng_opengl_tests)
        set_tests_properties(vng_opengl_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS opengl)

        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/render/opengl_program_runtime_tests.cpp")
            add_executable(vng_render_opengl_tests
                tests/render/opengl_program_runtime_tests.cpp)
            target_link_libraries(vng_render_opengl_tests PRIVATE
                vng_render_opengl vng_glfw_opengl Catch2::Catch2WithMain)
            vng_target_defaults(vng_render_opengl_tests)
            add_test(NAME vng_render_opengl_tests COMMAND vng_render_opengl_tests)
            set_tests_properties(vng_render_opengl_tests PROPERTIES
                SKIP_RETURN_CODE 77 LABELS opengl)
        endif()
    endif()

    function(vng_add_compile_failure_test test_name source_file expected_diagnostic)
        add_test(
            NAME ${test_name}
            COMMAND ${CMAKE_COMMAND}
                -DVNG_CXX=${CMAKE_CXX_COMPILER}
                -DVNG_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
                -DVNG_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/${source_file}
                -DVNG_EXPECT=${expected_diagnostic}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/run.cmake
        )
        set_tests_properties(${test_name} PROPERTIES LABELS compile-fail)
    endfunction()

    vng_add_compile_failure_test(
        vng_compile_fail_duplicate_record
        tests/compile_fail/duplicate_record.cpp
        "A semantic may occur only once in a Record")
    vng_add_compile_failure_test(
        vng_compile_fail_duplicate_layout
        tests/compile_fail/duplicate_layout.cpp
        "A semantic may occur only once across a VertexLayout")
    vng_add_compile_failure_test(
        vng_compile_fail_duplicate_mesh_semantic
        tests/compile_fail/duplicate_mesh_semantic.cpp
        "A semantic may occur only once across a Mesh")
    vng_add_compile_failure_test(
        vng_compile_fail_incomplete_vmesh_schema
        tests/compile_fail/incomplete_vmesh_schema.cpp
        "A vmesh schema must map every Record semantic exactly once")
    vng_add_compile_failure_test(
        vng_compile_fail_foreign_vmesh_schema_semantic
        tests/compile_fail/foreign_vmesh_schema_semantic.cpp
        "A vmesh schema field must name a semantic in its Record")
    vng_add_compile_failure_test(
        vng_compile_fail_foreign_vmesh_schema_builder_semantic
        tests/compile_fail/foreign_vmesh_schema_builder_semantic.cpp
        "A vmesh schema field must name a semantic in its Record")
    vng_add_compile_failure_test(
        vng_compile_fail_incompatible_codec
        tests/compile_fail/incompatible_codec.cpp
        "logical value type")
    vng_add_compile_failure_test(
        vng_compile_fail_missing_layout_semantic
        tests/compile_fail/missing_layout_semantic.cpp
        "resolve_vertex_input")
    vng_add_compile_failure_test(
        vng_compile_fail_gpu_mesh_missing_semantic
        tests/compile_fail/gpu_mesh_missing_semantic.cpp
        "GpuMesh vertex records do not satisfy the requested shader::VertexInputs")
    vng_add_compile_failure_test(
        vng_compile_fail_illegal_expression
        tests/compile_fail/illegal_expression.cpp
        "operator+")
    vng_add_compile_failure_test(
        vng_compile_fail_wrong_stage_result
        tests/compile_fail/wrong_stage_result.cpp
        "complete Outputs schema")
    vng_add_compile_failure_test(
        vng_compile_fail_direct_program_construction
        tests/compile_fail/direct_program_construction.cpp
        "private")
    vng_add_compile_failure_test(
        vng_compile_fail_inconsistent_codec_format
        tests/compile_fail/inconsistent_codec_format.cpp
        "VertexFormat is incompatible")
    vng_add_compile_failure_test(
        vng_compile_fail_boolean_vertex_attribute
        tests/compile_fail/boolean_vertex_attribute.cpp
        "VertexFormat is incompatible")
    vng_add_compile_failure_test(
        vng_compile_fail_matrix_vertex_attribute
        tests/compile_fail/matrix_vertex_attribute.cpp
        "VertexFormat is incompatible")
    vng_add_compile_failure_test(
        vng_compile_fail_record_vertex_attribute
        tests/compile_fail/record_vertex_attribute.cpp
        "VertexFormat is incompatible")
    vng_add_compile_failure_test(
        vng_compile_fail_positional_stage_output
        tests/compile_fail/positional_stage_output.cpp
        "NamedFieldExpression")
    vng_add_compile_failure_test(
        vng_compile_fail_missing_stage_output
        tests/compile_fail/missing_stage_output.cpp
        "dsl::make must initialize every record field exactly once")
    vng_add_compile_failure_test(
        vng_compile_fail_duplicate_stage_output
        tests/compile_fail/duplicate_stage_output.cpp
        "dsl::make fields must have unique semantics")
    vng_add_compile_failure_test(
        vng_compile_fail_unknown_stage_output
        tests/compile_fail/unknown_stage_output.cpp
        "dsl::make is missing a required semantic or contains an unknown semantic")
    vng_add_compile_failure_test(
        vng_compile_fail_wrong_stage_output_type
        tests/compile_fail/wrong_stage_output_type.cpp
        "dsl::make field has the wrong logical type")
    vng_add_compile_failure_test(
        vng_compile_fail_undeclared_stage_input
        tests/compile_fail/undeclared_stage_input.cpp
        "RecordContains")
endif()

if(VNG_BUILD_EXAMPLES AND VNG_BUILD_OPENGL AND VNG_BUILD_WINDOW_GLFW AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle.cpp")
    set(VNG_EXAMPLE_ASSET_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/examples/assets")
    file(MAKE_DIRECTORY "${VNG_EXAMPLE_ASSET_DIR}")
    configure_file(
        examples/assets/colored_triangle.vmesh
        "${VNG_EXAMPLE_ASSET_DIR}/colored_triangle.vmesh"
        COPYONLY)
    configure_file(
        examples/assets/colored_cube.vmesh
        "${VNG_EXAMPLE_ASSET_DIR}/colored_cube.vmesh"
        COPYONLY)

    # Example-only presentation glue. Engine-facing operations deliberately
    # remain in each demo; this target owns CLI, diagnostics, startup, and
    # native window-loop repetition so those operations do not obscure them.
    add_library(vng_example_support STATIC
        examples/support/diagnostics.cpp
        examples/support/glfw_opengl_session.cpp
        examples/support/inspection.cpp
        examples/support/options.cpp
        examples/support/window_loop.cpp)
    target_include_directories(vng_example_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    target_link_libraries(vng_example_support PUBLIC vng_glfw_opengl)
    vng_target_defaults(vng_example_support)

    add_executable(vng_triangle_demo examples/triangle.cpp)
    target_link_libraries(vng_triangle_demo PRIVATE
        vng_content vng_render_opengl vng_glfw_opengl vng_example_support)
    target_compile_definitions(vng_triangle_demo PRIVATE
        VNG_EXAMPLE_TRIANGLE_MESH_PATH="${VNG_EXAMPLE_ASSET_DIR}/colored_triangle.vmesh")
    vng_target_defaults(vng_triangle_demo)

    add_executable(vng_advanced_instanced_streams_demo
        examples/advanced_instanced_streams.cpp)
    target_link_libraries(vng_advanced_instanced_streams_demo PRIVATE
        vng_shader vng_glsl vng_opengl vng_glfw_opengl vng_example_support)
    vng_target_defaults(vng_advanced_instanced_streams_demo)

    add_executable(vng_file_mesh_demo
        examples/file_mesh.cpp
        examples/file_mesh_renderer.cpp)
    target_link_libraries(vng_file_mesh_demo PRIVATE
        vng_content vng_render_opengl vng_glfw_opengl vng_example_support)
    target_compile_definitions(vng_file_mesh_demo PRIVATE
        VNG_EXAMPLE_MESH_PATH="${VNG_EXAMPLE_ASSET_DIR}/colored_cube.vmesh")
    vng_target_defaults(vng_file_mesh_demo)

    add_executable(vng_file_mesh_direct_demo
        examples/file_mesh_direct.cpp)
    target_link_libraries(vng_file_mesh_direct_demo PRIVATE
        vng_content vng_opengl vng_glfw_opengl vng_example_support)
    target_compile_definitions(vng_file_mesh_direct_demo PRIVATE
        VNG_EXAMPLE_MESH_PATH="${VNG_EXAMPLE_ASSET_DIR}/colored_cube.vmesh")
    vng_target_defaults(vng_file_mesh_direct_demo)
endif()
