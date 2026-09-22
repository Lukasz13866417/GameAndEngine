#include <vng/resources_opengl/mesh_renderer.hpp>
#include <vng/providers/image.hpp>
#include <vng/providers/program.hpp>
#include <vng/shader/shader.hpp>
#include <vng/opengl/instance_buffer.hpp>
#include "../opengl/gl_error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace vng::opengl {
namespace {
using resources::Result;
using resources::into_result;
using namespace render::surface;
using Inputs = shader::VertexInputs<gfx::Position, gfx::TexCoord<>, gfx::Color,
    ModelColumn<0>, ModelColumn<1>, ModelColumn<2>, ModelColumn<3>, Tint, Emission>;
using Varyings = shader::VertexOutputs<shader::ClipPosition,
    shader::smooth<gfx::TexCoord<>>, shader::smooth<gfx::Color>, shader::flat<Tint>, shader::flat<Emission>>;
using FragmentInputs = shader::FragmentInputs<shader::smooth<gfx::TexCoord<>>, shader::smooth<gfx::Color>,
    shader::flat<Tint>, shader::flat<Emission>>;
using Outputs = shader::FragmentOutputs<shader::Color<0>>;

resources::Diagnostic invalid(std::string message)
{ return {.code = resources::ErrorCode::invalid_argument, .message = std::move(message)}; }
resources::Diagnostic missing(std::string role)
{ return {.code = resources::ErrorCode::no_provider, .message = "No provider configured for " + role}; }

Result<shader::GraphicsProgram> mesh_shaders()
{
    auto vertex = shader::vertex<Inputs,Varyings>("surface_vertex", [](auto& s) {
        const auto model = dsl::make<Mat4>(s.input(ModelColumn<0>{}), s.input(ModelColumn<1>{}),
            s.input(ModelColumn<2>{}), s.input(ModelColumn<3>{}));
        const auto world = model * dsl::vec4(s.input(gfx::Position{}), 1.0F);
        return s.output(dsl::field<shader::ClipPosition>(s.camera().project(world.xyz())),
            dsl::field<gfx::TexCoord<>>(s.input(gfx::TexCoord<>{})),
            dsl::field<gfx::Color>(s.input(gfx::Color{})),
            dsl::field<Tint>(s.input(Tint{})), dsl::field<Emission>(s.input(Emission{})));
    });
    if (!vertex) return std::unexpected(resources::to_diagnostic(vertex.error()));
    auto fragment = shader::fragment<FragmentInputs,Outputs>("surface_fragment", [](auto& s) {
        const auto tint = s.input(Tint{});
        const auto emission = s.input(Emission{});
        const auto texel = s.template sample_2d<0>(s.input(gfx::TexCoord<>{}));
        const auto color = texel * tint * s.input(gfx::Color{});
        return s.output(dsl::field<shader::Color<0>>(dsl::vec4(color.xyz() * (emission + 1.0F), color.w())));
    });
    if (!fragment) return std::unexpected(resources::to_diagnostic(fragment.error()));
    return into_result(shader::link(std::move(*vertex), std::move(*fragment)));
}

Result<void> validate_program(const SurfaceProgram& program, const Device& device)
{
    if (!program.belongs_to(device)) return std::unexpected(invalid("Program belongs to another device or is empty"));
    const auto* source = program.generated_source();
    if (!source) return std::unexpected(invalid("Mesh renderer requires a DSL program with interface metadata"));
    for (const auto& parameter : source->parameters)
        if (parameter.kind != shader::ParameterKind::camera_view_projection)
            return std::unexpected(invalid("Surface programs require per-instance attributes, not uniform draw arguments"));
    const auto& inputs = source->vertex.interface.inputs;
    bool missing_instance = false;
    Instance::for_each_field([&](auto field) {
        using Semantic = typename decltype(field)::semantic_type;
        missing_instance |= std::ranges::find(inputs, std::type_index{typeid(Semantic)},
            &glsl::InterfaceVariable::semantic_type) == inputs.end();
    });
    if (missing_instance)
        return std::unexpected(invalid("Surface program requires all ModelColumn<0..3>, Tint and Emission instance semantics"));
    for (const auto& input : inputs) {
        bool compatible = false;
        const auto inspect = [&]<class Record>() {
            Record::for_each_field([&](auto field) {
                using Semantic = typename decltype(field)::semantic_type;
                using Value = gfx::semantic_value_t<Semantic>;
                constexpr std::string_view type = std::same_as<Value, Vec4> ? "vec4" :
                    std::same_as<Value, Vec3> ? "vec3" : std::same_as<Value, Vec2> ? "vec2" : "float";
                if (input.semantic_type == typeid(Semantic) && input.glsl_type == type && input.location)
                    compatible = true;
            });
        };
        inspect.template operator()<render::SurfaceVertex>();
        inspect.template operator()<Instance>();
        if (!compatible) return std::unexpected(invalid("Unsupported surface vertex semantic: " + input.semantic_name));
    }
    for (auto binding : source->texture_bindings)
        if (binding != 0) return std::unexpected(invalid("Mesh renderer supplies only texture slot 0"));
    if (!source->matrix_buffer_bindings.empty())
        return std::unexpected(invalid("Mesh renderer does not supply matrix-buffer resources"));
    if (source->fragment.interface.outputs.size() != 1
        || source->fragment.interface.outputs.front().location != 0
        || source->fragment.interface.outputs.front().glsl_type != "vec4")
        return std::unexpected(invalid("Mesh renderer requires one vec4 color output at location 0"));
    return {};
}

Result<void> validate_image(const SharedImage& image, const Device& device)
{
    if (!image || !image->belongs_to(device)) return std::unexpected(invalid("Albedo is empty or belongs to another device"));
    switch (image->format()) {
    case ImageFormat::r8: case ImageFormat::rgba8: case ImageFormat::srgb8_alpha8:
    case ImageFormat::rgba16f: case ImageFormat::rgba32f: return {};
    default: return std::unexpected(invalid("Albedo requires a normalized or floating color image"));
    }
}

struct Geometry {
    render::MeshSource source;
    GpuMesh<render::SurfaceVertex> gpu;
};

Result<std::shared_ptr<Geometry>> prepare_geometry(Device& device, render::MeshSource source, const SurfaceProgram& program)
{
    if (!source) return std::unexpected(invalid("Mesh source is empty"));
    // Even shared_ptr<const Mesh> may alias a mutable shared_ptr elsewhere.
    // Freeze and validate at the adoption boundary, keeping GPU and provenance
    // consistent regardless of what the original caller edits afterward.
    auto frozen=render::mesh_source(*source,render::MeshFields{});
    if (!frozen) return std::unexpected(std::move(frozen.error()));
    source=std::move(*frozen);
    auto gpu = into_result(upload_mesh(device, *source));
    if (!gpu) return std::unexpected(std::move(gpu.error()));
    // validate_program checked the combined mesh/instance contract before any
    // adoption. The inferred VAO is created with the renderer's instance buffer
    // on first draw; a mesh-only VAO cannot represent this contract.
    if (auto valid = validate_program(program, device); !valid)
        return std::unexpected(std::move(valid.error()));
    return std::make_shared<Geometry>(Geometry{std::move(source), std::move(*gpu)});
}

// Texture slots are borrowed only for this synchronous submission. Typed
// instance attributes belong to the renderer, not a global scratch binding.
struct BindingScope {
    GLint image{}, sampler{};
    BindingScope() {
        glGetIntegeri_v(GL_TEXTURE_BINDING_2D, 0, &image);
        glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &sampler);
    }
    ~BindingScope() {
        GLint active{}; glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(image));
        glActiveTexture(static_cast<GLenum>(active)); glBindSampler(0,static_cast<GLuint>(sampler));
    }
};
} // namespace

struct MeshRenderer::Impl {
    MeshProvider mesh_provider;
    ProgramProvider program_provider;
    AlbedoProvider albedo_provider;
    std::shared_ptr<SurfaceProgram> program;
    std::shared_ptr<Geometry> geometry;
    SharedImage albedo;
    std::vector<render::surface::Instance> records{};
    InstanceBuffer<render::surface::Instance> instances{};
    render::SurfaceRenderStats stats{};
    u64 revision{1};
};

MeshRenderer::~MeshRenderer() = default;
MeshRenderer::Update::Update(std::weak_ptr<Impl> owner, Device& device, u64 generation)
    : owner_(std::move(owner)), device_(&device), generation_(generation) {}
MeshRenderer::Update MeshRenderer::update(Device& device)
{ return Update{impl_,device,revision()}; }

MeshRenderer::Update& MeshRenderer::Update::mesh(MeshProvider source)
{ mesh_changed_=true; mesh_ready_.reset(); mesh_provider_=std::move(source); return *this; }
MeshRenderer::Update& MeshRenderer::Update::mesh(render::MeshSource ready)
{ mesh_changed_=true; mesh_ready_=std::move(ready); mesh_provider_={}; return *this; }
MeshRenderer::Update& MeshRenderer::Update::program(ProgramProvider source)
{ program_changed_=true; program_ready_.reset(); program_provider_=std::move(source); return *this; }
MeshRenderer::Update& MeshRenderer::Update::program(SurfaceProgram&& ready)
{ program_changed_=true; program_ready_.emplace(std::move(ready)); program_provider_={}; return *this; }
MeshRenderer::Update& MeshRenderer::Update::albedo(SharedImage ready)
{ albedo_changed_=true; albedo_ready_=std::move(ready); albedo_provider_={}; return *this; }

Result<void> MeshRenderer::Update::commit()
{
    auto owner = owner_.lock();
    if (!owner || committed_ || owner->revision != generation_)
        return std::unexpected(invalid("Resource update is expired, already attempted, or based on an obsolete revision"));
    if (auto allowed = into_result(device_->require_resource_update("MeshRenderer::update")); !allowed) return allowed;
    if (!owner->program->belongs_to(*device_)) return std::unexpected(invalid("Renderer belongs to another device"));
    committed_=true;
    // A provider may hold a reference to this Update and call its setters.
    // Retain every input before invoking user code: the executing recipe must
    // stay alive and the installed recipe must match the candidate it made.
    const auto change_mesh=mesh_changed_;
    const auto change_program=program_changed_;
    const auto change_albedo=albedo_changed_;
    auto mesh_source=mesh_provider_;
    auto program_source=program_provider_;
    auto albedo_source=albedo_provider_;
    const auto mesh_ready=mesh_ready_;
    auto program_ready=std::move(program_ready_);
    const auto albedo_ready=albedo_ready_;
    auto* device=device_;
    const auto generation=generation_;
    if (!change_mesh && !change_program && !change_albedo) return {};
    auto next_program = owner->program;
    auto next_geometry = owner->geometry;
    auto next_albedo = owner->albedo;
    if (change_program) {
        auto candidate = program_ready ? Result<SurfaceProgram>{std::move(*program_ready)} : program_source.provide(*device);
        if (!candidate) return std::unexpected(std::move(candidate.error()).note("program"));
        if (auto valid=validate_program(*candidate,*device); !valid) return valid;
        next_program=std::make_shared<SurfaceProgram>(std::move(*candidate));
    }
    if (change_mesh || change_program) {
        auto source = change_mesh ? (mesh_ready ? Result<render::MeshSource>{*mesh_ready} : mesh_source.provide(*device))
                                 : Result<render::MeshSource>{next_geometry->source};
        if (!source) return std::unexpected(std::move(source.error()).note("mesh"));
        auto candidate=prepare_geometry(*device,std::move(*source),*next_program);
        if (!candidate) return std::unexpected(std::move(candidate.error()));
        next_geometry=std::move(*candidate);
    }
    if (change_albedo) {
        auto candidate = albedo_ready ? Result<SharedImage>{*albedo_ready} : albedo_source.provide(*device);
        if (!candidate) return std::unexpected(std::move(candidate.error()).note("albedo"));
        if (auto valid=validate_image(*candidate,*device); !valid) return valid;
        next_albedo=std::move(*candidate);
    }
    // All allocations, uploads and compatibility checks are complete. The
    // following owning-handle moves cannot fail or expose an intermediate draw.
    if (auto allowed=into_result(device->require_resource_update("MeshRenderer::commit")); !allowed) return allowed;
    if (owner->revision != generation)
        return std::unexpected(invalid("A provider changed the owner while an update was being prepared"));
    owner->geometry=std::move(next_geometry); owner->program=std::move(next_program); owner->albedo=std::move(next_albedo);
    if (change_mesh) owner->mesh_provider=std::move(mesh_source);
    if (change_program) owner->program_provider=std::move(program_source);
    if (change_albedo) owner->albedo_provider=std::move(albedo_source);
    ++owner->revision;
    return {};
}

Result<resources::ReloadReport> MeshRenderer::reload(Device& device)
{
    if (!impl_) return std::unexpected(invalid("Renderer is empty"));
    resources::ReloadReport report;
    auto pending=update(device);
    if (impl_->mesh_provider) { pending.mesh(impl_->mesh_provider); report.refreshed.push_back("mesh"); }
    else report.retained.push_back("mesh");
    if (impl_->program_provider) { pending.program(impl_->program_provider); report.refreshed.push_back("program"); }
    else report.retained.push_back("program");
    if (impl_->albedo_provider) { pending.albedo(impl_->albedo_provider); report.refreshed.push_back("albedo"); }
    else report.retained.push_back("albedo");
    if (auto result=pending.commit(); !result) return std::unexpected(std::move(result.error()));
    return report;
}
Result<void> MeshRenderer::reload_mesh(Device& d) {
    if (!impl_ || !impl_->mesh_provider) return std::unexpected(missing("mesh"));
    return reload_mesh(d,impl_->mesh_provider);
}
Result<void> MeshRenderer::reload_mesh(Device& d, MeshProvider p) { auto u=update(d); u.mesh(std::move(p)); return u.commit(); }
Result<void> MeshRenderer::reload_program(Device& d) {
    if (!impl_ || !impl_->program_provider) return std::unexpected(missing("program"));
    return reload_program(d,impl_->program_provider);
}
Result<void> MeshRenderer::reload_program(Device& d, ProgramProvider p) { auto u=update(d); u.program(std::move(p)); return u.commit(); }
Result<void> MeshRenderer::reload_albedo(Device& d) {
    if (!impl_ || !impl_->albedo_provider) return std::unexpected(missing("albedo"));
    return reload_albedo(d,impl_->albedo_provider);
}
Result<void> MeshRenderer::replace_mesh(Device& d, render::MeshSource p) { auto u=update(d); u.mesh(std::move(p)); return u.commit(); }
Result<void> MeshRenderer::replace_program(Device& d, SurfaceProgram&& p) { auto u=update(d); u.program(std::move(p)); return u.commit(); }
Result<void> MeshRenderer::replace_albedo(Device& d, SharedImage p) { auto u=update(d); u.albedo(std::move(p)); return u.commit(); }

Result<void> MeshRenderer::render(Frame& frame, const render::RenderView& view,
    std::span<const render::SurfaceDraw> draws)
{
    if (impl_) impl_->stats = {};
    if (!impl_ || !frame.active() || frame.extent()!=view.extent() || !view.camera())
        return std::unexpected(invalid("Mesh rendering requires a ready owner, active frame, matching extent and camera"));
    if (!impl_->program->belongs_to(frame.device())) return std::unexpected(invalid("Renderer belongs to another device"));
    if (auto current=into_result(frame.device().require_current("MeshRenderer::render")); !current) return current;
    if (frame.uses_image(*impl_->albedo))
        return std::unexpected(invalid("Albedo aliases an attachment of the active frame"));
    constexpr auto max_instances = std::min<std::size_t>(std::numeric_limits<i32>::max(),
        std::numeric_limits<std::ptrdiff_t>::max() / render::surface::Instance::stride);
    if (draws.size() > max_instances)
        return std::unexpected(invalid("Surface draw count exceeds instance-buffer capacity"));
    // Validate every ticket before emitting a partial batch.
    for (const auto& draw:draws) {
        if (draw.cull != render::CullMode::none && draw.cull != render::CullMode::front
            && draw.cull != render::CullMode::back)
            return std::unexpected(invalid("Unknown culling mode"));
        for (const auto& column:draw.transform.columns) for (u32 c=0;c<4;++c)
            if (!std::isfinite(column[c])) return std::unexpected(invalid("Object matrix must be finite"));
        for (u32 c=0;c<4;++c) if (!std::isfinite(draw.tint[c]) || draw.tint[c]<0)
            return std::unexpected(invalid("Tint must be finite and nonnegative"));
        if (!std::isfinite(draw.emission) || draw.emission<0)
            return std::unexpected(invalid("Emission must be finite and nonnegative"));
    }
    if (draws.empty()) return {};
    impl_->records.clear(); impl_->records.reserve(draws.size());
    for (const auto& draw : draws) {
        auto& record = impl_->records.emplace_back();
        record.set(ModelColumn<0>{}, draw.transform[0]); record.set(ModelColumn<1>{}, draw.transform[1]);
        record.set(ModelColumn<2>{}, draw.transform[2]); record.set(ModelColumn<3>{}, draw.transform[3]);
        record.set(Tint{}, draw.tint); record.set(Emission{}, draw.emission);
    }
    BindingScope scope;
    if (auto bound=into_result(detail::checked_gl_call("bind surface resources",[&] {
        glBindSampler(0,0); glBindTextureUnit(0,impl_->albedo->native_handle());
    })); !bound) return bound;
    auto commands=frame.render_context(); auto graphics=commands.graphics_state();
    if (auto blend=into_result(graphics.set(render::BlendMode::disabled)); !blend) return blend;
    if (auto state=into_result(graphics.set(render::DepthCompare::less)); !state) return state;
    if (auto state=into_result(graphics.set(render::FrontFace::counter_clockwise)); !state) return state;
    if (auto state=into_result(graphics.set(PolygonMode::fill)); !state) return state;
    if (auto run=into_result(commands.run(*impl_->program)); !run) return run;
    if (auto camera=into_result(commands.view(view)); !camera) return camera;
    for (std::size_t first=0; first<draws.size();) {
        const auto& draw = draws[first];
        auto end = first+1;
        while (end<draws.size() && draws[end].depth_test==draw.depth_test &&
               draws[end].depth_write==draw.depth_write && draws[end].cull==draw.cull) ++end;
        if (auto uploaded=into_result(impl_->instances.update(frame.device(),
            std::span<const render::surface::Instance>{impl_->records}.subspan(first,end-first))); !uploaded) return uploaded;
        if (auto state=into_result(graphics.set(render::DepthTest{draw.depth_test})); !state) return state;
        if (auto state=into_result(graphics.set(render::DepthWrite{draw.depth_write})); !state) return state;
        if (auto state=into_result(graphics.set(draw.cull)); !state) return state;
        if (auto drawn=into_result(commands.draw(impl_->geometry->gpu, impl_->instances)); !drawn) return drawn;
        ++impl_->stats.draw_calls; impl_->stats.instances += end-first;
        first=end;
    }
    return {};
}

const render::SurfaceMesh& MeshRenderer::source_mesh() const {
    if (!impl_) throw std::logic_error("Renderer is empty");
    return *impl_->geometry->source;
}
const SurfaceProgram& MeshRenderer::program() const {
    if (!impl_) throw std::logic_error("Renderer is empty");
    return *impl_->program;
}
const Image2D& MeshRenderer::albedo() const {
    if (!impl_) throw std::logic_error("Renderer is empty");
    return *impl_->albedo;
}
u64 MeshRenderer::revision() const noexcept { return impl_ ? impl_->revision : 0; }
render::SurfaceRenderStats MeshRenderer::stats() const noexcept { return impl_ ? impl_->stats : render::SurfaceRenderStats{}; }

MeshRendererBuilder::MeshRendererBuilder(Device& device) : device_(&device)
{
    // Build IR once per builder. Repeated builds/reloads compile the retained
    // neutral program, never replay a shader lambda for each backend.
    auto ir=mesh_shaders();
    program_=resources::provider([ir=std::move(ir)](Device& d)->Result<SurfaceProgram> {
        if (!ir) return std::unexpected(ir.error());
        return into_result(render::compile_program(d,*ir));
    });
    gfx::ImageData white{.extent={1,1},.pixels=std::vector<std::byte>(4,std::byte{255})};
    albedo_=detail::albedo_provider(providers::texture(std::move(white),{
        .format=gfx::ImageFormat::rgba8,.generate_mipmaps=false}));
}

Result<MeshRenderer> MeshRendererBuilder::build() const
{
    const auto mesh_source=mesh_;
    const auto program_source=program_;
    const auto albedo_source=albedo_;
    auto* device=device_;
    if (auto current=into_result(device->require_resource_update("MeshRendererBuilder::build")); !current)
        return std::unexpected(std::move(current.error()));
    auto program=program_source.provide(*device);
    if (!program) return std::unexpected(std::move(program.error()).note("program"));
    if (auto valid=validate_program(*program,*device); !valid) return std::unexpected(std::move(valid.error()));
    auto source=mesh_source.provide(*device);
    if (!source) return std::unexpected(std::move(source.error()).note("mesh"));
    auto geometry=prepare_geometry(*device,std::move(*source),*program);
    if (!geometry) return std::unexpected(std::move(geometry.error()));
    auto albedo=albedo_source.provide(*device);
    if (!albedo) return std::unexpected(std::move(albedo.error()).note("albedo"));
    if (auto valid=validate_image(*albedo,*device); !valid) return std::unexpected(std::move(valid.error()));
    if (auto allowed=into_result(device->require_resource_update("MeshRendererBuilder::commit")); !allowed)
        return std::unexpected(std::move(allowed.error()));
    return MeshRenderer{std::make_shared<MeshRenderer::Impl>(MeshRenderer::Impl{
        mesh_source,program_source,albedo_source,std::make_shared<SurfaceProgram>(std::move(*program)),
        std::move(*geometry),std::move(*albedo)})};
}

static_assert(render::RendererFor<MeshRenderer,Frame>);
} // namespace vng::opengl
