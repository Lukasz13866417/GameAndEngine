#pragma once
#include "project.hpp"
#include "mesh_visibility.hpp"
#include <vng/opengl/device.hpp>
#include <vng/opengl/readback.hpp>
#include <vng/resources/diagnostic.hpp>
#include <vng/gfx/image.hpp>
#include <memory>

namespace vng::analysis { class AnalysisManifest; }

namespace editor_example {
struct VertexPosition;
// Immutable render inputs borrowed for one frame submission. The realized GPU
// resources live in Runtime; rendering never installs a camera into the document.
struct RenderRequest {
    const State& state;
    vng::gfx::Camera camera;
    vng::Extent2D extent;
    vng::f32 time;
    bool diagnostic{};
    bool annotations{}; // Editor preview only; independent play remains undecorated.
};
struct RuntimeStats {
    // vertex_bytes_uploaded counts partial update traffic, not initial/full uploads.
    vng::u64 full_mesh_uploads{}, vertex_updates{}, vertex_bytes_uploaded{};
    vng::u64 rendered_frames{}, readbacks{}, presented_frames{}, preview_rescales{};
    // Geometry allocations are owned per blueprint, never per instance.
    vng::u64 resident_meshes{};
    // Last production mesh pass; diagnostic captures and sun/postprocess passes
    // are separate. One renderer call per visible blueprint, not per instance.
    vng::u64 last_mesh_renderer_calls{}, last_mesh_draw_calls{}, last_mesh_instances{};
    // Cumulative work counters: camera-only redraws reuse animation samples;
    // lighting candidates count actual visible suns, never unrelated meshes.
    vng::u64 sampled_instances{}, light_candidates{};
    // CPU wall time, not GPU timestamps. Synchronous readback includes GPU waits;
    // async last_readback_ms measures only polling/copy work, not transfer latency.
    double last_mesh_update_ms{}, last_render_ms{}, last_readback_ms{}, last_present_ms{};
};
class Runtime {
public:
    static vng::resources::Result<Runtime> create(vng::opengl::Device&, const State&);
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    ~Runtime();
    vng::resources::Result<void> update_mesh(vng::opengl::Device&, const State&, std::span<const BlueprintId> only = {});
    // Private blueprint inspection mask. Scene-instance draws ignore it.
    void mesh_visibility(MeshVisibility);
    // Explicit invalidation from a validated position patch: no catalog,
    // topology, attribute, or unrelated-vertex scans on the interaction path.
    vng::resources::Result<void> update_positions(vng::opengl::Device&, BlueprintId,
                                                 std::span<const VertexPosition>);
    vng::resources::Result<void> render_frame(vng::opengl::Device&, const State&, vng::Extent2D,
                                              std::optional<vng::f32> time = {});
    vng::resources::Result<void> render_frame(vng::opengl::Device&, const RenderRequest&);
    vng::resources::Result<vng::gfx::ImageData> render(vng::opengl::Device&, const RenderRequest&);
    vng::resources::Result<vng::gfx::ImageData> readback(vng::opengl::Device&);
    vng::resources::Result<vng::gfx::ImageData> readback(vng::opengl::Device&,
                                                         vng::Extent2D output_extent);
    // Live preview uses bounded asynchronous transfers; the synchronous helpers
    // above remain for screenshots and evidence captures.
    [[nodiscard]] bool readback_available() const;
    [[nodiscard]] bool readback_pending() const;
    vng::resources::Result<bool> queue_readback(vng::opengl::Device&, vng::Extent2D, vng::u64 id);
    vng::resources::Result<std::optional<vng::opengl::Rgba8Readback>> poll_readback();
    void discard_readbacks();
    vng::resources::Result<void> present(vng::opengl::Device&);
    vng::resources::Result<vng::gfx::ImageData> render(vng::opengl::Device&, const State&,
                                                       vng::Extent2D, bool diagnostic,
                                                       std::optional<vng::f32> time = {});
    [[nodiscard]] std::string_view diagnostics() const;
    // Neutral source-face and entity/blueprint provenance of the last diagnostic
    // render. Borrowed until the next render; null for normal/failed frames.
    [[nodiscard]] const vng::analysis::AnalysisManifest* diagnostic_manifest() const;
    [[nodiscard]] const RuntimeStats& stats() const;

private:
    struct Impl;
    explicit Runtime(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
    vng::resources::Result<const vng::opengl::Framebuffer*> prepare_readback(
        vng::opengl::Device&, vng::Extent2D);
    vng::resources::Result<std::optional<vng::gfx::ImageData>> draw(vng::opengl::Device&,
                                                                    const State&, const vng::gfx::Camera&, vng::Extent2D,
                                                                    bool diagnostic,
                                                                    std::optional<vng::f32> time, bool annotations = false);
};
} // namespace editor_example
