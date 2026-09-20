#include "mesh_overlay.hpp"
#include "preview_values.hpp"
#include <vng/opengl/gfx_vertex_input.hpp>
#include <vng/opengl/render_state_scope.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>
#include <glad/gl.h>
#include <algorithm>

namespace editor_example {
using namespace vng;
namespace {
struct Position : gfx::Semantic<Vec3> {};
using Inputs=shader::VertexInputs<Position>;
using Program=opengl::TypedProgram<Mat4,Mat4,f32,Vec4>;
opengl::Diagnostic invalid(std::string message) {
    return {.code=opengl::ErrorCode::invalid_argument,.message=std::move(message)};
}
auto make_program(opengl::Device& device)->std::expected<Program,opengl::Diagnostic> {
    auto vs=shader::vertex<Inputs,shader::VertexOutputs<shader::ClipPosition>>("mesh_overlay",
        [](auto& s,dsl::Float4x4 model,dsl::Float4x4 view,dsl::Float bias) {
            const auto p=view*model*dsl::vec4(s.input(Position{}),1.0F);
            return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(p.x(),p.y(),p.z()-bias*p.w(),p.w())));
        });
    auto fs=shader::fragment<shader::FragmentInputs<>,shader::FragmentOutputs<shader::Color<0>>>("mesh_overlay",
        [](auto& s,dsl::Float4 color) {
            return s.output(dsl::field<shader::Color<0>>(dsl::vec4(color.xyz()*color.w(),color.w())));
        });
    if(!vs) return std::unexpected(invalid(vs.error().message));
    if(!fs) return std::unexpected(invalid(fs.error().message));
    auto linked=shader::link(std::move(*vs),std::move(*fs));
    if(!linked) return std::unexpected(invalid(linked.error().message));
    return render::compile_program(device,*linked);
}
}
struct MeshOverlay::Impl {
    Impl(Program shader,opengl::VertexArray array):program(std::move(shader)),vao(std::move(array)) {}
    Program program;
    opengl::VertexArray vao;
    std::optional<opengl::Buffer> positions, indices, selection_indices;
    std::vector<Vec3> points;
    u64 document{}, topology{}, selection{}, visibility{};
    Stats stats{};
    struct Range {u32 first{},count{};bool selected{};};
    Range surface, edges, vertices, centers, selected_faces, selected_edges, selected_points;

    auto update(const opengl::Device& device,const State& state,const MeshTools& tools)
        -> std::expected<void,opengl::Diagnostic> {
        const auto* mesh=editable_mesh(state);
        if(!mesh) return {};
        const bool rebuild=topology!=tools.topology_revision();
        if(rebuild || document!=state.document.revision) {
            std::vector<Vec3> next;
            next.reserve(mesh->size()+mesh->document().faces.size());
            for(u32 i=0;i<mesh->size();++i) next.push_back(mesh->position(i));
            for(const auto& f:mesh->document().faces) {
                const auto a=next[f[0]],b=next[f[1]],c=next[f[2]];
                next.push_back({(a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3});
            }
            static_assert(sizeof(Vec3)==12);
            if(!positions || positions->size()!=next.size()*sizeof(Vec3)) {
                auto buffer=opengl::Buffer::from_bytes(device,std::as_bytes(std::span{next}),opengl::BufferStorage::dynamic);
                if(!buffer) return std::unexpected(buffer.error());
                positions=std::move(*buffer);
                using Layout=gfx::VertexLayout<gfx::Stream<gfx::Record<Position>,gfx::PerVertex>>;
                const std::array bindings{opengl::ResolvedStreamBuffer{0,&*positions,0}};
                if(auto r=opengl::configure_vertex_input(vao,gfx::resolve_vertex_input<Inputs,Layout>(),bindings);!r) return r;
                ++stats.position_uploads;
            } else {
                // Only changed positions/centers are uploaded during a drag;
                // attributes and element buffers remain resident.
                for(std::size_t i=0;i<next.size();) {
                    if(i<points.size() && next[i]==points[i]) {++i;continue;}
                    const auto first=i++;
                    while(i<next.size() && (i>=points.size() || next[i]!=points[i])) ++i;
                    if(auto r=positions->write(first*sizeof(Vec3),std::as_bytes(std::span{next}.subspan(first,i-first)));!r) return r;
                    ++stats.position_uploads;
                }
            }
            points=std::move(next);document=state.document.revision;
        }
        if(rebuild || !indices || visibility!=tools.visibility_revision()) {
            std::vector<u32> values;
            const auto range=[&](auto emit) { const auto first=static_cast<u32>(values.size());emit();return Range{first,static_cast<u32>(values.size())-first}; };
            surface=range([&]{for(u32 i=0;i<mesh->document().faces.size();++i) if(tools.visible(MeshSelectMode::face,i))
                for(auto v:mesh->document().faces[i].vertices) values.push_back(v);});
            edges=range([&]{for(u32 i=0;i<tools.all_edges().size();++i) if(tools.visible(MeshSelectMode::edge,i))
                {const auto e=tools.all_edges()[i];values.push_back(e[0]);values.push_back(e[1]);}});
            vertices=range([&]{for(u32 i=0;i<mesh->size();++i) if(tools.visible(MeshSelectMode::vertex,i)) values.push_back(i);});
            centers=range([&]{for(u32 i=0;i<mesh->document().faces.size();++i) if(tools.visible(MeshSelectMode::face,i)) values.push_back(static_cast<u32>(mesh->size())+i);});
            if(values.empty()) values.push_back(0);
            auto buffer=opengl::Buffer::from_bytes(device,std::as_bytes(std::span{values}));
            if(!buffer) return std::unexpected(buffer.error());
            indices=std::move(*buffer);++stats.index_uploads;++stats.topology_uploads;
            visibility=tools.visibility_revision();
        }
        if(rebuild || !selection_indices || selection!=tools.selection_revision()) {
            std::vector<u32> values;
            const auto range=[&](auto emit) { const auto first=static_cast<u32>(values.size());emit();return Range{first,static_cast<u32>(values.size())-first,true}; };
            selected_faces=range([&]{if(tools.mode()==MeshSelectMode::face) for(auto id:tools.selected())
                if(id<mesh->document().faces.size()) for(auto v:mesh->document().faces[id].vertices) values.push_back(v);});
            selected_edges=range([&]{for(auto e:tools.edges(*mesh)) {values.push_back(e[0]);values.push_back(e[1]);}});
            selected_points=range([&]{
                if(tools.mode()==MeshSelectMode::vertex) {auto ids=tools.vertices(*mesh);values.insert(values.end(),ids.begin(),ids.end());}
                else if(tools.mode()==MeshSelectMode::face) for(auto id:tools.selected()) if(id<mesh->document().faces.size()) values.push_back(static_cast<u32>(mesh->size())+id);
            });
            // Empty selections need no indices, but Buffer storage must be nonempty.
            if(values.empty())values.push_back(0);
            auto buffer=opengl::Buffer::from_bytes(device,std::as_bytes(std::span{values}));
            if(!buffer) return std::unexpected(buffer.error());
            selection_indices=std::move(*buffer);++stats.index_uploads;++stats.selection_uploads;
            selection=tools.selection_revision();
        }
        topology=tools.topology_revision();
        return {};
    }
};
MeshOverlay::MeshOverlay(std::unique_ptr<Impl> p):impl_(std::move(p)) {}
MeshOverlay::MeshOverlay(MeshOverlay&&) noexcept=default;
MeshOverlay& MeshOverlay::operator=(MeshOverlay&&) noexcept=default;
MeshOverlay::~MeshOverlay()=default;
auto MeshOverlay::create(opengl::Device& device)->std::expected<MeshOverlay,opengl::Diagnostic> {
    auto program=make_program(device);if(!program) return std::unexpected(program.error());
    auto vao=opengl::VertexArray::create(device);if(!vao) return std::unexpected(vao.error());
    return MeshOverlay{std::make_unique<Impl>(std::move(*program),std::move(*vao))};
}
MeshOverlay::Stats MeshOverlay::stats() const {return impl_->stats;}
auto MeshOverlay::render(opengl::Frame& frame,const State& state,const MeshTools& tools,
    ui::Rect bounds,Vec2 logical,Extent2D camera_extent,const gfx::Camera& camera)
    ->std::expected<void,opengl::Diagnostic> {
    auto& p=*impl_;p.stats={};
    // No uploads, depth pass, or raster-state changes in the clean preview.
    // In particular, do not disable wireframe supplied by the mesh renderer.
    if(!tools.component_mode()) return {};
    const auto preview=preview_mesh(state,state.viewport.time);
    // Menus own input, not mesh visibility. The viewport composites them after
    // this pass so editing geometry stays visible behind their controls.
    if(!preview || !preview->settings.visible || bounds.width<=0 || bounds.height<=0) return {};
    auto view=camera.snapshot(camera_extent);if(!view) return std::unexpected(invalid(view.error().message));
    auto scope=opengl::RenderStateScope::capture(frame.device(),std::array<u32,1>{0});
    if(!scope) return std::unexpected(scope.error());
    if(auto r=p.update(frame.device(),state,tools);!r) return r;
    if(auto r=frame.device().set_standard_raster_state();!r) return r;
    const auto extent=frame.extent();const auto sx=static_cast<f32>(extent.width)/logical.x,sy=static_cast<f32>(extent.height)/logical.y;
    const auto x=static_cast<GLint>(std::lround(bounds.x*sx)), y=static_cast<GLint>(std::lround(static_cast<f32>(extent.height)-(bounds.y+bounds.height)*sy));
    const auto w=static_cast<GLsizei>(std::lround(bounds.width*sx)),h=static_cast<GLsizei>(std::lround(bounds.height*sy));
    // A private depth pass uses current local geometry, not an asynchronous
    // worker depth image. Navigation and vertex drags therefore stay immediate.
    glViewport(x,y,w,h);glEnable(GL_SCISSOR_TEST);glScissor(x,y,w,h);
    glDepthMask(GL_TRUE);const GLfloat clear=1;glClearBufferfv(GL_DEPTH,0,&clear);
    struct PointScope {
        GLfloat size{}, line_width{};
        GLboolean programmable{}, smooth_lines{};
        PointScope() {
            glGetFloatv(GL_POINT_SIZE,&size);glGetFloatv(GL_LINE_WIDTH,&line_width);
            programmable=glIsEnabled(GL_PROGRAM_POINT_SIZE);smooth_lines=glIsEnabled(GL_LINE_SMOOTH);
            glDisable(GL_PROGRAM_POINT_SIZE);glDisable(GL_LINE_SMOOTH);glLineWidth(1);
        }
        ~PointScope() {
            glPointSize(size);glLineWidth(line_width);
            if(programmable) glEnable(GL_PROGRAM_POINT_SIZE);
            if(smooth_lines) glEnable(GL_LINE_SMOOTH);
        }
    } point_scope;
    auto context=frame.render_context();auto graphics=context.graphics_state();
    if(auto r=graphics.set(render::CullMode::none);!r) return r;
    if(auto r=graphics.set(opengl::PolygonMode::fill);!r) return r;
    if(auto r=graphics.set(render::BlendMode::premultiplied_alpha);!r) return r;
    if(auto r=p.vao.bind();!r) return r;
    std::optional<bool> selected_indices;
    const auto draw=[&](Impl::Range range,opengl::Primitive primitive,Vec4 tint,f32 bias)->std::expected<void,opengl::Diagnostic> {
        if(!range.count) return {};
        if(selected_indices!=range.selected) {
            if(auto r=p.vao.set_element_buffer(range.selected?*p.selection_indices:*p.indices);!r)return r;
            selected_indices=range.selected;
        }
        if(auto r=context.run(p.program,mesh_transform(state,preview->target.blueprint,preview->transform),view->view_projection,bias,tint);!r) return r;
        ++p.stats.draws;
        return frame.device().draw_elements_instanced(primitive,opengl::IndexFormat::u32,range.count,range.first*sizeof(u32));
    };
    if(auto r=graphics.set(render::DepthState{true,true});!r) return r;
    if(auto r=frame.device().set_color_write_mask(0,{false,false,false,false});!r) return r;
    if(auto r=draw(p.surface,opengl::Primitive::triangles,{0,0,0,0},0);!r) return r;
    if(auto r=frame.device().set_color_write_mask(0,{true,true,true,true});!r) return r;
    if(auto r=graphics.set(render::DepthState{!tools.xray(),false,render::DepthCompare::less_equal});!r) return r;
    constexpr Vec4 edge{.015F,.02F,.025F,.38F},marker{.025F,.035F,.045F,.8F},gold{1,.5F,.015F,1};
    if(auto r=draw(p.selected_faces,opengl::Primitive::triangles,{1,.35F,.01F,.17F},.00001F);!r) return r;
    if(auto r=draw(p.edges,opengl::Primitive::lines,edge,.00002F);!r) return r;
    if(auto r=draw(p.selected_edges,opengl::Primitive::lines,gold,.00003F);!r) return r;
    const auto pixel_scale=std::min(sx,sy);
    if(tools.mode()==MeshSelectMode::vertex) {
        // Constant logical-pixel dots, distinct from the thin edge overlay.
        // A narrow light rim keeps dark unselected vertices visible on dark
        // hulls too. Both passes use the same depth test (including X-ray).
        glPointSize(6*pixel_scale);
        if(auto r=draw(p.vertices,opengl::Primitive::points,{.45F,.45F,.45F,1},.00003F);!r) return r;
        glPointSize(4*pixel_scale);
        if(auto r=draw(p.vertices,opengl::Primitive::points,{.008F,.008F,.008F,1},.00003F);!r) return r;
        glPointSize(8*pixel_scale);
        if(auto r=draw(p.selected_points,opengl::Primitive::points,{.008F,.008F,.008F,1},.00004F);!r) return r;
        glPointSize(6*pixel_scale);
        if(auto r=draw(p.selected_points,opengl::Primitive::points,gold,.00004F);!r) return r;
    } else if(tools.mode()==MeshSelectMode::face) {
        glPointSize(2*pixel_scale);
        if(auto r=draw(p.centers,opengl::Primitive::points,marker,.00003F);!r) return r;
        glPointSize(5*pixel_scale);
        if(auto r=draw(p.selected_points,opengl::Primitive::points,gold,.00004F);!r) return r;
    }
    if(glGetError()!=GL_NO_ERROR) return std::unexpected(invalid("Mesh overlay OpenGL state/draw failed"));
    return scope->restore();
}
}
