#include <vng/rig_opengl/skinned_mesh_renderer.hpp>

#include <vng/opengl/frame.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/render_opengl/program_runtime.hpp>
#include <vng/shader/shader.hpp>
#include <vng/shader/skinning.hpp>

#include "../opengl/gl_error.hpp"

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <variant>

namespace vng::opengl {
namespace {

class ProvisionScope final {
public:
    explicit ProvisionScope(bool& providing) : providing_(providing) { providing_ = true; }
    ~ProvisionScope() { providing_ = false; }
    ProvisionScope(const ProvisionScope&) = delete;
    ProvisionScope& operator=(const ProvisionScope&) = delete;
private:
    bool& providing_;
};

static_assert(sizeof(Vec4) == 16 && sizeof(Mat4) == 64);
static_assert(std::is_trivially_copyable_v<Mat4>);

using Geometry = detail::SkinnedGeometry;
using Influences4 = gfx::Record<gfx::JointIndices<0>, gfx::JointWeights<0>>;
using Influences8 = gfx::Record<gfx::JointIndices<0>, gfx::JointWeights<0>,
    gfx::JointIndices<1>, gfx::JointWeights<1>>;
static_assert(Influences4::stride == 32 && Influences8::stride == 64);

template<u32 Count>
using Influences = std::conditional_t<Count == 4, Influences4, Influences8>;
template<u32 Count>
using Inputs = std::conditional_t<Count == 4,
    shader::VertexInputs<gfx::Position, gfx::Normal, gfx::Color,
        gfx::JointIndices<0>, gfx::JointWeights<0>>,
    shader::VertexInputs<gfx::Position, gfx::Normal, gfx::Color,
        gfx::JointIndices<0>, gfx::JointWeights<0>, gfx::JointIndices<1>, gfx::JointWeights<1>>>;

// The CPU source used by capture and its GPU allocation always have the same
// layout. Four-influence rendering never allocates/uploads the second group.
template<class InfluenceRecord>
struct MeshResources final {
    gfx::Mesh<Geometry, InfluenceRecord> mesh;
    GpuMesh<Geometry, InfluenceRecord> gpu;
};
using AnyMeshResources = std::variant<MeshResources<Influences4>, MeshResources<Influences8>>;
using Varyings = shader::VertexOutputs<shader::ClipPosition,
    shader::smooth<gfx::Color>, shader::smooth<render::SkinnedWorldPosition>,
    shader::smooth<render::SkinnedWorldNormal>>;
using FragmentInputs = shader::FragmentInputs<shader::smooth<gfx::Color>,
    shader::smooth<render::SkinnedWorldPosition>, shader::smooth<render::SkinnedWorldNormal>>;
using Outputs = shader::FragmentOutputs<shader::Color<0>>;
using SkinProgram = shader::TypedGraphicsProgram<Mat4, Mat4>;
using SkinRuntime = render::TypedOpenGLProgramRuntime<Mat4, Mat4>;

constexpr GraphicsStateSnapshot raster(const render::SkinnedDraw& draw)
{
    return {.depth = {draw.depth_test, draw.depth_write, render::DepthCompare::less},
        .cull = draw.cull, .front_face = render::FrontFace::counter_clockwise,
        .blend = render::BlendMode::disabled, .polygon = PolygonMode::fill};
}

Diagnostic invalid(std::string message)
{
    return {.code = ErrorCode::invalid_argument, .message = std::move(message)};
}

std::expected<Mat4, Diagnostic> normal_matrix(const Mat4& matrix)
{
    auto result = rig::normal_matrix(matrix);
    if (!result) return std::unexpected(detail::skin_diagnostic(result.error()));
    return *result;
}

// Deterministic, non-cryptographic resource mismatch detector. Capture identity
// must include actual static attributes/influences and submitted matrices, not
// merely topology or a Pose revision shared by independent pose copies.
class ResourceFingerprint final {
public:
    void append(u64 word) noexcept
    {
        for (u32 shift = 0; shift < 64; shift += 8) {
            const auto byte = static_cast<u8>(word >> shift);
            first_ = (first_ ^ byte) * 0x100000001B3ULL;
            second_ = (second_ + byte + 0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
            second_ ^= second_ >> 31;
        }
    }
    void append(std::span<const std::byte> bytes) noexcept
    {
        append(bytes.size());
        for (const auto byte : bytes) append(std::to_integer<u8>(byte));
    }
    void append(const Mat4& matrix) noexcept
    {
        for (const auto& column : matrix.columns) {
            for (u32 i = 0; i < 4; ++i) append(std::bit_cast<u32>(column[i]));
        }
    }
    std::string finish() const
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result(32, '0');
        for (u32 i = 0; i < 16; ++i) {
            const auto shift = (15U - i) * 4U;
            result[i] = digits[(first_ >> shift) & 15];
            result[16 + i] = digits[(second_ >> shift) & 15];
        }
        return result;
    }
private:
    u64 first_{0xCBF29CE484222325ULL}, second_{0x6A09E667F3BCC909ULL};
};

template<u32 Count>
std::expected<SkinProgram, Diagnostic> create_program(bool has_normals, bool lighting)
{
    static_assert(Count == 4 || Count == 8);
    auto vertex = shader::vertex<Inputs<Count>, Varyings>("skinned_vertex", [](auto& stage,
        dsl::Float4x4 object, dsl::Float4x4 object_normal) {
        auto positions = dsl::blend_matrices<0>(stage,
            stage.input(gfx::JointIndices<0>{}), stage.input(gfx::JointWeights<0>{}), 0, 2);
        auto normals = dsl::blend_matrices<0>(stage,
            stage.input(gfx::JointIndices<0>{}), stage.input(gfx::JointWeights<0>{}), 1, 2);
        if constexpr (Count == 8) {
            positions = positions + dsl::blend_matrices<0>(stage,
                stage.input(gfx::JointIndices<1>{}), stage.input(gfx::JointWeights<1>{}), 0, 2);
            normals = normals + dsl::blend_matrices<0>(stage,
                stage.input(gfx::JointIndices<1>{}), stage.input(gfx::JointWeights<1>{}), 1, 2);
        }
        const auto position = positions * dsl::vec4(stage.input(gfx::Position{}), 1.0F);
        const auto normal = normals * dsl::vec4(stage.input(gfx::Normal{}), 0.0F);
        const auto world = object * position;
        const auto world_normal = object_normal * normal;
        return stage.output(
            dsl::field<shader::ClipPosition>(stage.camera().project(world.xyz())),
            dsl::field<gfx::Color>(stage.input(gfx::Color{})),
            dsl::field<render::SkinnedWorldPosition>(world.xyz()),
            dsl::field<render::SkinnedWorldNormal>(world_normal.xyz()));
    });
    if (!vertex) return std::unexpected(invalid(vertex.error().message));
    auto fragment = shader::fragment<FragmentInputs, Outputs>("skinned_fragment", [=](auto& stage) {
        const auto color = stage.input(gfx::Color{});
        const auto raw_normal = stage.input(render::SkinnedWorldNormal{});
        // Opposing influences can cancel a blended normal. Avoid NaNs without
        // pretending that normalization can recover a meaningful direction.
        const auto magnitude = dsl::max(
            dsl::max(dsl::max(raw_normal.x(), -raw_normal.x()),
                     dsl::max(raw_normal.y(), -raw_normal.y())),
            dsl::max(raw_normal.z(), -raw_normal.z()));
        const auto scaled_normal = raw_normal / dsl::select(magnitude > 0.0F, magnitude, 1.0F);
        const auto normal = scaled_normal / dsl::sqrt(
            dsl::max(dsl::dot(scaled_normal, scaled_normal), 1.0e-20F));
        stage.observe(render::SkinnedWorldPosition{}, stage.input(render::SkinnedWorldPosition{}));
        // A synthesized fallback keeps the shared shader layout well-defined,
        // but is not authored surface data and must not masquerade as evidence.
        if (has_normals) stage.observe(render::SkinnedWorldNormal{}, normal);
        auto output = color;
        if (lighting) {
            const auto light = dsl::normalize(stage.constant(Vec3{0.3F, 0.6F, 1.0F}));
            const auto brightness = dsl::max(dsl::dot(normal, light), 0.0F) * 0.75F + 0.25F;
            output = dsl::vec4(color.xyz() * brightness, color.w());
        }
        return stage.output(dsl::field<shader::Color<0>>(output));
    });
    if (!fragment) return std::unexpected(invalid(fragment.error().message));
    auto program = shader::link(std::move(*vertex), std::move(*fragment));
    if (!program) return std::unexpected(invalid(program.error().message));
    return std::move(*program);
}

struct PreparedResources final {
    AnyMeshResources mesh;
    SkinRuntime shaders;
};

template<u32 Count>
std::expected<PreparedResources, Diagnostic> prepare_resources(
    Device& device, detail::SkinnedMeshSource& source,
    const rig::PackedInfluences& packed, bool lighting)
{
    using InfluenceRecord = Influences<Count>;
    gfx::Mesh<Geometry, InfluenceRecord> mesh(source.mesh.vertex_count());
    mesh.info() = source.mesh.info();
    mesh.faces() = source.mesh.faces();
    mesh.template vertices<Geometry>() = std::move(source.mesh.vertices());
    for (std::size_t i = 0; i < packed.vertices.size(); ++i) {
        auto& vertex = mesh.template vertices<InfluenceRecord>()[i];
        const auto& influence = packed.vertices[i];
        vertex.set(gfx::JointIndices<0>{}, influence.joints0);
        vertex.set(gfx::JointWeights<0>{}, influence.weights0);
        if constexpr (Count == 8) {
            vertex.set(gfx::JointIndices<1>{}, influence.joints1);
            vertex.set(gfx::JointWeights<1>{}, influence.weights1);
        }
    }
    auto program = create_program<Count>(source.has_normals, lighting);
    if (!program) return std::unexpected(std::move(program.error()));
    auto shaders = SkinRuntime::create(device, std::move(*program));
    if (!shaders) return std::unexpected(std::move(shaders.error()));
    auto gpu = upload_mesh(device, mesh);
    if (!gpu) return std::unexpected(std::move(gpu.error()));
    if (auto prepared = gpu->prepare_vertex_input(device, shaders->production()); !prepared)
        return std::unexpected(std::move(prepared.error()));
    return PreparedResources{
        MeshResources<InfluenceRecord>{std::move(mesh), std::move(*gpu)},
        std::move(*shaders)};
}

// Borrow only the palette's indexed SSBO slot. Preserve both a host's range
// binding and its separate generic binding, which glBindBufferBase changes.
class StorageSlotScope final {
public:
    static std::expected<StorageSlotScope, Diagnostic> capture()
    {
        StorageSlotScope scope;
        auto captured = detail::checked_gl_call("capture skin matrix buffer binding", [&] {
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &scope.indexed_);
            glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, 0, &scope.offset_);
            glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, 0, &scope.size_);
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &scope.generic_);
        });
        if (!captured) return std::unexpected(std::move(captured.error()));
        scope.active_ = true;
        return scope;
    }
    StorageSlotScope(StorageSlotScope&& other) noexcept
        : indexed_(other.indexed_), generic_(other.generic_), offset_(other.offset_),
          size_(other.size_), active_(std::exchange(other.active_, false)) {}
    ~StorageSlotScope() { if (active_) restore_native(); }
    std::expected<void, Diagnostic> bind(const Buffer& buffer)
    {
        return detail::checked_gl_call("bind skin matrix buffer", [&] {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer.native_handle());
        });
    }
    std::expected<void, Diagnostic> restore()
    {
        if (!active_) return {};
        active_ = false;
        return detail::checked_gl_call("restore skin matrix buffer binding", [&] { restore_native(); });
    }
private:
    StorageSlotScope() = default;
    void restore_native()
    {
        if (indexed_ != 0 && size_ > 0) {
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLuint>(indexed_),
                static_cast<GLintptr>(offset_), static_cast<GLsizeiptr>(size_));
        } else glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLuint>(indexed_));
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(generic_));
    }
    GLint indexed_{}, generic_{};
    GLint64 offset_{}, size_{};
    bool active_{};
};

} // namespace

struct SkinnedMeshRenderer::Impl {
    AnyMeshResources resources;
    rig::SkinWeights weights;
    SkinRuntime shaders;
    Buffer matrix_buffer;
    std::vector<Mat4> last_matrices;
    ResourceFingerprint static_fingerprint;
    render::SkinnedRenderStats stats;
    render::SkinnedRendererOptions options;
    bool has_normals{};
    SkinProvider skin_provider;

    Impl(AnyMeshResources source, rig::SkinWeights skin,
         SkinRuntime shader, Buffer matrices,
         render::SkinnedRendererOptions settings, bool normals)
        : resources(std::move(source)), weights(std::move(skin)), shaders(std::move(shader)),
          matrix_buffer(std::move(matrices)), options(settings), has_normals(normals)
    {
        static_fingerprint.append(0x534B494E52455331ULL); // "SKINRES1"
        std::visit([&](const auto& resource) {
            resource.mesh.for_each_vertex_stream([&](const auto& stream) {
                static_fingerprint.append(stream.bytes());
            });
        }, resources);
    }

    detail::SkinnedMeshSource current_source() const
    {
        return std::visit([&](const auto& resource) {
            gfx::Mesh<Geometry> mesh{resource.mesh.template vertices<Geometry>()};
            mesh.info() = resource.mesh.info();
            mesh.faces() = resource.mesh.faces();
            return detail::SkinnedMeshSource{std::move(mesh), weights, has_normals};
        }, resources);
    }

    std::expected<void, Diagnostic> validate_frame(Frame& frame, const render::RenderView& view) const
    {
        if (!frame.active() || frame.extent().empty() || view.extent() != frame.extent() || !view.camera())
            return std::unexpected(invalid("Skinned rendering requires an active matching frame and camera view"));
        if (!matrix_buffer.belongs_to(frame.device()))
            return std::unexpected(Diagnostic{.code = ErrorCode::incompatible_device,
                .message = "Skinned renderer belongs to another device/context"});
        return frame.device().require_current("SkinnedMeshRenderer");
    }

    std::expected<std::array<Mat4, 2>, Diagnostic> upload_palette(const render::SkinnedDraw& draw)
    {
        if (auto valid = rig::validate(draw.transform); !valid)
            return std::unexpected(detail::skin_diagnostic(valid.error()));
        auto palette = weights.palette(draw.pose.get());
        if (!palette) return std::unexpected(detail::skin_diagnostic(palette.error()));
        std::vector<Mat4> matrices;
        matrices.reserve(2 * palette->size());
        auto object = rig::matrix(draw.transform);
        auto object_normal = normal_matrix(object);
        if (!object_normal) return std::unexpected(std::move(object_normal.error()));
        const std::array arguments{object, *object_normal};
        for (const auto& bone : *palette) {
            auto normal = normal_matrix(bone);
            if (!normal) return std::unexpected(std::move(normal.error()));
            matrices.push_back(bone);
            matrices.push_back(*normal);
        }
        // Content comparison is deliberate: two independently copied poses
        // can have equal revisions but different values. No aliasing cache key.
        if (matrices == last_matrices) return arguments;
        if (auto written = matrix_buffer.write(0, std::as_bytes(std::span{matrices})); !written)
            return std::unexpected(std::move(written.error()));
        last_matrices = std::move(matrices);
        ++stats.palette_uploads;
        return arguments;
    }
};

SkinnedMeshRenderer::SkinnedMeshRenderer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SkinnedMeshRenderer::SkinnedMeshRenderer(SkinnedMeshRenderer&&) noexcept = default;
SkinnedMeshRenderer& SkinnedMeshRenderer::operator=(SkinnedMeshRenderer&&) noexcept = default;
SkinnedMeshRenderer::~SkinnedMeshRenderer() = default;

std::expected<SkinnedMeshRenderer, Diagnostic> SkinnedMeshRenderer::create(
    Device& device, detail::SkinnedMeshSource source, render::SkinnedRendererOptions options)
{
    auto packed = source.weights.pack(options.max_influences);
    if (!packed) return std::unexpected(detail::skin_diagnostic(packed.error()));
    if (source.mesh.vertex_count() != packed->vertices.size())
        return std::unexpected(invalid("Skin weights do not match geometry vertex count"));
    if (auto valid = source.mesh.validate(); !valid) return std::unexpected(invalid(valid.error().message));
    if (auto current = device.require_current("SkinnedMeshRenderer::create"); !current)
        return std::unexpected(std::move(current.error()));
    const auto bone_count = source.weights.armature().bone_count();
    const u64 bytes = 2 * static_cast<u64>(bone_count) * sizeof(Mat4);
    GLint bindings{}, vertex_blocks{};
    GLint64 max_bytes{};
    if (auto limits = detail::checked_gl_call("query skin matrix buffer limits", [&] {
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &bindings);
        glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vertex_blocks);
        glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_bytes);
    }); !limits) return std::unexpected(std::move(limits.error()));
    if (bindings < 1 || vertex_blocks < 1 || max_bytes <= 0 || bytes > static_cast<u64>(max_bytes))
        return std::unexpected(invalid("Armature exceeds vertex shader matrix-buffer capabilities"));

    const auto lighting = options.lighting && source.has_normals;
    auto prepared = options.max_influences == 4
        ? prepare_resources<4>(device, source, *packed, lighting)
        : prepare_resources<8>(device, source, *packed, lighting);
    if (!prepared) return std::unexpected(std::move(prepared.error()));
    auto buffer = Buffer::create(device, {.size = static_cast<std::size_t>(bytes),
        .initial_data = {}, .storage = BufferStorage::dynamic});
    if (!buffer) return std::unexpected(std::move(buffer.error()));
    return SkinnedMeshRenderer{std::make_unique<Impl>(std::move(prepared->mesh),
        std::move(source.weights), std::move(prepared->shaders), std::move(*buffer),
        options, source.has_normals)};
}

resources::Result<void> SkinnedMeshRenderer::validate_update(const Device& device) const
{
    if (providing_) {
        return std::unexpected(resources::Diagnostic{
            .code = resources::ErrorCode::operation_failed,
            .message = "A skin provider cannot update its owner reentrantly"});
    }
    if (!impl_ || !impl_->matrix_buffer.belongs_to(device)) {
        return std::unexpected(resources::to_diagnostic(
            invalid("Skin resource update requires a live renderer and its owning device")));
    }
    return resources::into_result(device.require_resource_update("SkinnedMeshRenderer resource update"));
}

resources::Result<void> SkinnedMeshRenderer::install_skin(
    Device& device, detail::SkinnedMeshSource source, SkinProvider provider)
{
    if (auto valid = validate_update(device); !valid) return valid;
    auto candidate = create(device, std::move(source), impl_->options);
    if (!candidate) return std::unexpected(resources::to_diagnostic(std::move(candidate.error())));
    candidate->impl_->skin_provider = std::move(provider);
    if (auto valid = validate_update(device); !valid) return valid;
    impl_.swap(candidate->impl_);
    return {};
}

resources::Result<void> SkinnedMeshRenderer::reload_skin(Device& device)
{
    if (auto valid = validate_update(device); !valid) return valid;
    return reload_skin(device, impl_->skin_provider);
}

resources::Result<void> SkinnedMeshRenderer::reload_skin(Device& device, SkinProvider provider)
{
    if (auto valid = validate_update(device); !valid) return valid;
    auto source = [&] {
        const ProvisionScope scope{providing_};
        return provider.provide(device);
    }();
    if (!source) return std::unexpected(std::move(source.error()));
    return install_skin(device, std::move(*source), std::move(provider));
}

resources::Result<resources::ReloadReport> SkinnedMeshRenderer::reload(Device& device)
{
    if (auto valid = validate_update(device); !valid)
        return std::unexpected(std::move(valid.error()));
    auto provider = impl_->skin_provider;
    resources::ReloadReport report;
    auto source = [&]() -> resources::Result<detail::SkinnedMeshSource> {
        if (!provider) return impl_->current_source();
        const ProvisionScope scope{providing_};
        return provider.provide(device);
    }();
    if (!source) return std::unexpected(std::move(source.error()));
    if (provider) report.refreshed.push_back("skin");
    else report.retained.push_back("skin");
    report.refreshed.push_back("program");
    report.refreshed.push_back("skin buffers");
    if (auto installed = install_skin(device, std::move(*source), std::move(provider)); !installed)
        return std::unexpected(std::move(installed.error()));
    return report;
}

resources::Result<SkinnedMeshRenderer> SkinnedMeshRendererBuilder::build() const
{
    auto& device = *device_;
    const auto provider = provider_;
    const auto options = options_;
    if (auto ready = device.require_resource_update("SkinnedMeshRendererBuilder::build"); !ready)
        return std::unexpected(resources::to_diagnostic(std::move(ready.error())));
    auto source = provider.provide(device);
    if (!source) return std::unexpected(std::move(source.error()));
    if (auto ready = device.require_resource_update("SkinnedMeshRendererBuilder::build commit"); !ready)
        return std::unexpected(resources::to_diagnostic(std::move(ready.error())));
    auto renderer = SkinnedMeshRenderer::create(device, std::move(*source), options);
    if (!renderer) return std::unexpected(resources::to_diagnostic(std::move(renderer.error())));
    renderer->impl_->skin_provider = provider;
    return std::move(*renderer);
}

std::expected<void, Diagnostic> SkinnedMeshRenderer::render(
    Frame& frame, const render::RenderView& view, const render::SkinnedDraw& draw)
{
    return render(frame, view, std::span{&draw, 1});
}

std::expected<void, Diagnostic> SkinnedMeshRenderer::render(
    Frame& frame, const render::RenderView& view, std::span<const render::SkinnedDraw> draws)
{
    if (!impl_) return std::unexpected(invalid("Skinned renderer is empty"));
    if (auto valid = impl_->validate_frame(frame, view); !valid) return valid;
    impl_->stats = {};
    if (draws.empty()) return {};
    auto slot = StorageSlotScope::capture();
    if (!slot) return std::unexpected(std::move(slot.error()));
    if (auto bound = slot->bind(impl_->matrix_buffer); !bound) return bound;
    auto context = frame.render_context();
    auto graphics = context.graphics_state();
    // One layout dispatch per submission, not per vertex or per ticket.
    auto rendered = std::visit([&](auto& resource) -> std::expected<void, Diagnostic> {
        for (const auto& draw : draws) {
            auto arguments = impl_->upload_palette(draw);
            if (!arguments) return std::unexpected(std::move(arguments.error()));
            if (auto selected = context.run(impl_->shaders.production(), (*arguments)[0], (*arguments)[1]); !selected)
                return selected;
            if (auto selected = context.view(view); !selected) return selected;
            if (auto changed = graphics.set(raster(draw)); !changed) return changed;
            if (auto drawn = context.draw(resource.gpu); !drawn) return drawn;
            ++impl_->stats.draw_calls;
            impl_->stats.triangles += resource.mesh.face_count();
        }
        return {};
    }, impl_->resources);
    if (!rendered) return rendered;
    return slot->restore();
}

std::expected<analysis::FrameEvidence, Diagnostic> SkinnedMeshRenderer::capture(
    Frame& frame, const render::RenderView& view, const render::SkinnedDraw& draw,
    const analysis::CaptureRequest& request)
{
    if (!impl_) return std::unexpected(invalid("Skinned renderer is empty"));
    if (auto valid = impl_->validate_frame(frame, view); !valid)
        return std::unexpected(std::move(valid.error()));
    impl_->stats = {};
    auto arguments = impl_->upload_palette(draw);
    if (!arguments) return std::unexpected(std::move(arguments.error()));
    if (auto supplied = impl_->shaders.set_arguments((*arguments)[0], (*arguments)[1]); !supplied)
        return std::unexpected(std::move(supplied.error()));
    auto slot = StorageSlotScope::capture();
    if (!slot) return std::unexpected(std::move(slot.error()));
    if (auto bound = slot->bind(impl_->matrix_buffer); !bound)
        return std::unexpected(std::move(bound.error()));
    render::AnalysisOptions options;
    options.provenance.object_to_world = rig::matrix(draw.transform);
    auto fingerprint = impl_->static_fingerprint;
    for (const auto& matrix : *arguments) fingerprint.append(matrix);
    fingerprint.append(impl_->last_matrices.size());
    for (const auto& matrix : impl_->last_matrices) fingerprint.append(matrix);
    options.resource_fingerprint = fingerprint.finish();
    auto evidence = std::visit([&](auto& resource) {
        return impl_->shaders.capture(frame.device(), resource.mesh, resource.gpu,
            view, CaptureState{raster(draw), frame.color_encoding()}, request, options);
    }, impl_->resources);
    if (!evidence) return evidence;
    if (auto restored = slot->restore(); !restored)
        return std::unexpected(std::move(restored.error()));
    impl_->stats.draw_calls = 1 + static_cast<u32>(request.observations().size());
    impl_->stats.triangles = std::visit([&](const auto& resource) {
        return resource.mesh.face_count() * impl_->stats.draw_calls;
    }, impl_->resources);
    return evidence;
}

render::SkinnedRenderStats SkinnedMeshRenderer::stats() const noexcept
{
    return impl_ ? impl_->stats : render::SkinnedRenderStats{};
}

const glsl::ProgramSource& SkinnedMeshRenderer::source() const
{
    if (!impl_) throw std::logic_error("Skinned renderer is empty");
    return impl_->shaders.normal_source();
}

static_assert(render::RendererFor<SkinnedMeshRenderer, Frame>);

} // namespace vng::opengl
