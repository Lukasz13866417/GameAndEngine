#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

#include <vng/analysis/analysis.hpp>
#include <vng/gfx/camera.hpp>
#include <vng/glsl/emitter.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/render/program.hpp>
#include <vng/render/view.hpp>
#include <vng/shader/program.hpp>

namespace vng::opengl {

// A per-invocation snapshot, never shader-owned policy. Low-level callers must
// specify both values; frame overloads snapshot the live frame automatically.
struct CaptureState final {
    GraphicsStateSnapshot raster;
    render::ColorEncoding target_encoding;
    CaptureState(GraphicsStateSnapshot raster, render::ColorEncoding encoding)
        : raster(raster), target_encoding(encoding) {}
};

} // namespace vng::opengl

namespace vng::render {

template<shader::Argument... Args> class TypedOpenGLProgramRuntime;

struct AnalysisOptions final {
    analysis::RenderItemProvenance provenance{};
    std::string label;
    std::array<float, 4> clear_color{0.0F, 0.0F, 0.0F, 0.0F};

    // Empty selects the convention implied by the effective depth comparison:
    // 1 for ordinary depth, 0 for reverse depth.
    std::optional<float> clear_depth{};

    // Optional renderer-owned identity for shader resources not represented by
    // mesh topology or placement, such as a skinning palette. Concrete
    // renderers include both immutable asset contents and per-invocation
    // resource values here so diagnostic counterfactuals cannot accidentally
    // compare different workloads. Ordinary callers need not provide it.
    std::string resource_fingerprint{};
};

// Backend shader runtime shared by concrete OpenGL renderers. It owns the
// ordinary compiled program and lazily cached enhanced variants produced from
// the same canonical shader IR. It is deliberately not an opengl::Renderer<Ticket>:
// ticket interpretation, draw ordering, and state decisions belong to the
// concrete renderer that owns this value.
class OpenGLProgramRuntime {
public:
    [[nodiscard]] static std::expected<OpenGLProgramRuntime, opengl::Diagnostic> create(
        const opengl::Device& device,
        shader::GraphicsProgram program);
    template<shader::Argument... Args>
    [[nodiscard]] static std::expected<TypedOpenGLProgramRuntime<Args...>, opengl::Diagnostic> create(
        const opengl::Device& device,
        shader::TypedGraphicsProgram<Args...> program);

    OpenGLProgramRuntime(OpenGLProgramRuntime&&) noexcept = default;
    OpenGLProgramRuntime& operator=(OpenGLProgramRuntime&&) noexcept = default;
    OpenGLProgramRuntime(const OpenGLProgramRuntime&) = delete;
    OpenGLProgramRuntime& operator=(const OpenGLProgramRuntime&) = delete;
    ~OpenGLProgramRuntime() = default;

    [[nodiscard]] const glsl::ProgramSource& normal_source() const noexcept
    {
        // create() guarantees that the backend program came from neutral
        // shader emission and therefore owns this artifact.
        return *normal_program_.generated_source();
    }

    // Program selection leaves draw state entirely under renderer control.
    [[nodiscard]] const opengl::Program& production() const noexcept
    {
        return normal_program_;
    }

    // Null until the analysis variant has first been emitted and compiled.
    // Keeping this lazy means an unsupported optional analysis mode never
    // prevents ordinary drawing.
    [[nodiscard]] const glsl::ProgramSource* analysis_source() const noexcept
    {
        return analysis_program_
            ? analysis_program_->generated_source()
            : nullptr;
    }

    // Renders color, authoritative device depth, and surface identity in one
    // draw. Source topology and provenance are attached automatically to the
    // resulting CPU capture; no analysis attributes or shader code are needed
    // at the call site.
    template<gfx::RecordType... Records>
    [[nodiscard]] std::expected<analysis::AnalysisCapture, opengl::Diagnostic>
    render_analysis(
        const opengl::Device& device,
        const gfx::Mesh<Records...>& source_mesh,
        opengl::GpuMesh<Records...>& gpu_mesh,
        analysis::Extent2D extent,
        const opengl::CaptureState& state,
        AnalysisOptions options = {})
    {
        return render_analysis_impl(
            device,
            source_mesh,
            gpu_mesh,
            extent,
            nullptr,
            std::move(options),
            state,
            RasterOverride::production);
    }

    template<gfx::RecordType... Records>
    [[nodiscard]] std::expected<analysis::AnalysisCapture, opengl::Diagnostic>
    render_analysis(
        const opengl::Device& device,
        const gfx::Mesh<Records...>& source_mesh,
        opengl::GpuMesh<Records...>& gpu_mesh,
        const gfx::Camera& camera,
        analysis::Extent2D extent,
        const opengl::CaptureState& state,
        AnalysisOptions options = {})
    {
        auto snapshot = snapshot_camera(camera, extent);
        if (!snapshot) {
            return std::unexpected(std::move(snapshot.error()));
        }
        return render_analysis_impl(
            device,
            source_mesh,
            gpu_mesh,
            extent,
            &*snapshot,
            std::move(options),
            state,
            RasterOverride::production);
    }

    // High-level diagnostic boundary. The caller describes evidence, not
    // framebuffer attachments or shader variants. Named observations are
    // emitted lazily from the same neutral shader IR and returned as typed
    // EvidenceChannel values.
    template<gfx::RecordType... Records>
    [[nodiscard]] std::expected<analysis::FrameEvidence, opengl::Diagnostic>
    capture(
        const opengl::Device& device,
        const gfx::Mesh<Records...>& source_mesh,
        opengl::GpuMesh<Records...>& gpu_mesh,
        const RenderView& view,
        const opengl::CaptureState& state,
        analysis::CaptureRequest request = analysis::CaptureRequest::standard(),
        AnalysisOptions options = {})
    {
        return capture_impl(
            device,
            source_mesh,
            gpu_mesh,
            view,
            state,
            std::move(request),
            std::move(options),
            RasterOverride::production);
    }

    // Runs two one-variable counterfactuals in addition to the production
    // capture, then derives causal hints from their coverage. This is intended
    // for tools/tests, not for the hot render loop.
    template<gfx::RecordType... Records>
    [[nodiscard]] std::expected<analysis::DiagnosticSweep, opengl::Diagnostic>
    diagnose(
        const opengl::Device& device,
        const gfx::Mesh<Records...>& source_mesh,
        opengl::GpuMesh<Records...>& gpu_mesh,
        const RenderView& view,
        const opengl::CaptureState& state,
        analysis::CaptureRequest request = analysis::CaptureRequest::diagnostic(),
        AnalysisOptions options = {})
    {
        auto production = capture_impl(
            device, source_mesh, gpu_mesh, view, state, std::move(request), options,
            RasterOverride::production);
        if (!production) {
            return std::unexpected(std::move(production.error()));
        }
        auto no_cull = capture_impl(
            device, source_mesh, gpu_mesh, view, state,
            analysis::CaptureRequest::standard(), options,
            RasterOverride::cull_disabled);
        if (!no_cull) {
            return std::unexpected(std::move(no_cull.error()));
        }
        auto depth_always = capture_impl(
            device, source_mesh, gpu_mesh, view, state,
            analysis::CaptureRequest::standard(), std::move(options),
            RasterOverride::depth_always);
        if (!depth_always) {
            return std::unexpected(std::move(depth_always.error()));
        }

        std::vector<analysis::VariantEvidence> variants;
        variants.push_back({
            analysis::DiagnosticVariant::production,
            std::move(*production),
        });
        variants.push_back({
            analysis::DiagnosticVariant::cull_disabled,
            std::move(*no_cull),
        });
        variants.push_back({
            analysis::DiagnosticVariant::depth_always,
            std::move(*depth_always),
        });
        auto sweep = analysis::DiagnosticSweep::create(std::move(variants));
        if (!sweep) {
            return std::unexpected(sweep_diagnostic(sweep.error()));
        }
        return std::move(*sweep);
    }

    // Frame entry points inherit this invocation's live state, not creation-time defaults.
    template<gfx::RecordType... Records>
    auto capture(opengl::Frame& frame, const gfx::Mesh<Records...>& cpu,
                 opengl::GpuMesh<Records...>& gpu, const RenderView& view,
                 analysis::CaptureRequest request = analysis::CaptureRequest::standard(),
                 AnalysisOptions options = {})
        -> std::expected<analysis::FrameEvidence, opengl::Diagnostic>
    {
        if (frame.extent() != view.extent()) return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::invalid_argument, .message = "Capture requires matching frame and view extents"});
        auto state = snapshot_state(frame);
        if (!state) return std::unexpected(state.error());
        return capture(frame.device(), cpu, gpu, view, *state, std::move(request), std::move(options));
    }
    template<gfx::RecordType... Records>
    auto diagnose(opengl::Frame& frame, const gfx::Mesh<Records...>& cpu,
                  opengl::GpuMesh<Records...>& gpu, const RenderView& view,
                  analysis::CaptureRequest request = analysis::CaptureRequest::diagnostic(),
                  AnalysisOptions options = {})
        -> std::expected<analysis::DiagnosticSweep, opengl::Diagnostic>
    {
        if (frame.extent() != view.extent()) return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::invalid_argument, .message = "Capture requires matching frame and view extents"});
        auto state = snapshot_state(frame);
        if (!state) return std::unexpected(state.error());
        return diagnose(frame.device(), cpu, gpu, view, *state, std::move(request), std::move(options));
    }
    template<gfx::RecordType... Records>
    auto render_analysis(opengl::Frame& frame, const gfx::Mesh<Records...>& cpu,
                         opengl::GpuMesh<Records...>& gpu, AnalysisOptions options = {})
        -> std::expected<analysis::AnalysisCapture, opengl::Diagnostic>
    {
        auto state = snapshot_state(frame);
        if (!state) return std::unexpected(state.error());
        return render_analysis(frame.device(), cpu, gpu, frame.extent(), *state, std::move(options));
    }
    template<gfx::RecordType... Records>
    auto render_analysis(opengl::Frame& frame, const gfx::Mesh<Records...>& cpu,
                         opengl::GpuMesh<Records...>& gpu, const gfx::Camera& camera, AnalysisOptions options = {})
        -> std::expected<analysis::AnalysisCapture, opengl::Diagnostic>
    {
        auto state = snapshot_state(frame);
        if (!state) return std::unexpected(state.error());
        return render_analysis(frame.device(), cpu, gpu, camera, frame.extent(), *state, std::move(options));
    }

private:
    static std::expected<opengl::CaptureState, opengl::Diagnostic> snapshot_state(opengl::Frame&);
    static std::expected<void, opengl::Diagnostic> validate_state(const opengl::CaptureState&);
    static std::optional<std::vector<u32>> fragment_outputs(const opengl::Program&);
    enum class RasterOverride {
        production,
        cull_disabled,
        depth_always,
    };

    static opengl::GraphicsStateSnapshot effective_raster(const opengl::CaptureState&, RasterOverride);
    static std::expected<void, opengl::Diagnostic> apply_raster(
        const opengl::Device&, const opengl::Program&, const opengl::CaptureState&, RasterOverride);

    template<gfx::RecordType... Records>
    [[nodiscard]] std::expected<analysis::FrameEvidence, opengl::Diagnostic>
    capture_impl(
        const opengl::Device& device,
        const gfx::Mesh<Records...>& source_mesh,
        opengl::GpuMesh<Records...>& gpu_mesh,
        const RenderView& view,
        const opengl::CaptureState& state,
        analysis::CaptureRequest request,
        AnalysisOptions options,
        RasterOverride raster_override)
    {
        auto diagnostic_cursor = device.diagnostic_cursor();
        if (!diagnostic_cursor) {
            return std::unexpected(std::move(diagnostic_cursor.error()));
        }
        const auto invocation = make_invocation_identity(
            source_mesh.topology_fingerprint(), view, options, state);
        const auto* camera = view.camera() ? &*view.camera() : nullptr;
        auto canonical = render_analysis_impl(
            device,
            source_mesh,
            gpu_mesh,
            view.extent(),
            camera,
            options,
            state,
            raster_override);
        if (!canonical) {
            return std::unexpected(std::move(canonical.error()));
        }

        std::vector<analysis::EvidenceChannel> observations;
        if (raster_override == RasterOverride::production) {
            observations.reserve(request.observations().size());
            const auto clear_depth = options.clear_depth.value_or(
                default_analysis_clear_depth(state.raster));
            for (const auto& requested : request.observations()) {
                auto channel = render_observation_impl(
                    device,
                    gpu_mesh,
                    view.extent(),
                    camera,
                    requested,
                    clear_depth, state);
                if (!channel) {
                    return std::unexpected(std::move(channel.error()));
                }
                observations.push_back(std::move(*channel));
            }
        }

        return make_frame_evidence(
            device,
            std::move(*canonical),
            std::move(request),
            camera,
            options.label,
            invocation,
            std::move(observations),
            *diagnostic_cursor,
            raster_override, state);
    }

    template<gfx::RecordType... Records>
    [[nodiscard]] std::expected<analysis::EvidenceChannel, opengl::Diagnostic>
    render_observation_impl(
        const opengl::Device& device,
        opengl::GpuMesh<Records...>& gpu_mesh,
        analysis::Extent2D extent,
        const gfx::CameraSnapshot* camera,
        const analysis::ObservationRequest& requested,
        float clear_depth,
        const opengl::CaptureState& state)
    {
        auto product = ensure_observation_product(device, requested);
        if (!product) {
            return std::unexpected(std::move(product.error()));
        }
        if (auto target = ensure_observation_target(
                device, **product, extent);
            !target) {
            return std::unexpected(std::move(target.error()));
        }
        if (auto parameters = bind_camera_parameters(
                (*product)->program,
                (*product)->source(),
                camera);
            !parameters) {
            return std::unexpected(std::move(parameters.error()));
        }
        if (auto arguments = production().copy_arguments_to((*product)->program);
            !arguments) return std::unexpected(std::move(arguments.error()));

        const auto affected_draw_buffers =
            fragment_outputs((*product)->program);
        if (!affected_draw_buffers) {
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::operation_failed,
                .message = "shader observation program lost its emitted fragment-output metadata",
            });
        }
        auto scope = opengl::RenderStateScope::capture(
            device, *affected_draw_buffers);
        if (!scope) {
            return std::unexpected(std::move(scope.error()));
        }

        auto result = [&]()
            -> std::expected<analysis::EvidenceChannel, opengl::Diagnostic> {
            if (auto begun = begin_observation(
                    device, **product, extent, clear_depth, state);
                !begun) {
                return std::unexpected(std::move(begun.error()));
            }
            if (auto drawn = gpu_mesh.draw_bound(
                    device, (*product)->program);
                !drawn) {
                return std::unexpected(std::move(drawn.error()));
            }
            return finish_observation(**product);
        }();

        auto restored = scope->restore();
        if (!restored) {
            if (!result) {
                auto diagnostic = std::move(result.error());
                diagnostic.message +=
                    "; restoring caller OpenGL state also failed: "
                    + restored.error().message;
                return std::unexpected(std::move(diagnostic));
            }
            return std::unexpected(std::move(restored.error()));
        }
        return result;
    }

    template<gfx::RecordType... Records>
    [[nodiscard]] std::expected<analysis::AnalysisCapture, opengl::Diagnostic>
    render_analysis_impl(
        const opengl::Device& device,
        const gfx::Mesh<Records...>& source_mesh,
        opengl::GpuMesh<Records...>& gpu_mesh,
        analysis::Extent2D extent,
        const gfx::CameraSnapshot* camera,
        AnalysisOptions options,
        const opengl::CaptureState& state,
        RasterOverride raster_override)
    {
        if (auto valid = validate_state(state); !valid) return std::unexpected(valid.error());
        if (extent.empty()) {
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::invalid_argument,
                .message = "OpenGLProgramRuntime::render_analysis requires a non-empty extent",
            });
        }
        const auto clear_depth = options.clear_depth.value_or(
            default_analysis_clear_depth(state.raster));
        if (!std::isfinite(clear_depth)
            || clear_depth < 0.0F
            || clear_depth > 1.0F) {
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::invalid_argument,
                .message = "OpenGLProgramRuntime::render_analysis requires clear_depth in [0, 1]",
            });
        }
        for (const auto component : options.clear_color) {
            if (!std::isfinite(component)) {
                return std::unexpected(opengl::Diagnostic{
                    .code = opengl::ErrorCode::invalid_argument,
                    .message = "OpenGLProgramRuntime::render_analysis requires a finite clear color",
                });
            }
        }
        if (source_mesh.topology_fingerprint()
            != gpu_mesh.topology_fingerprint()) {
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::invalid_argument,
                .message = "OpenGLProgramRuntime::render_analysis requires the CPU topology used to upload the GPU mesh",
            });
        }

        if (auto program = ensure_analysis_program(device); !program) {
            return std::unexpected(std::move(program.error()));
        }
        if (auto parameters = bind_camera_parameters(
                *analysis_program_, *analysis_source(), camera);
            !parameters) {
            return std::unexpected(std::move(parameters.error()));
        }
        if (auto arguments = production().copy_arguments_to(*analysis_program_);
            !arguments) return std::unexpected(std::move(arguments.error()));

        auto frame = allocate_frame_id();
        if (!frame) {
            return std::unexpected(std::move(frame.error()));
        }

        analysis::AnalysisManifest manifest{*frame};
        const auto first_item = manifest.add(
            std::move(options.provenance),
            analysis::primitive_sources(source_mesh));

        const auto affected_draw_buffers =
            fragment_outputs(*analysis_program_);
        if (!affected_draw_buffers) {
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::operation_failed,
                .message = "analysis program lost its emitted fragment-output metadata",
            });
        }
        auto scope = opengl::RenderStateScope::capture(
            device, *affected_draw_buffers);
        if (!scope) {
            return std::unexpected(std::move(scope.error()));
        }

        auto result = [&]()
            -> std::expected<analysis::AnalysisCapture, opengl::Diagnostic> {
            if (auto begun = begin_analysis(
                    device,
                    extent,
                    first_item,
                    options.clear_color,
                    clear_depth,
                    state,
                    raster_override);
                !begun) {
                return std::unexpected(std::move(begun.error()));
            }

            if (auto drawn = gpu_mesh.draw_bound(
                    device, *analysis_program_);
                !drawn) {
                return std::unexpected(std::move(drawn.error()));
            }
            return finish_analysis(std::move(manifest));
        }();

        auto restored = scope->restore();
        if (!restored) {
            if (!result) {
                auto diagnostic = std::move(result.error());
                diagnostic.message += "; restoring caller OpenGL state also failed: "
                    + restored.error().message;
                return std::unexpected(std::move(diagnostic));
            }
            return std::unexpected(std::move(restored.error()));
        }
        return result;
    }

    struct AnalysisTarget final {
        analysis::Extent2D extent{};
        u32 surface_key_attachment{};
        opengl::Image2D color;
        opengl::Image2D surface_keys;
        opengl::Image2D depth;
        opengl::Framebuffer framebuffer;
    };

    struct ObservationTarget final {
        analysis::Extent2D extent{};
        u32 attachment{};
        opengl::ImageFormat format{opengl::ImageFormat::rgba32f};
        opengl::Image2D value;
        opengl::Image2D depth;
        opengl::Framebuffer framebuffer;
    };

    struct ObservationProduct final {
        std::type_index semantic_type{typeid(void)};
        opengl::Program program;
        std::optional<ObservationTarget> target;

        [[nodiscard]] const glsl::ProgramSource& source() const noexcept
        {
            return *program.generated_source();
        }
    };

    OpenGLProgramRuntime(
        shader::GraphicsProgram program,
        opengl::Program normal_program) noexcept
        : program_(std::move(program)),
          normal_program_(std::move(normal_program))
    {}

    [[nodiscard]] static std::expected<gfx::CameraSnapshot, opengl::Diagnostic>
    snapshot_camera(const gfx::Camera& camera, Extent2D extent);

    [[nodiscard]] static float default_analysis_clear_depth(
        const opengl::GraphicsStateSnapshot& raster) noexcept;

    [[nodiscard]] static opengl::Diagnostic sweep_diagnostic(
        const analysis::SweepDiagnostic& diagnostic);

    [[nodiscard]] static std::expected<void, opengl::Diagnostic>
    bind_camera_parameters(
        const opengl::Program& program,
        const glsl::ProgramSource& source,
        const gfx::CameraSnapshot* camera);

    [[nodiscard]] std::expected<analysis::FrameId, opengl::Diagnostic>
    allocate_frame_id();

    [[nodiscard]] std::expected<void, opengl::Diagnostic> begin_analysis(
        const opengl::Device& device,
        analysis::Extent2D extent,
        analysis::FrameItemId first_item,
        const std::array<float, 4>& clear_color,
        float clear_depth,
        const opengl::CaptureState& state,
        RasterOverride raster_override);

    [[nodiscard]] std::expected<analysis::AnalysisCapture, opengl::Diagnostic>
    finish_analysis(analysis::AnalysisManifest manifest);

    [[nodiscard]] std::expected<void, opengl::Diagnostic> ensure_analysis_target(
        const opengl::Device& device,
        analysis::Extent2D extent);

    [[nodiscard]] std::expected<void, opengl::Diagnostic> ensure_analysis_program(
        const opengl::Device& device);

    [[nodiscard]] std::expected<ObservationProduct*, opengl::Diagnostic>
    ensure_observation_product(
        const opengl::Device& device,
        const analysis::ObservationRequest& requested);

    [[nodiscard]] std::expected<void, opengl::Diagnostic>
    ensure_observation_target(
        const opengl::Device& device,
        ObservationProduct& product,
        analysis::Extent2D extent);

    [[nodiscard]] std::expected<void, opengl::Diagnostic> begin_observation(
        const opengl::Device& device,
        ObservationProduct& product,
        analysis::Extent2D extent,
        float clear_depth,
        const opengl::CaptureState& state);

    [[nodiscard]] std::expected<analysis::EvidenceChannel, opengl::Diagnostic>
    finish_observation(ObservationProduct& product);

    [[nodiscard]] std::expected<analysis::FrameEvidence, opengl::Diagnostic>
    make_frame_evidence(
        const opengl::Device& device,
        analysis::AnalysisCapture capture,
        analysis::CaptureRequest request,
        const gfx::CameraSnapshot* camera,
        std::string label,
        analysis::RenderInvocationIdentity invocation,
        std::vector<analysis::EvidenceChannel> observations,
        const opengl::DiagnosticCursor& diagnostic_cursor,
        RasterOverride raster_override,
        const opengl::CaptureState& state) const;

    [[nodiscard]] analysis::RenderInvocationIdentity make_invocation_identity(
        gfx::MeshTopologyFingerprint topology,
        const RenderView& view,
        const AnalysisOptions& options,
        const opengl::CaptureState& state) const;

    shader::GraphicsProgram program_;
    opengl::Program normal_program_;
    std::optional<opengl::Program> analysis_program_;
    std::optional<AnalysisTarget> analysis_target_;
    std::vector<ObservationProduct> observation_products_;
    u64 next_frame_id_{1};
};

// The facade preserves the lambda signature while the implementation owns one
// erased runtime. Captures automatically reuse the production value snapshot.
template<shader::Argument... Args>
class TypedOpenGLProgramRuntime final : public OpenGLProgramRuntime {
public:
    using signature = shader::Arguments<Args...>;
    [[nodiscard]] static std::expected<TypedOpenGLProgramRuntime, opengl::Diagnostic> create(
        const opengl::Device& device, shader::TypedGraphicsProgram<Args...> program)
    {
        auto runtime = OpenGLProgramRuntime::create(
            device, std::move(program).release_untyped());
        if (!runtime) return std::unexpected(std::move(runtime.error()));
        return TypedOpenGLProgramRuntime{std::move(*runtime)};
    }

    [[nodiscard]] opengl::TypedProgramView<Args...> production() const & noexcept
    { return opengl::TypedProgramView<Args...>{OpenGLProgramRuntime::production()}; }
    opengl::TypedProgramView<Args...> production() const && = delete;

    template<class... Values>
        requires shader::arguments_match_v<signature, Values...>
    [[nodiscard]] std::expected<void, opengl::Diagnostic> set_arguments(const Values&... values) const
    { return production().set_arguments(values...); }

private:
    explicit TypedOpenGLProgramRuntime(OpenGLProgramRuntime runtime) noexcept
        : OpenGLProgramRuntime(std::move(runtime)) {}
};

template<shader::Argument... Args>
std::expected<TypedOpenGLProgramRuntime<Args...>, opengl::Diagnostic> OpenGLProgramRuntime::create(
    const opengl::Device& device, shader::TypedGraphicsProgram<Args...> program)
{
    return TypedOpenGLProgramRuntime<Args...>::create(device, std::move(program));
}

} // namespace vng::render
