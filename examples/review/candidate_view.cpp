#include "candidate_view.hpp"
#include "../editor/animation.hpp"
#include "../editor/scene_file.hpp"
#include <algorithm>
#include <set>

namespace review {
using namespace vng;
namespace project = editor_example;

content::Result<project::State> CandidateView::load_scene(const std::filesystem::path& path) {
    project::SceneFile file;
    auto state = file.load(path);
    if (!state) return state;
    // Play the scene through its own cameras, as the demos do.
    state->viewport.mode = project::ViewMode::scene;
    state->viewport.pilot_camera = true;
    state->viewport.gizmo_only = false;
    std::set<project::BlueprintId> warmed;
    for (const auto& instance : state->document.instances)
        if (std::holds_alternative<project::MeshSettings>(instance.settings) && warmed.insert(instance.blueprint).second)
            if (const auto* geometry = project::mesh_geometry(std::as_const(*state), instance.blueprint))
                (void)geometry->picking_index();
    return state;
}

resources::Result<CandidateView> CandidateView::create(opengl::Device& device, project::State state) {
    auto owned = std::make_unique<project::State>(std::move(state));
    auto runtime = project::Runtime::create(device, *owned);
    if (!runtime) return std::unexpected(runtime.error());
    return CandidateView{std::move(owned), std::move(*runtime)};
}

CandidateView::CandidateView(std::unique_ptr<project::State> state, project::Runtime runtime)
    : state_(std::move(state)), runtime_(std::move(runtime)) {}

std::optional<f32> CandidateView::shown_time() const {
    return shown_ ? std::optional{shown_->time} : std::nullopt;
}

resources::Result<void> CandidateView::update(opengl::Device& device, f32 time, Extent2D extent) {
    if (auto ready = runtime_.poll_readback(); !ready) return std::unexpected(ready.error());
    else if (*ready) {
        if (const auto frame = in_flight_.find((*ready)->id); frame != in_flight_.end()) {
            shown_ = frame->second;
            image_ = std::make_shared<const gfx::ImageData>(std::move((*ready)->image));
            ++revision_;
        }
        std::erase_if(in_flight_, [&](const auto& entry) { return entry.first <= (*ready)->id; });
    }
    if (extent.width == 0 || extent.height == 0) return {};
    time = std::clamp(time, 0.F, duration());
    // Paused on a frame that is already shown or on its way: nothing to draw.
    if (requested_ && requested_->time == time && requested_->extent.width == extent.width &&
        requested_->extent.height == extent.height)
        return {};
    // Up to three frames in flight; polling always takes the newest finished one.
    if (!runtime_.readback_available()) return {};
    const Frame frame{time, extent, project::render_camera(*state_, time)};
    if (auto drawn = runtime_.render_frame(device, {*state_, frame.camera, extent, time}); !drawn) return drawn;
    const auto id = next_id_++;
    auto queued = runtime_.queue_readback(device, extent, id);
    if (!queued) return std::unexpected(queued.error());
    if (*queued) {
        in_flight_.emplace(id, frame);
        requested_ = frame;
    }
    return {};
}

std::optional<project::Pick> CandidateView::pick(Vec2 normalized) {
    if (!shown_) return {};
    state_->viewport.time = shown_->time;
    return project::pick(*state_, normalized, shown_->extent, &shown_->camera,
                         nullptr, {.camera_glyphs = false});
}

std::optional<Vec2> CandidateView::project(Vec3 point) const {
    if (!shown_) return {};
    const auto snapshot = shown_->camera.snapshot(shown_->extent);
    if (!snapshot) return {};
    std::array<double, 4> clip{};
    const std::array<double, 4> world{point.x, point.y, point.z, 1};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            clip[row] += double(snapshot->view_projection[column][row]) * world[column];
    if (clip[3] <= 1e-9) return {};
    return Vec2{static_cast<f32>((clip[0] / clip[3] + 1) * .5), static_cast<f32>((1 - clip[1] / clip[3]) * .5)};
}

std::string CandidateView::name_of(u32 object) const {
    const auto* instance = project::find_instance(*state_, object);
    return instance ? instance->name : std::string{};
}
} // namespace review
