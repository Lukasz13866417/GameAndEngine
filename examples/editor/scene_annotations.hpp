#pragma once
#include "annotation_geometry.hpp"
#include <vng/opengl/renderer.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/render/view.hpp>
#include <memory>

namespace editor_example {
class SceneAnnotationRenderer final : public vng::opengl::Renderer<SceneLine> {
public:
    static std::expected<SceneAnnotationRenderer,vng::opengl::Diagnostic> create(vng::opengl::Device&);
    ~SceneAnnotationRenderer();
    SceneAnnotationRenderer(SceneAnnotationRenderer&&) noexcept;
    SceneAnnotationRenderer& operator=(SceneAnnotationRenderer&&) noexcept;
    std::expected<void,vng::opengl::Diagnostic> render(vng::opengl::Frame&,const vng::render::RenderView&,
                                                     std::span<const SceneLine>);
private:
    struct Impl;
    explicit SceneAnnotationRenderer(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
