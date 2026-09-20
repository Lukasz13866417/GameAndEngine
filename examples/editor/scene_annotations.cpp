#include "scene_annotations.hpp"
#include <vng/opengl/gfx_vertex_input.hpp>
#include <vng/opengl/render_state_scope.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>
#include <glad/gl.h>

namespace editor_example {
using namespace vng;
namespace {
struct Position:gfx::Semantic<Vec3>{};
struct Color:gfx::Semantic<Vec4>{};
using Vertex=gfx::Record<Position,Color>;
using Inputs=shader::VertexInputs<Position,Color>;
using Program=opengl::TypedProgram<Mat4>;
}
struct SceneAnnotationRenderer::Impl {
    Program program;
    opengl::VertexArray vao;
    std::optional<opengl::Buffer> buffer;
    std::vector<SceneLine> lines;
};
SceneAnnotationRenderer::SceneAnnotationRenderer(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
SceneAnnotationRenderer::~SceneAnnotationRenderer()=default;
SceneAnnotationRenderer::SceneAnnotationRenderer(SceneAnnotationRenderer&&) noexcept=default;
SceneAnnotationRenderer& SceneAnnotationRenderer::operator=(SceneAnnotationRenderer&&) noexcept=default;
std::expected<SceneAnnotationRenderer,opengl::Diagnostic> SceneAnnotationRenderer::create(opengl::Device& device) {
    auto vs=shader::vertex<Inputs,shader::VertexOutputs<shader::ClipPosition,shader::smooth<Color>>>("scene_annotations",
        [](auto& s,dsl::Float4x4 vp) {
            return s.output(dsl::field<shader::ClipPosition>(vp*dsl::vec4(s.input(Position{}),1.F)),
                            dsl::field<Color>(s.input(Color{})));
        });
    auto fs=shader::fragment<shader::FragmentInputs<shader::smooth<Color>>,shader::FragmentOutputs<shader::Color<0>>>(
        [](auto& s){return s.output(dsl::field<shader::Color<0>>(s.input(Color{})));});
    const auto invalid=[](const auto& error){return opengl::Diagnostic{.code=opengl::ErrorCode::invalid_argument,.message=error.message};};
    if(!vs)return std::unexpected(invalid(vs.error()));
    if(!fs)return std::unexpected(invalid(fs.error()));
    auto linked=shader::link(std::move(*vs),std::move(*fs));
    if(!linked)return std::unexpected(invalid(linked.error()));
    auto program=render::compile_program(device,*linked);
    if(!program)return std::unexpected(program.error());
    auto vao=opengl::VertexArray::create(device);
    if(!vao)return std::unexpected(vao.error());
    return SceneAnnotationRenderer{std::make_unique<Impl>(std::move(*program),std::move(*vao))};
}
std::expected<void,opengl::Diagnostic> SceneAnnotationRenderer::render(opengl::Frame& frame,const render::RenderView& view,
                                                                     std::span<const SceneLine> lines) {
    if(lines.empty())return {};
    auto& p=*impl_;
    auto scope=opengl::RenderStateScope::capture(frame.device(),std::array<u32,1>{0});
    if(!scope)return std::unexpected(scope.error());
    if(!p.buffer || !std::ranges::equal(lines,p.lines)) {
        std::vector<Vertex> vertices(lines.size()*2);
        for(std::size_t i=0;i<lines.size();++i) {
            vertices[2*i].set(Position{},lines[i].from);vertices[2*i+1].set(Position{},lines[i].to);
            for(unsigned j=0;j<2;++j)vertices[2*i+j].set(Color{},lines[i].color);
        }
        auto buffer=opengl::Buffer::from_bytes(frame.device(),std::as_bytes(std::span{vertices}));
        if(!buffer)return std::unexpected(buffer.error());
        p.buffer=std::move(*buffer);
        using Layout=gfx::VertexLayout<gfx::Stream<Vertex,gfx::PerVertex>>;
        const std::array streams{opengl::ResolvedStreamBuffer{0,&*p.buffer,0}};
        if(auto r=opengl::configure_vertex_input(p.vao,gfx::resolve_vertex_input<Inputs,Layout>(),streams);!r)return r;
        p.lines.assign(lines.begin(),lines.end());
    }
    auto context=frame.render_context();auto graphics=context.graphics_state();
    // Opaque lines also occlude farther annotation lines at crossings. This
    // is the final color pass; the next scene frame clears the shared depth.
    if(auto r=graphics.set(render::DepthState{true,true,render::DepthCompare::less_equal});!r)return r;
    if(auto r=graphics.set(render::BlendMode::disabled);!r)return r;
    if(auto r=graphics.set(render::CullMode::none);!r)return r;
    if(auto r=graphics.set(opengl::PolygonMode::fill);!r)return r;
    if(auto r=context.run(p.program,view.camera()->view_projection);!r)return r;
    if(auto r=p.vao.bind();!r)return r;
    GLfloat width{};glGetFloatv(GL_LINE_WIDTH,&width);glLineWidth(1);
    auto drawn=frame.device().draw_arrays_instanced(opengl::Primitive::lines,0,static_cast<u32>(lines.size()*2));
    glLineWidth(width);
    if(!drawn)return drawn;
    return scope->restore();
}
}
