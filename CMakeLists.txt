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
option(VNG_BUILD_TEXT "Build font shaping and text rendering" ON)
option(VNG_BUILD_UI "Build retained widgets and UI rendering (requires text)" ON)
option(VNG_BUILD_EDITOR "Build editor core and Linux preview/editor applications" ON)
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

add_library(vng_timeline STATIC src/timeline/timeline.cpp)
add_library(vng::timeline ALIAS vng_timeline)
target_link_libraries(vng_timeline PUBLIC vng_core)
vng_target_defaults(vng_timeline)

add_library(vng_spatial STATIC src/spatial/triangle_bvh.cpp)
add_library(vng::spatial ALIAS vng_spatial)
target_link_libraries(vng_spatial PUBLIC vng_core)
vng_target_defaults(vng_spatial)

add_library(vng_resources INTERFACE)
add_library(vng::resources ALIAS vng_resources)
target_link_libraries(vng_resources INTERFACE vng_core)

add_library(vng_gfx INTERFACE)
add_library(vng::gfx ALIAS vng_gfx)
target_link_libraries(vng_gfx INTERFACE vng_core)

add_library(vng_analysis INTERFACE)
add_library(vng::analysis ALIAS vng_analysis)
target_link_libraries(vng_analysis INTERFACE vng_gfx)

add_library(vng_render INTERFACE)
add_library(vng::render ALIAS vng_render)
target_link_libraries(vng_render INTERFACE vng_gfx vng_resources)

file(GLOB VNG_RIG_SOURCES CONFIGURE_DEPENDS src/rig/*.cpp)
add_library(vng_rig STATIC ${VNG_RIG_SOURCES})
add_library(vng::rig ALIAS vng_rig)
target_link_libraries(vng_rig PUBLIC vng_gfx vng_render)
vng_target_defaults(vng_rig)

if(VNG_BUILD_TEXT)
    vng_provide_text_dependencies()
    file(GLOB VNG_TEXT_SOURCES CONFIGURE_DEPENDS src/text/*.cpp)
    add_library(vng_text STATIC ${VNG_TEXT_SOURCES})
    add_library(vng::text ALIAS vng_text)
    target_link_libraries(vng_text PUBLIC vng_core
        PRIVATE Freetype::Freetype vng_harfbuzz_dependency)
    vng_target_defaults(vng_text)
    if(VNG_BUILD_UI)
        find_package(ICU 67 REQUIRED COMPONENTS uc i18n)
        add_library(vng_ui STATIC src/ui/ui.cpp)
        add_library(vng::ui ALIAS vng_ui)
        target_link_libraries(vng_ui PUBLIC vng_text vng_gfx vng_render PRIVATE ICU::uc ICU::i18n)
        vng_target_defaults(vng_ui)
    endif()
endif()

file(GLOB VNG_CONTENT_SOURCES CONFIGURE_DEPENDS src/content/*.cpp)
add_library(vng_content STATIC ${VNG_CONTENT_SOURCES})
add_library(vng::content ALIAS vng_content)
target_link_libraries(vng_content PUBLIC vng_gfx)
find_package(PNG REQUIRED)
target_link_libraries(vng_content PRIVATE PNG::PNG)
target_include_directories(vng_content PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
vng_target_defaults(vng_content)

if(VNG_BUILD_EDITOR)
    # CPU-only procedural authoring; no dependency back into editor or graphics.
    add_library(vng_earth_assets STATIC examples/support/earth_assets.cpp
        examples/support/earth_clouds.cpp examples/support/earth_edit.cpp)
    target_link_libraries(vng_earth_assets PUBLIC vng_content)
    vng_target_defaults(vng_earth_assets)
    add_library(vng_editor STATIC src/editor/inspector.cpp src/editor/mesh.cpp src/editor/mesh_topology.cpp src/editor/mesh_patch.cpp)
    add_library(vng::editor ALIAS vng_editor)
    target_link_libraries(vng_editor PUBLIC vng_content vng_spatial)
    vng_target_defaults(vng_editor)
    add_library(vng_editor_project STATIC examples/editor/project.cpp examples/editor/edits.cpp examples/editor/effects.cpp examples/editor/effect_presets.cpp
        examples/editor/preview_updates.cpp examples/editor/camera_edits.cpp
        examples/editor/navigation.cpp examples/editor/selection.cpp examples/editor/blueprint_gizmos.cpp examples/editor/vertex_drag.cpp
        examples/editor/scene_file.cpp examples/editor/animation.cpp examples/editor/playback.cpp
        examples/editor/keyframes.cpp examples/editor/settings.cpp examples/editor/position_edits.cpp
        examples/editor/rotation_edits.cpp examples/editor/scale_edits.cpp examples/editor/transform_pivot.cpp examples/editor/edit_clipboard.cpp
        examples/editor/selection_edits.cpp examples/editor/document_patch.cpp)
    target_link_libraries(vng_editor_project PUBLIC vng_editor vng_timeline vng_earth_assets)
    target_sources(vng_editor_project PRIVATE examples/editor/blueprint_mesh_controls.cpp)
    target_sources(vng_editor_project PRIVATE examples/editor/editing_session.cpp examples/editor/session_operations.cpp
        examples/editor/regions.cpp examples/editor/region_geometry.cpp examples/editor/region_edits.cpp
        examples/editor/region_instances.cpp examples/editor/camera_instances.cpp)
    target_include_directories(vng_editor_project PUBLIC examples)
    vng_target_defaults(vng_editor_project)
    if(TARGET vng_ui)
        # Retained panels, dialogs and gizmo tools: everything the editor UI
        # draws through vng_ui without a GL context. The app and the UI tests
        # both link this; neither lists these sources again.
        add_library(vng_editor_ui STATIC
            examples/editor/inspector_panel.cpp examples/editor/timeline_panel.cpp examples/editor/tool_panel.cpp
            examples/editor/blueprint_mesh_panel.cpp examples/editor/mesh_tools.cpp
            examples/editor/file_dialog.cpp examples/editor/import_dialog.cpp examples/editor/open_scene_dialog.cpp
            examples/editor/save_dialog.cpp
            examples/editor/translation_tool.cpp examples/editor/rotation_tool.cpp examples/editor/scale_tool.cpp
            examples/editor/world_bounds_tool.cpp examples/editor/surface_move_tool.cpp examples/editor/cage_tool.cpp
            examples/editor/region_editor.cpp examples/editor/transform_gesture.cpp examples/editor/component_transform.cpp)
        add_library(vng::editor_ui ALIAS vng_editor_ui)
        target_link_libraries(vng_editor_ui PUBLIC vng_editor_project vng_ui)
        vng_target_defaults(vng_editor_ui)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Threads REQUIRED)
        add_library(vng_editor_preview STATIC src/editor/preview.cpp)
        add_library(vng::editor_preview ALIAS vng_editor_preview)
        target_link_libraries(vng_editor_preview PUBLIC vng_editor PRIVATE Threads::Threads)
        vng_target_defaults(vng_editor_preview)
    endif()
endif()

file(GLOB VNG_SHADER_SOURCES CONFIGURE_DEPENDS src/shader/*.cpp)
add_library(vng_shader STATIC ${VNG_SHADER_SOURCES})
add_library(vng::shader ALIAS vng_shader)
target_link_libraries(vng_shader PUBLIC vng_gfx)
target_include_directories(vng_shader PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
vng_target_defaults(vng_shader)

add_library(vng_providers INTERFACE)
add_library(vng::providers ALIAS vng_providers)
target_link_libraries(vng_providers INTERFACE vng_resources vng_render vng_shader vng_content)

# Shader compilation belongs to backend integrations and concrete renderers;
# the backend-neutral Renderer<Ticket, Backend> core remains independent of shaders.

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
        PUBLIC vng_gfx vng_render vng_glsl vng_resources
        PRIVATE vng_glad_dependency)
    vng_target_defaults(vng_opengl)

    file(GLOB VNG_RENDER_OPENGL_SOURCES CONFIGURE_DEPENDS src/render_opengl/*.cpp)
    add_library(vng_render_opengl STATIC ${VNG_RENDER_OPENGL_SOURCES})
    add_library(vng::render_opengl ALIAS vng_render_opengl)
    target_include_directories(vng_render_opengl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(vng_render_opengl PUBLIC
        vng_render vng_analysis vng_glsl vng_opengl)
    vng_target_defaults(vng_render_opengl)

    add_library(vng_resources_opengl STATIC src/resources_opengl/mesh_renderer.cpp)
    add_library(vng::resources_opengl ALIAS vng_resources_opengl)
    target_link_libraries(vng_resources_opengl PUBLIC vng_opengl vng_providers
        PRIVATE vng_glad_dependency)
    vng_target_defaults(vng_resources_opengl)

    add_library(vng_bloom_opengl STATIC src/bloom_opengl/bloom.cpp)
    add_library(vng::bloom_opengl ALIAS vng_bloom_opengl)
    target_link_libraries(vng_bloom_opengl PUBLIC vng_opengl vng_resources
        PRIVATE vng_glad_dependency)
    vng_target_defaults(vng_bloom_opengl)

    add_library(vng_rig_opengl STATIC src/rig_opengl/skinned_mesh_renderer.cpp)
    add_library(vng::rig_opengl ALIAS vng_rig_opengl)
    target_link_libraries(vng_rig_opengl PUBLIC vng_rig vng_render_opengl
        vng_resources
        PRIVATE vng_glad_dependency)
    vng_target_defaults(vng_rig_opengl)

    if(VNG_BUILD_TEXT)
        add_library(vng_text_opengl STATIC src/text_opengl/text_renderer.cpp)
        add_library(vng::text_opengl ALIAS vng_text_opengl)
        target_link_libraries(vng_text_opengl PUBLIC vng_opengl vng_text
            vng_resources
            PRIVATE vng_glad_dependency)
        vng_target_defaults(vng_text_opengl)
        if(VNG_BUILD_UI)
            add_library(vng_ui_opengl STATIC src/ui_opengl/ui_renderer.cpp)
            add_library(vng::ui_opengl ALIAS vng_ui_opengl)
            target_link_libraries(vng_ui_opengl PUBLIC vng_ui vng_text_opengl PRIVATE vng_glad_dependency)
            vng_target_defaults(vng_ui_opengl)
        endif()
    endif()
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
    add_library(vng_glfw_opengl STATIC src/glfw_opengl/glfw_opengl.cpp)
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
    # Source-level checks: every <vng/...> include stays inside its target's
    # declared links, the module graph is a tree, and the explore guide matches.
    find_program(VNG_NODE_EXECUTABLE node)
    if(VNG_NODE_EXECUTABLE)
        add_test(NAME vng_layering_tests COMMAND ${VNG_NODE_EXECUTABLE} --test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/layering/layering.test.cjs)
        add_test(NAME vng_guide_tests COMMAND ${VNG_NODE_EXECUTABLE} --test
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/explore/tests/content.test.cjs)
    endif()
    add_executable(vng_timeline_tests tests/timeline/timeline_tests.cpp)
    target_link_libraries(vng_timeline_tests PRIVATE vng_timeline Catch2::Catch2WithMain)
    vng_target_defaults(vng_timeline_tests)
    add_test(NAME vng_timeline_tests COMMAND vng_timeline_tests)
    if(VNG_BUILD_EDITOR)
        add_executable(vng_editor_tests tests/editor/inspector_tests.cpp tests/editor/project_tests.cpp
            tests/editor/edit_tests.cpp tests/editor/update_tests.cpp tests/editor/camera_edit_tests.cpp
            tests/editor/navigation_tests.cpp tests/editor/selection_tests.cpp tests/editor/vertex_drag_tests.cpp
            tests/editor/viewport_shortcut_tests.cpp tests/editor/file_shortcut_tests.cpp
            tests/editor/scene_file_tests.cpp tests/editor/animation_tests.cpp tests/editor/playback_tests.cpp
            tests/editor/keyframe_tests.cpp tests/editor/preview_viewport_tests.cpp
            tests/editor/settings_tests.cpp tests/editor/camera_instance_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/mesh_import_view_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/effect_preset_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/blueprint_mesh_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/position_edit_tests.cpp tests/editor/position_drag_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/rotation_edit_tests.cpp tests/editor/rotation_drag_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/scale_edit_tests.cpp tests/editor/edit_shortcut_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/selection_edit_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/effects_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/document_patch_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/mesh_topology_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/interaction_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/multiselection_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/picking_acceleration_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/editing_session_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/region_tests.cpp)
        target_sources(vng_editor_tests PRIVATE tests/editor/region_edit_precision_tests.cpp)
        target_link_libraries(vng_editor_tests PRIVATE vng_editor_project Catch2::Catch2WithMain)
        target_compile_definitions(vng_editor_tests PRIVATE
            VNG_TIMELINE_SCENE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/editor_timeline.vscene")
        vng_target_defaults(vng_editor_tests)
        add_test(NAME vng_editor_tests COMMAND vng_editor_tests)
        if(TARGET vng_editor_preview)
            add_executable(vng_editor_preview_tests tests/editor/preview_tests.cpp)
            target_link_libraries(vng_editor_preview_tests PRIVATE vng_editor_preview Catch2::Catch2)
            vng_target_defaults(vng_editor_preview_tests)
            add_test(NAME vng_editor_preview_tests COMMAND vng_editor_preview_tests)
        endif()
    endif()

    add_executable(vng_resources_tests tests/resources/provider_tests.cpp)
    target_link_libraries(vng_resources_tests PRIVATE vng_resources Catch2::Catch2WithMain)
    vng_target_defaults(vng_resources_tests)
    add_test(NAME vng_resources_tests COMMAND vng_resources_tests)

    file(GLOB VNG_RIG_TEST_SOURCES CONFIGURE_DEPENDS tests/rig/*.cpp)
    add_executable(vng_rig_tests ${VNG_RIG_TEST_SOURCES})
    target_link_libraries(vng_rig_tests PRIVATE vng_rig Catch2::Catch2WithMain)
    vng_target_defaults(vng_rig_tests)
    add_test(NAME vng_rig_tests COMMAND vng_rig_tests)

    if(VNG_BUILD_TEXT)
        file(GLOB VNG_TEXT_TEST_SOURCES CONFIGURE_DEPENDS tests/text/*.cpp)
        add_executable(vng_text_tests ${VNG_TEXT_TEST_SOURCES})
        target_link_libraries(vng_text_tests PRIVATE vng_text Catch2::Catch2WithMain)
        target_compile_definitions(vng_text_tests PRIVATE
            VNG_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/DejaVuSans.ttf")
        vng_target_defaults(vng_text_tests)
        add_test(NAME vng_text_tests COMMAND vng_text_tests)
        if(VNG_BUILD_UI)
            add_executable(vng_ui_tests tests/ui/ui_tests.cpp)
            target_link_libraries(vng_ui_tests PRIVATE vng_ui Catch2::Catch2WithMain)
            target_compile_definitions(vng_ui_tests PRIVATE
                VNG_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/DejaVuSans.ttf")
            vng_target_defaults(vng_ui_tests)
            add_test(NAME vng_ui_tests COMMAND vng_ui_tests)
            set_tests_properties(vng_ui_tests PROPERTIES LABELS ui)
            if(VNG_BUILD_EDITOR)
                add_executable(vng_editor_ui_tests
                    tests/editor/inspector_panel_tests.cpp tests/editor/blueprint_mesh_panel_tests.cpp
                    tests/editor/save_dialog_tests.cpp tests/editor/import_dialog_tests.cpp tests/editor/open_scene_dialog_tests.cpp
                    tests/editor/rotation_tool_tests.cpp tests/editor/scale_tool_tests.cpp tests/editor/blueprint_gizmo_tests.cpp
                    tests/editor/world_bounds_tool_tests.cpp tests/editor/timeline_panel_tests.cpp
                    tests/editor/settings_panel_tests.cpp tests/editor/view_selector_tests.cpp tests/editor/toolbar_row_tests.cpp
                    tests/editor/preview_fps_tests.cpp tests/editor/mesh_tools_tests.cpp tests/editor/instance_list_tests.cpp
                    tests/editor/tool_panel_tests.cpp tests/editor/region_ui_tests.cpp tests/editor/component_transform_tests.cpp
                    tests/editor/instance_transform_tests.cpp)
                target_link_libraries(vng_editor_ui_tests PRIVATE vng_editor_ui Catch2::Catch2WithMain)
                target_compile_definitions(vng_editor_ui_tests PRIVATE VNG_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/DejaVuSans.ttf")
                vng_target_defaults(vng_editor_ui_tests)
                add_test(NAME vng_editor_ui_tests COMMAND vng_editor_ui_tests)
            endif()
        endif()
    endif()

    add_executable(vng_gfx_tests
        tests/gfx/camera_tests.cpp
        tests/gfx/gfx_tests.cpp
        tests/gfx/mesh_tests.cpp)
    target_link_libraries(vng_gfx_tests PRIVATE vng_gfx Catch2::Catch2WithMain)
    vng_target_defaults(vng_gfx_tests)
    add_test(NAME vng_gfx_tests COMMAND vng_gfx_tests)

    add_executable(vng_render_tests
        tests/render/graphics_state_tests.cpp
        tests/render/program_tests.cpp
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
        add_executable(vng_content_tests tests/content/content_tests.cpp tests/content/image_tests.cpp)
        target_link_libraries(vng_content_tests PRIVATE
            vng_content Catch2::Catch2WithMain)
        vng_target_defaults(vng_content_tests)
        add_test(NAME vng_content_tests COMMAND vng_content_tests)
    endif()

    add_executable(vng_document_tests tests/content/document_tests.cpp)
    target_link_libraries(vng_document_tests PRIVATE vng_content Catch2::Catch2WithMain)
    vng_target_defaults(vng_document_tests)
    add_test(NAME vng_document_tests COMMAND vng_document_tests)

    add_executable(vng_spaceflight_scene_tests
        tests/examples/spaceflight_scene_tests.cpp
        examples/support/spaceflight_scene.cpp)
    target_include_directories(vng_spaceflight_scene_tests PRIVATE examples)
    target_compile_definitions(vng_spaceflight_scene_tests PRIVATE
        VNG_TEST_SOLAR_FLYBY_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/solar_flyby.vscene")
    target_link_libraries(vng_spaceflight_scene_tests PRIVATE vng_content Catch2::Catch2WithMain)
    vng_target_defaults(vng_spaceflight_scene_tests)
    add_test(NAME vng_spaceflight_scene_tests COMMAND vng_spaceflight_scene_tests)

    add_executable(vng_spaceship_model_tests tests/examples/spaceship_model_tests.cpp
        tests/examples/fleet_model_tests.cpp)
    target_link_libraries(vng_spaceship_model_tests PRIVATE vng_content Catch2::Catch2WithMain)
    target_compile_definitions(vng_spaceship_model_tests PRIVATE
        VNG_TEST_SPACESHIP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/spaceship.vmesh")
    vng_target_defaults(vng_spaceship_model_tests)
    add_test(NAME vng_spaceship_model_tests COMMAND vng_spaceship_model_tests)

    add_executable(vng_sun_assets_tests tests/examples/sun_assets_tests.cpp
        examples/support/sun_assets.cpp)
    target_include_directories(vng_sun_assets_tests PRIVATE examples)
    target_link_libraries(vng_sun_assets_tests PRIVATE vng_gfx Catch2::Catch2WithMain)
    vng_target_defaults(vng_sun_assets_tests)
    add_test(NAME vng_sun_assets_tests COMMAND vng_sun_assets_tests)

    add_executable(vng_sun_options_tests tests/examples/sun_options_tests.cpp
        examples/support/sun_options.cpp)
    target_include_directories(vng_sun_options_tests PRIVATE examples)
    target_link_libraries(vng_sun_options_tests PRIVATE vng_resources Catch2::Catch2WithMain)
    vng_target_defaults(vng_sun_options_tests)
    add_test(NAME vng_sun_options_tests COMMAND vng_sun_options_tests)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/shader/shader_tests.cpp")
        add_executable(vng_shader_tests tests/shader/shader_tests.cpp
            tests/shader/argument_tests.cpp
            tests/shader/sun_math_tests.cpp
            tests/shader/constant_operand_tests.cpp)
        target_link_libraries(vng_shader_tests PRIVATE vng_shader Catch2::Catch2WithMain)
        vng_target_defaults(vng_shader_tests)
        add_test(NAME vng_shader_tests COMMAND vng_shader_tests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/glsl/glsl_tests.cpp")
        add_executable(vng_glsl_tests tests/glsl/glsl_tests.cpp
            tests/glsl/sun_math_tests.cpp
            tests/glsl/argument_tests.cpp)
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
        if(VNG_GLFW_SMOOTH_SCROLL AND GLFW_BUILD_X11)
            find_package(X11 REQUIRED)
            add_executable(vng_scroll_input_tests tests/window/scroll_axes_tests.cpp)
            target_include_directories(vng_scroll_input_tests PRIVATE ${X11_INCLUDE_DIR})
            target_link_libraries(vng_scroll_input_tests PRIVATE Catch2::Catch2WithMain)
            vng_target_defaults(vng_scroll_input_tests)
            add_test(NAME vng_scroll_input_tests COMMAND vng_scroll_input_tests)
            find_program(VNG_XVFB_RUN xvfb-run)
            find_program(VNG_XEPHYR Xephyr)
            if(X11_Xtst_FOUND AND (VNG_XVFB_RUN OR VNG_XEPHYR))
                add_executable(vng_x11_scroll_tests tests/window/x11_scroll_tests.cpp)
                target_link_libraries(vng_x11_scroll_tests PRIVATE vng_window_glfw
                    vng_glfw_dependency X11::X11 X11::Xi X11::Xtst Catch2::Catch2WithMain)
                vng_target_defaults(vng_x11_scroll_tests)
                if(VNG_XVFB_RUN)
                    add_test(NAME vng_x11_scroll_tests
                        COMMAND ${VNG_XVFB_RUN} -a $<TARGET_FILE:vng_x11_scroll_tests>)
                else()
                    add_test(NAME vng_x11_scroll_tests COMMAND bash
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/window/run_nested_x11.sh
                        ${VNG_XEPHYR} $<TARGET_FILE:vng_x11_scroll_tests>)
                endif()
                set_tests_properties(vng_x11_scroll_tests PROPERTIES
                    SKIP_RETURN_CODE 77 LABELS window TIMEOUT 20)
            endif()
        endif()
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
            tests/opengl/image_resource_tests.cpp
            tests/opengl/bloom_tests.cpp
            tests/opengl/graphics_state_tests.cpp
            tests/opengl/commands_tests.cpp
            tests/opengl/diagnostic_tests.cpp
            tests/opengl/frame_tests.cpp
            tests/opengl/graphics_program_tests.cpp
            tests/opengl/gpu_mesh_tests.cpp
            tests/opengl/opengl_tests.cpp
            tests/opengl/shader_driver_tests.cpp)
        target_link_libraries(vng_opengl_tests PRIVATE
            vng_opengl vng_glfw_opengl vng_glsl vng_bloom_opengl Catch2::Catch2WithMain)
        vng_target_defaults(vng_opengl_tests)
        add_test(NAME vng_opengl_tests COMMAND vng_opengl_tests)
        set_tests_properties(vng_opengl_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS opengl)

        add_executable(vng_resource_owner_tests tests/opengl/resource_owner_tests.cpp)
        target_link_libraries(vng_resource_owner_tests PRIVATE vng_resources_opengl
            vng_glfw_opengl vng_glad_dependency Catch2::Catch2WithMain)
        vng_target_defaults(vng_resource_owner_tests)
        add_test(NAME vng_resource_owner_tests COMMAND vng_resource_owner_tests)
        set_tests_properties(vng_resource_owner_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS "resources;opengl")

        add_executable(vng_rig_opengl_tests tests/opengl/skinned_renderer_tests.cpp)
        target_link_libraries(vng_rig_opengl_tests PRIVATE
            vng_rig_opengl vng_glfw_opengl vng_glad_dependency Catch2::Catch2WithMain)
        vng_target_defaults(vng_rig_opengl_tests)
        add_test(NAME vng_rig_opengl_tests COMMAND vng_rig_opengl_tests)
        set_tests_properties(vng_rig_opengl_tests PROPERTIES
            SKIP_RETURN_CODE 77 LABELS "rig;opengl")

        if(VNG_BUILD_TEXT)
            add_executable(vng_text_opengl_tests tests/opengl/text_renderer_tests.cpp)
            target_link_libraries(vng_text_opengl_tests PRIVATE
                vng_text_opengl vng_glfw_opengl vng_glad_dependency Catch2::Catch2WithMain)
            target_compile_definitions(vng_text_opengl_tests PRIVATE
                VNG_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/DejaVuSans.ttf")
            vng_target_defaults(vng_text_opengl_tests)
            add_test(NAME vng_text_opengl_tests COMMAND vng_text_opengl_tests)
            set_tests_properties(vng_text_opengl_tests PROPERTIES
                SKIP_RETURN_CODE 77 LABELS "text;opengl")
            if(VNG_BUILD_UI)
                add_executable(vng_ui_opengl_tests tests/opengl/ui_renderer_tests.cpp)
                target_link_libraries(vng_ui_opengl_tests PRIVATE
                    vng_ui_opengl vng_glfw_opengl Catch2::Catch2WithMain)
                target_compile_definitions(vng_ui_opengl_tests PRIVATE
                    VNG_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fonts/DejaVuSans.ttf")
                vng_target_defaults(vng_ui_opengl_tests)
                add_test(NAME vng_ui_opengl_tests COMMAND vng_ui_opengl_tests)
                set_tests_properties(vng_ui_opengl_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS "ui;opengl")
            endif()
        endif()

        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/render_opengl/program_runtime_tests.cpp")
            add_executable(vng_render_opengl_tests
                tests/render_opengl/program_runtime_tests.cpp
                tests/opengl/sun_math_tests.cpp
                tests/opengl/argument_tests.cpp)
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

    vng_add_compile_failure_test(vng_compile_fail_renderer_missing_backend
        tests/compile_fail/renderer_missing_backend.cpp "Renderer")
    vng_add_compile_failure_test(vng_compile_fail_renderer_wrong_backend
        tests/compile_fail/renderer_wrong_backend.cpp "no matching function")

    vng_add_compile_failure_test(
        vng_compile_fail_argument_host_lambda
        tests/compile_fail/argument_host_lambda.cpp
        "Shader lambda arguments after the stage context must be Expr")

    foreach(operand_case captured_operand clamp_bounds constructor_operand
            intrinsic_operand make_operand mutable_operand runtime_matrix
            runtime_prvalue runtime_vector select_operand power_operand texture_lod)
        vng_add_compile_failure_test(
            vng_compile_fail_literal_${operand_case}
            tests/compile_fail/literal_${operand_case}.cpp
            "constant expression")
    endforeach()
    vng_add_compile_failure_test(vng_compile_fail_literal_wrong_integer
        tests/compile_fail/literal_wrong_integer.cpp "no match")
    vng_add_compile_failure_test(vng_compile_fail_literal_all_host_constructor
        tests/compile_fail/literal_all_host_constructor.cpp "no matching")

    vng_add_compile_failure_test(
        vng_compile_fail_document_unknown_type
        tests/compile_fail/document_unknown_type.cpp
        "Unsupported document type")
    vng_add_compile_failure_test(
        vng_compile_fail_document_expected_callback
        tests/compile_fail/document_expected_callback.cpp
        "read() callbacks return plain values")
    vng_add_compile_failure_test(
        vng_compile_fail_document_reference_callback
        tests/compile_fail/document_reference_callback.cpp
        "read() callbacks must return a value")

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

if(VNG_BUILD_EXAMPLES)
    add_executable(vng_make_spaceship EXCLUDE_FROM_ALL examples/tools/make_spaceship.cpp)
    target_link_libraries(vng_make_spaceship PRIVATE vng_content)
    vng_target_defaults(vng_make_spaceship)
    add_executable(vng_make_fleet EXCLUDE_FROM_ALL examples/tools/make_fleet.cpp)
    target_link_libraries(vng_make_fleet PRIVATE vng_content)
    vng_target_defaults(vng_make_fleet)
    if(TARGET vng_editor_project)
        add_library(vng_earth_scene STATIC examples/scenes/earth_scene.cpp)
        target_link_libraries(vng_earth_scene PUBLIC vng_editor_project)
        vng_target_defaults(vng_earth_scene)
        add_executable(vng_make_earth EXCLUDE_FROM_ALL examples/tools/make_earth.cpp)
        target_link_libraries(vng_make_earth PRIVATE vng_earth_scene)
        vng_target_defaults(vng_make_earth)
        if(VNG_BUILD_TESTS)
            add_executable(vng_earth_assets_tests tests/examples/earth_assets_tests.cpp)
            target_link_libraries(vng_earth_assets_tests PRIVATE vng_earth_scene Catch2::Catch2WithMain)
            target_compile_definitions(vng_earth_assets_tests PRIVATE VNG_EARTH_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
            vng_target_defaults(vng_earth_assets_tests)
            add_test(NAME vng_earth_assets_tests COMMAND vng_earth_assets_tests)
        endif()
        add_library(vng_fleet_scene STATIC examples/scenes/fleet_scene.cpp)
        target_link_libraries(vng_fleet_scene PUBLIC vng_editor_project)
        vng_target_defaults(vng_fleet_scene)
        add_executable(vng_make_fleet_scene EXCLUDE_FROM_ALL examples/tools/make_fleet_scene.cpp)
        target_link_libraries(vng_make_fleet_scene PRIVATE vng_fleet_scene)
        vng_target_defaults(vng_make_fleet_scene)
        add_library(vng_asteroid_scene STATIC examples/scenes/asteroid_scene.cpp
            examples/support/asteroid_assets.cpp)
        target_link_libraries(vng_asteroid_scene PUBLIC vng_fleet_scene)
        vng_target_defaults(vng_asteroid_scene)
        add_executable(vng_make_asteroid_scene EXCLUDE_FROM_ALL examples/tools/make_asteroid_scene.cpp)
        target_link_libraries(vng_make_asteroid_scene PRIVATE vng_asteroid_scene)
        vng_target_defaults(vng_make_asteroid_scene)
        add_library(vng_solar_system_scene STATIC examples/scenes/solar_system_scene.cpp)
        target_link_libraries(vng_solar_system_scene PUBLIC vng_asteroid_scene)
        vng_target_defaults(vng_solar_system_scene)
        add_executable(vng_make_solar_system_scene EXCLUDE_FROM_ALL examples/tools/make_solar_system_scene.cpp)
        target_link_libraries(vng_make_solar_system_scene PRIVATE vng_solar_system_scene)
        vng_target_defaults(vng_make_solar_system_scene)
        if(VNG_BUILD_TESTS)
            add_executable(vng_fleet_scene_tests tests/examples/fleet_scene_tests.cpp)
            target_link_libraries(vng_fleet_scene_tests PRIVATE vng_fleet_scene Catch2::Catch2WithMain)
            target_compile_definitions(vng_fleet_scene_tests PRIVATE
                VNG_TEST_ASSET_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
            vng_target_defaults(vng_fleet_scene_tests)
            add_test(NAME vng_fleet_scene_tests COMMAND vng_fleet_scene_tests)
            add_executable(vng_asteroid_scene_tests tests/examples/asteroid_scene_tests.cpp)
            target_link_libraries(vng_asteroid_scene_tests PRIVATE vng_asteroid_scene Catch2::Catch2WithMain)
            target_compile_definitions(vng_asteroid_scene_tests PRIVATE
                VNG_ASTEROID_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
            vng_target_defaults(vng_asteroid_scene_tests)
            add_test(NAME vng_asteroid_scene_tests COMMAND vng_asteroid_scene_tests)
            add_executable(vng_solar_system_scene_tests tests/examples/solar_system_scene_tests.cpp)
            target_link_libraries(vng_solar_system_scene_tests PRIVATE vng_solar_system_scene Catch2::Catch2WithMain)
            target_compile_definitions(vng_solar_system_scene_tests PRIVATE
                VNG_SOLAR_SYSTEM_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
            vng_target_defaults(vng_solar_system_scene_tests)
            add_test(NAME vng_solar_system_scene_tests COMMAND vng_solar_system_scene_tests)
        endif()
    endif()
    set(VNG_DOCUMENT_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/examples/assets")
    file(MAKE_DIRECTORY "${VNG_DOCUMENT_ASSET_DIR}")
    configure_file(examples/assets/custom_scene.vscene
        "${VNG_DOCUMENT_ASSET_DIR}/custom_scene.vscene" COPYONLY)
    add_executable(vng_document_demo examples/document.cpp)
    target_link_libraries(vng_document_demo PRIVATE vng_content)
    target_compile_definitions(vng_document_demo PRIVATE
        VNG_EXAMPLE_DOCUMENT_PATH="${VNG_DOCUMENT_ASSET_DIR}/custom_scene.vscene")
    vng_target_defaults(vng_document_demo)
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

    foreach(asset spaceship.vmesh spaceflight.vscene solar_flyby.vscene quiet_sun.veffect)
        configure_file("examples/assets/${asset}" "${VNG_EXAMPLE_ASSET_DIR}/${asset}" COPYONLY)
    endforeach()
    add_library(vng_example_presentation STATIC examples/support/presentation.cpp)
    target_include_directories(vng_example_presentation PUBLIC examples)
    target_link_libraries(vng_example_presentation PUBLIC vng_opengl
        PRIVATE vng_glad_dependency PNG::PNG)
    vng_target_defaults(vng_example_presentation)
    add_library(vng_spaceflight_support STATIC
        examples/support/spaceflight_scene.cpp
        examples/support/spaceship_renderer.cpp
        examples/support/space_background.cpp
        examples/support/spaceflight_inspection.cpp)
    target_include_directories(vng_spaceflight_support PUBLIC examples)
    target_link_libraries(vng_spaceflight_support PUBLIC vng_content vng_render_opengl vng_example_presentation
        PRIVATE vng_glad_dependency PNG::PNG)
    vng_target_defaults(vng_spaceflight_support)
    add_executable(vng_spaceflight_demo examples/spaceflight.cpp)
    target_link_libraries(vng_spaceflight_demo PRIVATE
        vng_spaceflight_support vng_sun_support vng_bloom_opengl vng_example_support)
    target_compile_definitions(vng_spaceflight_demo PRIVATE
        VNG_EXAMPLE_SPACEFLIGHT_PATH="${VNG_EXAMPLE_ASSET_DIR}/spaceflight.vscene")
    vng_target_defaults(vng_spaceflight_demo)

    add_library(vng_sun_support STATIC
        examples/support/sun_assets.cpp
        examples/support/sun_options.cpp
        examples/support/sun_shaders.cpp
        examples/support/sun_renderer.cpp
        examples/support/sun_softening.cpp
        examples/support/sun_inspection.cpp)
    target_include_directories(vng_sun_support PUBLIC examples)
    target_link_libraries(vng_sun_support PUBLIC vng_render_opengl
        PRIVATE vng_glad_dependency vng_example_presentation)
    vng_target_defaults(vng_sun_support)
    if(TARGET vng_editor_preview)
        add_library(vng_editor_runtime STATIC examples/editor/runtime.cpp examples/editor/blueprint_mesh_renderer.cpp
            examples/editor/mesh_programs.cpp examples/editor/scene_annotations.cpp)
        target_link_libraries(vng_editor_runtime PUBLIC vng_editor_project vng_sun_support
            vng_bloom_opengl vng_example_presentation PRIVATE vng_glad_dependency)
        vng_target_defaults(vng_editor_runtime)
        add_library(vng_scene_demo_support STATIC examples/scenes/scene_demo.cpp
            examples/scenes/fleet_inspection.cpp examples/support/spaceflight_scene.cpp)
        target_link_libraries(vng_scene_demo_support PUBLIC vng_editor_runtime vng_example_support)
        vng_target_defaults(vng_scene_demo_support)
        add_executable(vng_earth_demo examples/earth.cpp)
        target_link_libraries(vng_earth_demo PRIVATE vng_scene_demo_support)
        target_compile_definitions(vng_earth_demo PRIVATE
            VNG_EARTH_SCENE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/earth.vscene")
        vng_target_defaults(vng_earth_demo)
        add_executable(vng_fleet_reveal_demo examples/fleet_reveal.cpp)
        target_link_libraries(vng_fleet_reveal_demo PRIVATE vng_scene_demo_support)
        target_compile_definitions(vng_fleet_reveal_demo PRIVATE
            VNG_FLEET_SCENE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/fleet_reveal.vscene")
        vng_target_defaults(vng_fleet_reveal_demo)
        add_executable(vng_asteroid_fleet_demo examples/asteroid_fleet.cpp)
        target_link_libraries(vng_asteroid_fleet_demo PRIVATE vng_scene_demo_support)
        target_compile_definitions(vng_asteroid_fleet_demo PRIVATE
            VNG_ASTEROID_SCENE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/asteroid_fleet.vscene")
        vng_target_defaults(vng_asteroid_fleet_demo)
        add_executable(vng_solar_system_demo examples/solar_system.cpp)
        target_link_libraries(vng_solar_system_demo PRIVATE vng_scene_demo_support vng_solar_system_scene)
        target_compile_definitions(vng_solar_system_demo PRIVATE
            VNG_SOLAR_SYSTEM_SCENE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/solar_system.vscene")
        vng_target_defaults(vng_solar_system_demo)
        add_executable(vng_editor_worker examples/editor_worker.cpp)
        target_link_libraries(vng_editor_worker PRIVATE vng_editor_runtime vng_editor_preview vng_example_support)
        vng_target_defaults(vng_editor_worker)
        if(VNG_BUILD_TESTS)
            add_executable(vng_earth_render_tests tests/examples/earth_render_tests.cpp)
            target_link_libraries(vng_earth_render_tests PRIVATE vng_editor_runtime vng_earth_scene vng_example_support Catch2::Catch2WithMain)
            target_compile_definitions(vng_earth_render_tests PRIVATE VNG_EARTH_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
            vng_target_defaults(vng_earth_render_tests)
            add_test(NAME vng_earth_render_tests COMMAND vng_earth_render_tests)
            set_tests_properties(vng_earth_render_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS "editor;examples;opengl")
            add_executable(vng_editor_runtime_tests tests/editor/runtime_tests.cpp tests/editor/mesh_batch_tests.cpp)
            target_link_libraries(vng_editor_runtime_tests PRIVATE vng_editor_runtime vng_asteroid_scene vng_example_support vng_glad_dependency Catch2::Catch2WithMain)
            vng_target_defaults(vng_editor_runtime_tests)
            add_test(NAME vng_editor_runtime_tests COMMAND vng_editor_runtime_tests)
            set_tests_properties(vng_editor_runtime_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS "editor;opengl")
            add_executable(vng_asteroid_render_tests tests/examples/asteroid_render_tests.cpp)
            target_link_libraries(vng_asteroid_render_tests PRIVATE vng_editor_runtime vng_asteroid_scene vng_example_support Catch2::Catch2WithMain)
            target_compile_definitions(vng_asteroid_render_tests PRIVATE
                VNG_ASTEROID_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
            vng_target_defaults(vng_asteroid_render_tests)
            add_test(NAME vng_asteroid_render_tests COMMAND vng_asteroid_render_tests)
            set_tests_properties(vng_asteroid_render_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS "editor;examples;opengl")
            add_executable(vng_editor_worker_tests tests/editor/worker_tests.cpp)
            target_link_libraries(vng_editor_worker_tests PRIVATE vng_editor_project vng_editor_preview)
            target_compile_definitions(vng_editor_worker_tests PRIVATE
                VNG_EDITOR_WORKER_PATH="$<TARGET_FILE:vng_editor_worker>"
                VNG_EDITOR_MESH_PATH="${VNG_EXAMPLE_ASSET_DIR}/colored_cube.vmesh")
            add_dependencies(vng_editor_worker_tests vng_editor_worker)
            vng_target_defaults(vng_editor_worker_tests)
            add_test(NAME vng_editor_worker_tests COMMAND vng_editor_worker_tests)
            set_tests_properties(vng_editor_worker_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 360 LABELS "editor;opengl")
        endif()
        if(VNG_BUILD_UI)
            add_library(vng_editor_app STATIC examples/editor/app.cpp
                examples/editor/mesh_overlay.cpp examples/editor/viewport_window.cpp)
            target_link_libraries(vng_editor_app PUBLIC vng_editor_ui vng_editor_preview
                vng_ui_opengl vng_example_support vng_example_presentation)
            target_link_libraries(vng_editor_app PRIVATE vng_glad_dependency)
            target_compile_definitions(vng_editor_app PRIVATE
                VNG_EDITOR_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                VNG_EDITOR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                VNG_EDITOR_WORKER_PATH="$<TARGET_FILE:vng_editor_worker>"
                VNG_EDITOR_MESH_PATH="${VNG_EXAMPLE_ASSET_DIR}/colored_cube.vmesh"
                VNG_EXAMPLE_FONT_PATH="${VNG_EXAMPLE_ASSET_DIR}/fonts/DejaVuSans.ttf")
            add_dependencies(vng_editor_app vng_editor_worker)
            vng_target_defaults(vng_editor_app)
            add_executable(vng_editor_demo examples/editor.cpp)
            target_link_libraries(vng_editor_demo PRIVATE vng_editor_app)
            vng_target_defaults(vng_editor_demo)
            if(VNG_BUILD_TESTS)
                add_executable(vng_editor_overlay_tests tests/editor/mesh_overlay_tests.cpp)
                target_link_libraries(vng_editor_overlay_tests PRIVATE vng_editor_app vng_editor_runtime vng_glad_dependency Catch2::Catch2WithMain)
                vng_target_defaults(vng_editor_overlay_tests)
                add_test(NAME vng_editor_overlay_tests COMMAND vng_editor_overlay_tests)
                set_tests_properties(vng_editor_overlay_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS "editor;opengl")
                add_executable(vng_editor_e2e_tests tests/editor/e2e_tests.cpp)
                target_link_libraries(vng_editor_e2e_tests PRIVATE vng_editor_app vng_earth_scene vng_asteroid_scene)
                target_compile_definitions(vng_editor_e2e_tests PRIVATE
                    VNG_EDITOR_MESH_PATH="${VNG_EXAMPLE_ASSET_DIR}/colored_cube.vmesh"
                    VNG_EDITOR_SPACESHIP_PATH="${VNG_EXAMPLE_ASSET_DIR}/spaceship.vmesh"
                    VNG_EDITOR_FLEET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/asteroid_fleet.vscene"
                    VNG_EDITOR_EARTH_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/earth.vscene"
                    VNG_EDITOR_E2E_ARTIFACTS_DIR="${CMAKE_CURRENT_BINARY_DIR}/editor-e2e")
                vng_target_defaults(vng_editor_e2e_tests)
                add_test(NAME vng_editor_e2e_tests COMMAND vng_editor_e2e_tests)
                add_test(NAME vng_editor_fleet_e2e_tests COMMAND vng_editor_e2e_tests --fleet)
                add_test(NAME vng_editor_multiselect_e2e_tests COMMAND vng_editor_e2e_tests --multi)
                add_test(NAME vng_editor_popout_e2e_tests COMMAND vng_editor_e2e_tests --popout)
                add_test(NAME vng_editor_earth_e2e_tests COMMAND vng_editor_e2e_tests --earth)
                set_tests_properties(vng_editor_e2e_tests vng_editor_fleet_e2e_tests vng_editor_multiselect_e2e_tests vng_editor_popout_e2e_tests vng_editor_earth_e2e_tests PROPERTIES
                    SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS "editor;opengl;e2e" RUN_SERIAL TRUE)
            endif()
        endif()
    endif()
    add_executable(vng_sun_demo examples/sun.cpp)
    # Reuse the proven sRGB presentation and screenshot helpers.
    target_link_libraries(vng_sun_demo PRIVATE vng_sun_support
        vng_example_presentation vng_bloom_opengl vng_example_support)
    vng_target_defaults(vng_sun_demo)

    # Same example runner, a different authored scene; no duplicate render loop.
    add_executable(vng_solar_flyby_demo examples/spaceflight.cpp)
    target_link_libraries(vng_solar_flyby_demo PRIVATE
        vng_spaceflight_support vng_sun_support vng_bloom_opengl vng_example_support)
    target_compile_definitions(vng_solar_flyby_demo PRIVATE
        VNG_EXAMPLE_SPACEFLIGHT_PATH="${VNG_EXAMPLE_ASSET_DIR}/solar_flyby.vscene")
    vng_target_defaults(vng_solar_flyby_demo)

    if(VNG_BUILD_TESTS)
        add_executable(vng_sun_render_tests tests/examples/sun_render_tests.cpp
            tests/examples/sun_rotation_tests.cpp
            tests/examples/sun_softening_tests.cpp)
        target_link_libraries(vng_sun_render_tests PRIVATE vng_sun_support
            vng_bloom_opengl vng_glfw_opengl vng_glad_dependency Catch2::Catch2WithMain)
        vng_target_defaults(vng_sun_render_tests)
        add_test(NAME vng_sun_render_tests COMMAND vng_sun_render_tests)
        set_tests_properties(vng_sun_render_tests PROPERTIES SKIP_RETURN_CODE 77 LABELS "examples;opengl")
        add_executable(vng_spaceflight_render_tests tests/examples/spaceflight_render_tests.cpp
            tests/examples/solar_flyby_render_tests.cpp)
        target_link_libraries(vng_spaceflight_render_tests PRIVATE
            vng_spaceflight_support vng_sun_support vng_bloom_opengl vng_glfw_opengl
            vng_glad_dependency Catch2::Catch2WithMain)
        target_compile_definitions(vng_spaceflight_render_tests PRIVATE
            VNG_TEST_SPACEFLIGHT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/spaceflight.vscene"
            VNG_TEST_SOLAR_FLYBY_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/solar_flyby.vscene")
        vng_target_defaults(vng_spaceflight_render_tests)
        add_test(NAME vng_spaceflight_render_tests COMMAND vng_spaceflight_render_tests)
        set_tests_properties(vng_spaceflight_render_tests PROPERTIES
            SKIP_RETURN_CODE 77 LABELS "examples;opengl")
    endif()

    add_executable(vng_rigging_demo examples/rigging.cpp)
    target_link_libraries(vng_rigging_demo PRIVATE vng_rig_opengl vng_example_support)
    vng_target_defaults(vng_rigging_demo)

    add_executable(vng_glow_demo examples/glow.cpp)
    target_link_libraries(vng_glow_demo PRIVATE vng_resources_opengl vng_bloom_opengl vng_example_support)
    if(VNG_BUILD_TEXT)
        target_link_libraries(vng_glow_demo PRIVATE vng_text_opengl)
        target_compile_definitions(vng_glow_demo PRIVATE VNG_GLOW_TEXT=1
            VNG_EXAMPLE_FONT_PATH="${VNG_EXAMPLE_ASSET_DIR}/fonts/DejaVuSans.ttf")
    endif()
    vng_target_defaults(vng_glow_demo)

    if(VNG_BUILD_TEXT)
        file(MAKE_DIRECTORY "${VNG_EXAMPLE_ASSET_DIR}/fonts")
        foreach(asset DejaVuSans.ttf LICENSE.txt)
            configure_file("examples/assets/fonts/${asset}"
                "${VNG_EXAMPLE_ASSET_DIR}/fonts/${asset}" COPYONLY)
        endforeach()
        add_executable(vng_text_demo examples/text.cpp)
        target_link_libraries(vng_text_demo PRIVATE vng_text_opengl vng_example_support)
        target_compile_definitions(vng_text_demo PRIVATE
            VNG_EXAMPLE_FONT_PATH="${VNG_EXAMPLE_ASSET_DIR}/fonts/DejaVuSans.ttf")
        vng_target_defaults(vng_text_demo)
        if(VNG_BUILD_UI)
            add_executable(vng_ui_demo examples/ui.cpp)
            target_link_libraries(vng_ui_demo PRIVATE vng_ui_opengl vng_example_support vng_example_presentation)
            target_compile_definitions(vng_ui_demo PRIVATE
                VNG_EXAMPLE_FONT_PATH="${VNG_EXAMPLE_ASSET_DIR}/fonts/DejaVuSans.ttf")
            vng_target_defaults(vng_ui_demo)
        endif()
    endif()

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
