#include "editor/effects.hpp"
#include "editor/animation.hpp"
#include "editor/edits.hpp"
#include "editor/playback.hpp"
#include "editor/position_edits.hpp"
#include "editor/rotation_edits.hpp"
#include "editor/scale_edits.hpp"
#include "editor/selection_edits.hpp"
#include "editor/navigation.hpp"
#include "editor/runtime.hpp"
#include "editor/preview_viewport.hpp"
#include "editor/settings.hpp"
#include "editor/viewport_session.hpp"
#include "editor/document_patch.hpp"
#include "support/glfw_opengl_session.hpp"

#include <vng/editor/preview.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <sstream>
#include <string_view>

namespace {
using Clock = std::chrono::steady_clock;
namespace preview = vng::editor::preview;
namespace editor = vng::editor;
namespace project = editor_example;
constexpr vng::Extent2D viewport_extent = project::default_preview_extent;

template <class T> std::optional<T> parse_integer(std::string_view text) {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return {};
    return value;
}

std::string message(const vng::resources::Diagnostic& error) {
    std::string out = error.message;
    for (const auto& note : error.context)
        out += "\n" + note;
    if (!error.driver_log.empty())
        out += "\nDriver log:\n" + error.driver_log;
    if (!error.generated_source.empty())
        out += "\nGenerated shader:\n" + error.generated_source;
    return out;
}

// Native navigation is a private view, not an unsolicited authored edit.
// Override only the camera for a render/input scope; never copy the mesh.
struct CameraScope {
    project::State& state;
    project::CameraPose original;
    CameraScope(project::State& state, const std::optional<project::CameraPose>& pose)
        : state(state), original(state.viewport.editor_camera) {
        if (pose) state.viewport.editor_camera = *pose;
    }
    ~CameraScope() { state.viewport.editor_camera = original; }
};

// A worker owns the realization of the editor's authored snapshot, never the
// editor document or undo stack. All callbacks and GPU calls run on this one
// context-owning thread, between frames.
class Worker final {
public:
    Worker(preview::WorkerEndpoint& endpoint, vng::opengl::Device& device,
           vng::glfw_opengl::Window& window, vng::u64 generation)
        : endpoint_(endpoint), device_(device), window_(window), generation_(generation),
          preview_extent_(project::fit_preview_extent(viewport_extent, endpoint.capacity())) {}

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] int exit_code() const noexcept { return fatal_error_ ? 1 : 0; }
    [[nodiscard]] bool wants_frame() const noexcept {
        if (!state_ || render_blocked_)
            return false;
        if (playing_)
            return !window_.framebuffer_extent().empty();
        return (!ready_ || linked_) && (dirty_ || !state_->viewport.paused || !zoom_.settled());
    }
    [[nodiscard]] Clock::time_point next_frame() const noexcept { return next_frame_; }
    [[nodiscard]] bool transfer_pending() const {
        return pending_image_.has_value() || (runtime_ && runtime_->readback_pending());
    }
    // Independent of rendering demand: a single final edit must complete even
    // when the scene goes idle or the UI temporarily owns shared image storage.
    void transfer() {
        if (!runtime_) return;
        if (runtime_->readback_pending()) {
            auto completed = runtime_->poll_readback();
            if (!completed) { discard_transfers(); rendering_failed(message(completed.error())); return; }
            if (*completed) {
                auto value = std::move(**completed);
                const auto found = pending_frames_.find(value.id);
                if (found != pending_frames_.end()) {
                    auto metadata = std::move(found->second);
                    metadata.info.interaction.readback_ready_ns = vng::monotonic_ns();
                    pending_image_ = PendingImage{std::move(metadata), std::move(value.image)};
                    pending_frames_.erase(pending_frames_.begin(), std::next(found));
                }
            }
        }
        if (!pending_image_) return;
        auto& completed = *pending_image_;
        auto published = endpoint_.try_publish(completed.metadata.info, completed.image.pixels);
        if (!published) throw std::runtime_error(published.error().message);
        if (!*published) return; // Retry only the newest image; never block on the UI.
        if (linked_ && completed.metadata.diagnostic != last_diagnostic_) {
            last_diagnostic_ = std::move(completed.metadata.diagnostic);
            send("info\n" + last_diagnostic_);
        }
        pending_image_.reset();
        if (!ready_) {
            auto ready = endpoint_.ready();
            if (!ready) throw std::runtime_error(ready.error().message);
            ready_ = true;
        }
    }

    void window_events() {
        window_.poll_events();
        auto input = window_.take_input();
        if (window_.should_close() || (playing_ && input.keyDown(vng::input::Key::escape))) {
            window_.cancel_close();
            set_play(false);
        }
        if (playing_ && state_) {
            if (!play_camera_override_) play_camera_ = scene_camera(static_cast<vng::f32>(play_time_));
            CameraScope view{*state_, play_camera_};
            if (navigation_.update(*state_, {}, input.logical_size, input.events, input.events,
                                   input.focused && !input.overflow && !input.framebuffer.empty())) {
                play_camera_ = state_->viewport.editor_camera;
                play_camera_override_ = true;
            }
        } else
            navigation_.cancel();
    }

    void report(std::string text) {
        std::cerr << text << '\n';
        // Very large compiler/driver diagnostics still leave a useful error
        // on the control channel without violating its packet-size contract.
        constexpr auto limit = preview::max_message_bytes - 64;
        if (text.size() > limit) {
            text.resize(limit);
            text += "\n[diagnostic truncated]";
        }
        send("error\n" + text);
    }

    void packets(const std::vector<std::string>& incoming) {
        std::vector<std::string_view> packets;
        std::vector<editor::InteractionTrace> traces;
        for (const auto& bytes : incoming) {
            const auto received = vng::monotonic_ns();
            auto decoded = editor::receive_interaction(bytes);
            if (!decoded) { report(decoded.error()); continue; }
            packets.push_back(decoded->payload);
            traces.push_back({.origin = decoded->origin, .worker_received_ns = received});
        }
        for (std::size_t i = 0; i < packets.size() && running_; ++i) {
            request_trace_ = traces[i];
            if(packets[i].starts_with("mesh_visibility\n")) {
                if(ready_ && !linked_) continue;
                auto visibility=project::decode_mesh_visibility(packets[i].substr(16));
                if(!visibility) report(visibility.error());
                else pending_visibility_=std::move(*visibility);
                continue;
            }
            if (packets[i].starts_with("view\n")) {
                if (ready_ && !linked_) continue;
                auto view = project::decode_viewport_request(packets[i].substr(5));
                if (!view) report(view.error());
                else view_inbox_.offer(*view, request_trace_);
                continue;
            }
            if (packets[i].starts_with("settings\n")) {
                auto settings = project::decode_settings(std::string_view(packets[i]).substr(9));
                if (!settings) report(settings.error().message);
                else {
                    if (auto applied = window_.set_vsync(playing_ ? settings->vsync : vng::window::VSync::off); !applied) {
                        report(applied.error().message); continue;
                    }
                    settings_ = *settings;
                    dirty_ = true;
                    next_frame_ = next_preview_ = {};
                }
                continue;
            }
            if (packets[i].starts_with("snapshot\n")) {
                if (ready_ && !linked_)
                    continue;
                // Only adjacent snapshots commute. Never move an event past
                // a snapshot: its stamp refers to the state at that position.
                std::optional<project::State> latest;
                do {
                    ++scene_decode_calls_;
                    auto decoded = project::decode(std::string_view(packets[i]).substr(9));
                    if (!decoded)
                        report(decoded.error().message);
                    else if ((!state_ || decoded->document.revision >= state_->document.revision) &&
                             (!latest || decoded->document.revision >= latest->document.revision))
                        { latest = std::move(*decoded); request_trace_ = traces[i]; }
                    else
                        report("Ignoring a stale editor snapshot");
                    if (i + 1 == packets.size() || !packets[i + 1].starts_with("snapshot\n"))
                        break;
                    ++i;
                } while (true);
                if (latest)
                    accept_snapshot(std::move(*latest));
                else
                    announce(false);
                continue;
            }
            const auto& packet = packets[i];
            if (packet.starts_with("viewport\n")) {
                auto size = project::decode_viewport_size(std::string_view(packet).substr(9),
                                                          endpoint_.capacity());
                if (!size)
                    report(size.error().message);
                else if (*size != preview_extent_) {
                    discard_transfers();
                    preview_extent_ = *size;
                    dirty_ = true;
                    render_blocked_ = false;
                }
                continue;
            }
            if (packet == "shutdown") {
                running_ = false;
                return;
            }
            if (packet == "play\n0" || packet == "play\n1") {
                set_play(packet.back() == '1');
            } else if (packet == "link\n0" || packet == "link\n1") {
                linked_ = packet.back() == '1';
                if (!linked_) discard_transfers();
                dirty_ = true;
                next_preview_ = {};
                render_blocked_ = false;
                status();
                if (linked_)
                    announce(false);
            } else if (packet == "close") {
                window_.request_close();
            } else if (packet == "stats") {
                statistics();
            } else if (packet.starts_with("launch\n")) {
                ++scene_decode_calls_;
                auto decoded = project::decode(std::string_view(packet).substr(7));
                if (!decoded)
                    report(decoded.error().message);
                else if (state_ && decoded->document.revision < state_->document.revision)
                    report("Cannot launch an older authored scene revision");
                else if (accept_snapshot(std::move(*decoded)))
                    set_play(true, true);
            } else if (!linked_ && ready_) {
                // A disconnected editor owns any local edits. Nothing is
                // decoded/applied until it explicitly reconnects/resends or
                // requests one-shot launch with a complete authored snapshot.
                continue;
            } else if (packet.starts_with("patch\n")) {
                auto decoded = project::decode_patch(packet.substr(6));
                if (!decoded) send("resync\n" + decoded.error().message);
                else accept_patch(*decoded);
            } else if (packet.starts_with("vertices\n")) {
                auto decoded = project::decode_edit(std::string_view(packet).substr(9));
                if (!decoded)
                    send("resync\n" + decoded.error().message);
                else
                    accept_vertices(*decoded);
            } else if (packet.starts_with("position\n")) {
                auto decoded = project::decode_position_edit(std::string_view(packet).substr(9));
                if (!decoded)
                    send("resync\n" + decoded.error().message);
                else
                    accept_position(*decoded);
            } else if (packet.starts_with("scale\n")) {
                auto decoded = project::decode_scale_edit(std::string_view(packet).substr(6));
                if (!decoded) send("resync\n" + decoded.error().message);
                else accept_scale(*decoded);
            } else if (packet.starts_with("rotation\n")) {
                auto decoded = project::decode_rotation_edit(std::string_view(packet).substr(9));
                if (!decoded)
                    send("resync\n" + decoded.error().message);
                else
                    accept_rotation(*decoded);
            } else if (packet.starts_with("selection\n")) {
                auto decoded = project::decode_selection_edit(std::string_view(packet).substr(10));
                if (!decoded)
                    send("resync\n" + decoded.error().message);
                else
                    accept_selection(*decoded);
            } else if (packet.starts_with("playback\n")) {
                auto decoded = project::decode_playback_edit(std::string_view(packet).substr(9));
                if (!decoded)
                    send("resync\n" + decoded.error().message);
                else
                    accept_playback(*decoded);
            } else if (packet.starts_with("event\n")) {
                auto decoded = editor::decode_event(std::string_view(packet).substr(6));
                if (!decoded) {
                    report(decoded.error().message);
                    announce(true);
                } else
                    accept_event(*decoded);
            } else if (packet == "diagnostic\n0" || packet == "diagnostic\n1") {
                discard_transfers();
                diagnostic_ = packet.back() == '1';
                dirty_ = true;
                render_blocked_ = false;
            } else
                report("Unknown preview worker message");
        }
        apply_view();
        if(state_ && runtime_ && pending_visibility_ &&
           pending_visibility_->required_document_revision<=state_->document.revision) {
            runtime_->mesh_visibility(std::move(pending_visibility_->visibility));
            pending_visibility_.reset();dirty_=true;render_blocked_=false;
        }
    }

    // Tick the playback clock even while a low render cap delays presentation.
    // Long actual stalls retain the existing 0.25 s safety clamp; deliberate
    // frame pacing does not slow the animation down.
    void update_playback() {
        const auto now = Clock::now();
        if (state_ && (playing_ || ((!ready_ || linked_) && !state_->viewport.paused))) {
            const auto elapsed = std::clamp(
                std::chrono::duration<double>(now - simulation_clock_).count(), 0.0, 0.25);
            auto& time = playing_ ? play_time_ : preview_time_;
            time = project::advance_playback(time, elapsed, state_->document.timeline_duration);
        }
        simulation_clock_ = now;
    }

    void render() {
        if (!wants_frame())
            return;
        const auto now = Clock::now();
        if (now < next_frame_)
            return;
        // Interactive editing favours freshness over queued throughput. Input
        // still runs while transfer is pending; do not spend another expensive
        // CPU render hiding completion of the previous image from the UI.
        // Independent Play can pipeline the bounded debug-transfer queue.
        if (!playing_ && (runtime_->readback_pending() || pending_image_)) return;
        const auto time = static_cast<vng::f32>(playing_ ? play_time_ : preview_time_);
        if (!playing_) {
            zoom_.target(state_->viewport.editor_camera, state_->viewport.smooth_zoom);
            zoom_.advance(std::chrono::duration<double>(now - view_clock_).count());
        }
        view_clock_ = now;
        const auto rendered_pose = playing_
            ? (play_camera_override_ ? *play_camera_ : scene_camera(time))
            : zoom_.pose();
        const auto rendered_camera = project::camera(rendered_pose,state_->viewport.mode,settings_.maximum_viewing_distance);
        render_trace_ = active_trace_;
        render_trace_.render_started_ns = vng::monotonic_ns();
        next_frame_ = now + project::frame_interval(playing_ ? settings_.play_fps : settings_.preview_fps);
        if (playing_) {
            const auto native_extent = window_.framebuffer_extent();
            auto rendered = runtime_->render_frame(device_, project::RenderRequest{*state_,rendered_camera,native_extent,time});
            if (!rendered) {
                rendering_failed(message(rendered.error()));
                return;
            }
            auto copied = runtime_->present(device_);
            if (!copied) {
                rendering_failed(message(copied.error()));
                return;
            }
            auto presented = window_.present();
            if (!presented) {
                rendering_failed(presented.error().message);
                return;
            }
            // No readback, shared-memory copy, preview serialization, or
            // periodic telemetry at all on the disconnected native path.
            if (!linked_ || now < next_preview_)
                return;
            next_preview_ = now + project::frame_interval(settings_.debug_fps);
            const auto preview_extent = project::fit_preview_extent(native_extent, preview_extent_);
            // Debugging Play streams the production image, never a second
            // diagnostic render that would resize the native render targets.
            queue_preview(preview_extent, time, rendered_pose);
            return;
        }
        const project::RenderRequest request{*state_,rendered_camera,preview_extent_,time,diagnostic_,true};
        if (diagnostic_) {
            // Evidence capture computes CPU analysis of several attachments; it
            // remains an explicit synchronous inspection, not the live color path.
            discard_transfers();
            auto image = runtime_->render(device_, request);
            if (!image) { rendering_failed(message(image.error())); return; }
            auto metadata = frame_metadata(image->extent, time, rendered_pose);
            metadata.info.interaction.readback_ready_ns = vng::monotonic_ns();
            pending_image_ = PendingImage{std::move(metadata), std::move(*image)};
            dirty_ = false;
        } else {
            auto rendered = runtime_->render_frame(device_, request);
            if (!rendered) { rendering_failed(message(rendered.error())); return; }
            queue_preview(preview_extent_, time, rendered_pose);
        }
    }

private:
    void status() {
        send(std::string("status\nplay=") + (playing_ ? "1" : "0") +
             ";link=" + (linked_ ? "1" : "0"));
    }

    void statistics() {
        const auto stats = runtime_ ? runtime_->stats() : project::RuntimeStats{};
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "stats\nrendered_frames=" << stats.rendered_frames
            << ";readbacks=" << stats.readbacks << ";presented_frames=" << stats.presented_frames
            << ";full_mesh_uploads=" << stats.full_mesh_uploads
            << ";resident_meshes=" << stats.resident_meshes
            << ";mesh_renderer_calls=" << stats.last_mesh_renderer_calls
            << ";mesh_draw_calls=" << stats.last_mesh_draw_calls
            << ";mesh_instances=" << stats.last_mesh_instances
            << ";sampled_instances=" << stats.sampled_instances
            << ";light_candidates=" << stats.light_candidates
            << ";vertex_updates=" << stats.vertex_updates
            << ";vertex_bytes_uploaded=" << stats.vertex_bytes_uploaded
            << ";preview_rescales=" << stats.preview_rescales << ";play=" << (playing_ ? 1 : 0)
            << ";link=" << (linked_ ? 1 : 0) << ";visible=" << (window_.visible() ? 1 : 0)
            << ";time=" << (playing_ ? play_time_ : preview_time_)
            << ";revision=" << (state_ ? state_->document.revision : 0)
            << ";last_render_ms=" << stats.last_render_ms
            << ";last_readback_ms=" << stats.last_readback_ms
            << ";preview_width=" << preview_extent_.width
            << ";preview_height=" << preview_extent_.height
            << ";last_mesh_update_ms=" << stats.last_mesh_update_ms
            << ";preview_fps=" << settings_.preview_fps << ";play_fps=" << settings_.play_fps
            << ";debug_fps=" << settings_.debug_fps
            << ";vsync=" << (window_.vsync() == vng::window::VSync::on ? 1 : 0);
        out << ";scene_encode_calls=" << scene_encode_calls_
            << ";scene_decode_calls=" << scene_decode_calls_
            << ";mesh_update_calls=" << mesh_update_calls_
            << ";position_edits=" << position_edits_
            << ";rotation_edits=" << rotation_edits_
            << ";scale_edits=" << scale_edits_
            << ";document_patches=" << document_patches_
            << ";inspector_rebuilds=" << inspector_rebuilds_
            << ";schema_sends=" << schema_sends_
            << ";selection_edits=" << selection_edits_
            << ";selected_object=" << (state_ ? state_->viewport.selected_object : 0)
            << ";selected_vertex=" << (state_ ? state_->viewport.selected_vertex : 0);
        send(out.str());
    }

    void set_play(bool play, bool restart = false) {
        if (play && (!state_ || !ready_)) {
            report("Play requires a completed initial preview");
            return;
        }
        if (auto applied = window_.set_vsync(play ? settings_.vsync : vng::window::VSync::off); !applied) {
            report(applied.error().message); return;
        }
        if (play != playing_ || restart) discard_transfers();
        if (play && (!playing_ || restart)) {
            navigation_.cancel();
            play_camera_override_ = false;
            diagnostic_ = false;
            play_time_ = preview_time_;
            simulation_clock_ = Clock::now();
            window_.cancel_close();
            window_.show();
            render_blocked_ = false;
            next_frame_ = {};
            next_preview_ = {};
        } else if (!play) {
            navigation_.cancel();
            play_camera_.reset();
            window_.hide();
            window_.cancel_close();
            if (playing_) {
                preview_time_ = state_->viewport.time;
                simulation_clock_ = Clock::now();
                dirty_ = true;
                next_frame_ = {};
            }
        }
        playing_ = play;
        status();
    }

    void revision() {
        accept_trace();
        if (linked_ && state_)
            send("revision\n" + std::to_string(state_->document.revision));
    }

    void rendering_failed(std::string error) {
        report(std::move(error));
        render_blocked_ = true;
        if (!ready_) {
            running_ = false;
            fatal_error_ = true;
        }
    }

    struct PendingMetadata { preview::FrameInfo info; std::string diagnostic; };
    struct PendingImage { PendingMetadata metadata; vng::gfx::ImageData image; };
    void discard_transfers() {
        if (runtime_) runtime_->discard_readbacks();
        pending_frames_.clear(); pending_image_.reset();
    }
    PendingMetadata frame_metadata(vng::Extent2D extent, vng::f32 time, const project::CameraPose& pose) {
        if (frame_id_ == std::numeric_limits<vng::u64>::max())
            throw std::runtime_error("Preview frame IDs exhausted");
        preview::FrameInfo info{extent.width, extent.height, generation_,
                               state_->document.revision, ++frame_id_, static_cast<double>(time)};
        info.view_sequence = state_->viewport.sequence;
        info.view_camera = {pose.yaw,pose.pitch,pose.distance,pose.target.x,pose.target.y,pose.target.z,pose.zoom};
        info.view_far_plane = settings_.maximum_viewing_distance;
        info.view_mode = static_cast<vng::u32>(state_->viewport.mode);
        info.inspected_mesh = static_cast<vng::u32>(state_->viewport.inspected_mesh);
        info.has_view = true;
        info.settled = playing_ || zoom_.settled();
        info.interaction = render_trace_;
        return {info, std::string(runtime_->diagnostics())};
    }
    void queue_preview(vng::Extent2D extent, vng::f32 time, const project::CameraPose& pose) {
        if (!runtime_->readback_available()) return;
        auto metadata = frame_metadata(extent, time, pose);
        auto queued = runtime_->queue_readback(device_, extent, metadata.info.frame_id);
        if (!queued) { rendering_failed(message(queued.error())); return; }
        if (!*queued) return;
        pending_frames_.emplace(metadata.info.frame_id, std::move(metadata));
        dirty_ = false; // Completion must not clear a newer input's dirty bit.
    }
    void send(std::string_view text) {
        auto sent = endpoint_.send(text);
        if (!sent)
            throw std::runtime_error(sent.error().message);
    }

    void rebuild_inspector() {
        ++inspector_rebuilds_;
        inspector_.emplace(editor::Stamp{state_->viewport.selected_object, generation_, state_->document.revision,
                                         state_->viewport.sequence});
        controls_->describe_editor(*inspector_);
        auto valid = editor::validate(inspector_->schema());
        if (!valid)
            throw std::runtime_error(valid.error().message);
    }

    void announce(bool include_state) {
        if (!linked_ || !state_ || !inspector_)
            return;
        if (include_state) {
            ++scene_encode_calls_;
            auto snapshot = project::encode(*state_);
            if (!snapshot)
                throw std::runtime_error(snapshot.error().message);
            send("state\n" + *snapshot);
        }
        auto schema = editor::encode_schema(inspector_->schema());
        if (!schema)
            throw std::runtime_error(schema.error().message);
        send("schema\n" + *schema);
        ++schema_sends_;
    }

    bool accept_snapshot(project::State candidate) {
        if (state_ && candidate.viewport.sequence < state_->viewport.sequence)
            candidate.viewport = state_->viewport;
        // Validate user-defined descriptions before swapping GPU resources or
        // authored state. A declaration error must not replace the good view.
        project::ProjectControls check_controls{candidate};
        editor::Inspector check{candidate.viewport.selected_object, generation_, candidate.document.revision};
        try {
            check_controls.describe_editor(check);
            if (auto valid = editor::validate(check.schema()); !valid) {
                report(valid.error().message);
                announce(false);
                return false;
            }
        } catch (const std::exception& error) {
            report(std::string("Cannot describe project controls: ") + error.what());
            announce(false);
            return false;
        }

        const bool reset_preview_time = !state_ || candidate.viewport.time != state_->viewport.time;
        const bool reset_simulation_clock =
            reset_preview_time || (state_ && candidate.viewport.paused != state_->viewport.paused);
        const auto play_time = static_cast<vng::f32>(play_time_);
        const bool reset_play_camera =
            playing_ && state_ &&
            (project::evaluate_camera(candidate, play_time) != project::evaluate_camera(*state_, play_time) ||
             candidate.viewport.mode != state_->viewport.mode);
        if (!runtime_) {
            auto created = project::Runtime::create(device_, candidate);
            if (!created) {
                report(message(created.error()));
                running_ = false;
                fatal_error_ = true;
                return false;
            }
            state_.emplace(std::move(candidate));
            controls_.emplace(*state_);
            runtime_.emplace(std::move(*created));
        } else {
            ++mesh_update_calls_;
            auto uploaded = runtime_->update_mesh(device_, candidate);
            if (!uploaded) {
                report(message(uploaded.error()));
                announce(false);
                return false;
            }
            // Assignment preserves State's address; ProjectControls::state_
            // and native callback captures therefore remain valid.
            *state_ = std::move(candidate);
        }
        rebuild_inspector();
        if (reset_play_camera) follow_scene_camera();
        if (reset_preview_time) {
            preview_time_ = state_->viewport.time;
        }
        if (reset_simulation_clock && !playing_)
            simulation_clock_ = Clock::now();
        dirty_ = true;
        render_blocked_ = false;
        revision();
        announce(false);
        return true;
    }

    void accept_trace() {
        active_trace_ = request_trace_;
        active_trace_.worker_applied_ns = vng::monotonic_ns();
    }
    void apply_view() {
        if (!state_) return;
        const auto before = state_->viewport;
        if (!view_inbox_.apply(*state_, active_trace_)) return;
        const auto& after = state_->viewport;
        if(before.mode!=after.mode || before.inspected_mesh!=after.inspected_mesh) {
            const bool draft_switch=(before.mode==project::ViewMode::mesh && state_->document.mesh_drafts.contains(before.inspected_mesh)) ||
                (after.mode==project::ViewMode::mesh && state_->document.mesh_drafts.contains(after.inspected_mesh));
            if(draft_switch && runtime_) if(auto updated=runtime_->update_mesh(device_,*state_);!updated) {
                state_->viewport=before;
                report(message(updated.error()));send("resync\nMesh draft view update failed");return;
            }
        }
        const bool was_settled = zoom_.settled();
        zoom_.target(after.editor_camera, after.smooth_zoom);
        if (was_settled) view_clock_ = Clock::now();
        if (before.time != after.time || before.paused != after.paused) {
            preview_time_ = after.time;
            if (!playing_) simulation_clock_ = Clock::now();
        }
        // Camera/zoom do not change inspector values, schemas, geometry, or
        // timeline widgets. Only an actual inspection-target/value change does.
        if (before.mode != after.mode || before.inspected_mesh != after.inspected_mesh ||
            before.selected_object != after.selected_object || before.selected_vertex != after.selected_vertex ||
            before.time != after.time || before.paused != after.paused) {
            if (before.selected_object != after.selected_object || before.selected_vertex != after.selected_vertex)
                ++selection_edits_;
            rebuild_inspector();
            announce(false);
        }
        if (before.mode != after.mode || before.inspected_mesh != after.inspected_mesh)
            zoom_.target(after.editor_camera, false);
        dirty_ = true;
        render_blocked_ = false;
    }

    void accept_playback(const project::PlaybackEdit& edit) {
        if (!state_ || !runtime_) {
            send("resync\nPreview has no authored snapshot");
            return;
        }
        if (auto applied = project::apply_playback_edit(*state_, edit); !applied) {
            send("resync\n" + applied.error().message);
            return;
        }
        preview_time_ = state_->viewport.time;
        if (!playing_)
            simulation_clock_ = Clock::now();
        rebuild_inspector();
        dirty_ = true;
        render_blocked_ = false;
        revision();
        announce(false);
    }

    // Independent Play looks through the scene camera; a scene without one
    // keeps the editor's view rather than inventing a pose.
    project::CameraPose scene_camera(vng::f32 time) const {
        return project::evaluate_camera(*state_, time).value_or(state_->viewport.editor_camera);
    }
    // An authored change to the scene camera hands the Play view back to it,
    // discarding any navigation the viewer did inside Play.
    void follow_scene_camera() {
        navigation_.cancel();
        play_camera_override_ = false;
    }

    void accept_selection(const project::SelectionEdit& edit) {
        if (!state_ || !runtime_) {
            send("resync\nPreview has no authored snapshot");
            return;
        }
        if (auto applied = project::apply_selection_edit(*state_, edit); !applied) {
            send("resync\n" + applied.error().message);
            return;
        }
        // Only transient editor selection and its revision changed. Rebuild
        // callbacks/schema immediately; meshes and playback clocks stay intact.
        ++selection_edits_;
        rebuild_inspector();
        dirty_ = true;
        render_blocked_ = false;
        revision();
        announce(false);
    }

    void accept_position(const project::PositionEdit& edit) {
        if (!state_ || !runtime_) {
            send("resync\nPreview has no authored snapshot");
            return;
        }
        if (auto applied = project::apply_position_edit(*state_, edit); !applied) {
            send("resync\n" + applied.error().message);
            return;
        }
        // This protocol owns one validated position property, including its
        // optional animation track. Geometry, other settings and playback
        // clocks are untouched; no full-scene encode or GPU update is needed.
        ++position_edits_;
        rebuild_inspector();
        dirty_ = true;
        render_blocked_ = false;
        revision();
        announce(false);
    }

    void accept_scale(const project::ScaleEdit& edit) {
        if (!state_ || !runtime_) { send("resync\nPreview has no authored snapshot"); return; }
        if (auto applied=project::apply_scale_edit(*state_,edit); !applied) {
            send("resync\n" + applied.error().message); return;
        }
        ++scale_edits_;
        rebuild_inspector();
        dirty_=true;
        render_blocked_=false;
        revision();
        announce(false);
    }
    void accept_rotation(const project::RotationEdit& edit) {
        if (!state_ || !runtime_) {
            send("resync\nPreview has no authored snapshot");
            return;
        }
        if (auto applied = project::apply_rotation_edit(*state_, edit); !applied) {
            send("resync\n" + applied.error().message);
            return;
        }
        // The instance transform/its own animation track are the only writes.
        // Keep mesh allocations, playback clocks and unrelated tracks intact.
        ++rotation_edits_;
        rebuild_inspector();
        dirty_ = true;
        render_blocked_ = false;
        revision();
        announce(false);
    }

    void accept_vertices(const project::VertexEdit& edit) {
        if (!state_ || !runtime_) {
            send("resync\nPreview has no authored snapshot");
            return;
        }
        if (edit.base_revision != state_->document.revision) {
            send("resync\nVertex edit base revision is stale");
            return;
        }
        auto* geometry = project::mesh_edit_geometry(
            *state_, static_cast<project::BlueprintId>(edit.blueprint));
        if (!geometry) {
            send("resync\nVertex edit refers to a missing mesh blueprint");
            return;
        }
        std::vector<std::pair<vng::u32, vng::Vec3>> previous;
        previous.reserve(edit.vertices.size());
        for (const auto& vertex : edit.vertices) {
            if (vertex.index >= geometry->size()) {
                send("resync\nVertex index is outside the mesh");
                return;
            }
            previous.emplace_back(vertex.index, geometry->position(vertex.index));
        }
        auto applied = project::apply_edit(*state_, edit);
        if (!applied) {
            send("resync\n" + applied.error().message);
            return;
        }
        ++mesh_update_calls_;
        const auto blueprint=static_cast<project::BlueprintId>(edit.blueprint);
        auto updated = project::mesh_view_geometry(*state_,blueprint)==project::mesh_edit_geometry(*state_,blueprint)
            ?runtime_->update_positions(device_,blueprint,edit.vertices):vng::resources::Result<void>{};
        if (!updated) {
            for (const auto& [index, position] : previous)
                (void)geometry->set_position(index, position);
            state_->document.revision = edit.base_revision;
            report(message(updated.error()));
            send("resync\nVertex GPU update failed");
            return;
        }
        rebuild_inspector();
        dirty_ = true;
        render_blocked_ = false;
        revision();
        announce(false);
    }

    void accept_patch(const project::DocumentPatch& patch) {
        if (!state_ || !runtime_) { send("resync\nPreview has no document"); return; }
        const auto play_time = static_cast<vng::f32>(play_time_);
        const auto camera_before = playing_ ? project::evaluate_camera(*state_, play_time) : std::nullopt;
        auto before = project::capture_patch(patch.base_revision, *state_, project::changes_of(patch));
        if (!before) { send("resync\n" + before.error().message); return; }
        // An inverse uses the same validated revision interval during rollback;
        // neither the revision nor a partially updated GPU state is published.
        before->revision = patch.revision;
        for (auto& edit : before->vertices) edit.revision = patch.revision;
        if (auto applied = project::apply_patch(*state_, patch); !applied) {
            send("resync\n" + applied.error().message); return;
        }
        if(!patch.meshes.empty()) {
            std::vector<project::BlueprintId> affected;
            for(const auto& edit:patch.meshes) {
                const auto id=static_cast<project::BlueprintId>(edit.blueprint);
                if(state_->viewport.mode==project::ViewMode::mesh && state_->viewport.inspected_mesh==id)affected.push_back(id);
            }
            for(const auto& edit:patch.vertices) {
                const auto id=static_cast<project::BlueprintId>(edit.blueprint);
                if(project::mesh_view_geometry(*state_,id)==project::mesh_edit_geometry(*state_,id))affected.push_back(id);
            }
            if(!affected.empty()) {
                ++mesh_update_calls_;
                if(auto uploaded=runtime_->update_mesh(device_,*state_,affected);!uploaded) {
                    state_->document.revision=patch.base_revision;
                    if(auto restored=project::apply_patch(*state_,*before);!restored)
                        throw std::runtime_error("Mesh patch rollback failed: "+restored.error().message);
                    state_->document.revision=patch.base_revision;
                    send("resync\n"+message(uploaded.error()));return;
                }
            }
        }
        for (std::size_t i = 0; patch.meshes.empty() && i < patch.vertices.size(); ++i) {
            const auto& edit = patch.vertices[i];
            const auto blueprint=static_cast<project::BlueprintId>(edit.blueprint);
            if(project::mesh_view_geometry(*state_,blueprint)!=project::mesh_edit_geometry(*state_,blueprint)) continue;
            ++mesh_update_calls_;
            auto uploaded = runtime_->update_positions(device_, static_cast<project::BlueprintId>(edit.blueprint), edit.vertices);
            if (!uploaded) {
                for (std::size_t j = 0; j < i; ++j) {
                    const auto& undo = *std::ranges::find(before->vertices, patch.vertices[j].blueprint,
                                                          &project::VertexEdit::blueprint);
                    const auto undo_blueprint=static_cast<project::BlueprintId>(undo.blueprint);
                    if(project::mesh_view_geometry(*state_,undo_blueprint)!=project::mesh_edit_geometry(*state_,undo_blueprint)) continue;
                    if (auto restored = runtime_->update_positions(device_, static_cast<project::BlueprintId>(undo.blueprint), undo.vertices); !restored)
                        report("Patch GPU rollback failed: " + message(restored.error()));
                }
                state_->document.revision = patch.base_revision;
                if (auto restored = project::apply_patch(*state_, *before); !restored)
                    throw std::runtime_error("Patch rollback failed: " + restored.error().message);
                state_->document.revision = patch.base_revision;
                send("resync\n" + message(uploaded.error())); return;
            }
        }
        ++document_patches_;
        if (playing_ && project::evaluate_camera(*state_, play_time) != camera_before) follow_scene_camera();
        rebuild_inspector(); dirty_ = true; render_blocked_ = false;
        revision(); announce(false);
    }

    void accept_event(const editor::Event& event) {
        if (!state_ || !runtime_ || !inspector_) {
            report("Preview has no authored snapshot yet");
            return;
        }
        (void)controls_->take_changes();
        const auto base_revision = state_->document.revision;
        const auto previous_stamp = inspector_->schema().stamp;
        auto applied = inspector_->dispatch(event);
        if (!applied) {
            // ProjectControls stages/validates only its affected instance and
            // animation values. Failed callbacks commit no document writes.
            (void)controls_->take_changes();
            if (inspector_->schema().stamp != previous_stamp)
                rebuild_inspector();
            report(applied.error().message);
            announce(false);
            return;
        }
        state_->document.revision = inspector_->schema().stamp.revision;
        const auto changes = controls_->take_changes();
        auto patch = project::capture_patch(base_revision, *state_, changes);
        if (!patch) throw std::runtime_error(patch.error().message);
        auto bytes = project::encode_patch(*patch);
        if (!bytes) throw std::runtime_error(bytes.error().message);
        // A drag needs the original position until its terminal packet. All
        // other callbacks may change which controls the project describes.
        if (event.phase != editor::Phase::begin && event.phase != editor::Phase::update)
            rebuild_inspector();
        dirty_ = true;
        render_blocked_ = false;
        revision();
        send("state_patch\n" + *bytes);
        announce(false);
    }

    preview::WorkerEndpoint& endpoint_;
    vng::opengl::Device& device_;
    vng::glfw_opengl::Window& window_;
    vng::u64 generation_{};
    // Reverse destruction: runtime resources, callback registry, adapter,
    // then State. The caller keeps the GL session alive around this object.
    std::optional<project::State> state_;
    std::optional<project::ProjectControls> controls_;
    std::optional<editor::Inspector> inspector_;
    std::optional<project::Runtime> runtime_;
    std::map<vng::u64, PendingMetadata> pending_frames_; // At most three GPU transfers.
    std::optional<PendingImage> pending_image_; // Latest completed image awaiting publication.
    bool running_{true}, ready_{}, dirty_{}, diagnostic_{}, render_blocked_{}, fatal_error_{};
    bool playing_{}, linked_{true};
    std::optional<project::CameraPose> play_camera_;
    bool play_camera_override_{};
    project::NavigationTool navigation_;
    project::Settings settings_;
    project::ViewportInbox view_inbox_;
    std::optional<project::MeshVisibilityRequest> pending_visibility_;
    project::ZoomMotion zoom_;
    editor::InteractionTrace request_trace_, active_trace_, render_trace_;
    Clock::time_point view_clock_{Clock::now()};
    double preview_time_{}, play_time_{};
    vng::u64 frame_id_{};
    vng::u64 scene_encode_calls_{}, scene_decode_calls_{}, mesh_update_calls_{}, position_edits_{};
    vng::u64 selection_edits_{}, rotation_edits_{}, scale_edits_{};
    vng::u64 document_patches_{}, inspector_rebuilds_{}, schema_sends_{};
    vng::Extent2D preview_extent_{};
    Clock::time_point simulation_clock_{Clock::now()}, next_frame_{}, next_preview_{};
    std::string last_diagnostic_;
};

int run(preview::WorkerEndpoint& endpoint, int control_fd, vng::u64 generation) {
    auto session = example::GlfwOpenGLSession::create(
        {.width = 960,
         .height = 720,
         .title = "Vibe Engine — Play",
         .visible = false,
         .resizable = true},
        {.debug = true,
         .default_framebuffer_encoding = vng::render::ColorEncoding::linear},
        {.vsync = vng::window::VSync::off});
    if (!session) {
        const auto text =
            std::visit([](const auto& error) { return error.message; }, session.error());
        (void)endpoint.send("error\nCannot initialize preview OpenGL context: " + text);
        (void)endpoint.receive();
        std::cerr << text << '\n';
        return 1;
    }
    Worker worker{endpoint, session->device(), session->window(), generation};
    while (worker.running() && !endpoint.closed()) {
        auto packets = endpoint.receive();
        if (!packets)
            throw std::runtime_error(packets.error().message);
        worker.packets(*packets);
        if (!worker.running() || endpoint.closed())
            break;
        worker.window_events();
        worker.transfer();
        worker.update_playback();
        worker.render();
        worker.transfer();
        // send() queues only when socket backpressure requires it; receive()
        // flushes those writes even if there are no inbound messages.
        if (!worker.running())
            break;
        int wait_ms = 100;
        if (worker.wants_frame()) {
            const auto remaining = worker.next_frame() - Clock::now();
            wait_ms = std::clamp(
                static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count()),
                0, 34);
        }
        // Short socket waits let ready transfers surface promptly without busy
        // spinning or waiting for the GPU inside a GL call. Incoming input wakes
        // this wait immediately, including while a final transfer is pending.
        timespec timeout{wait_ms / 1000, (wait_ms % 1000) * 1'000'000L};
        if (worker.transfer_pending()) timeout = {0, 200'000};
        pollfd descriptor{control_fd, POLLIN, 0};
        if (::ppoll(&descriptor, 1, &timeout, nullptr) < 0 && errno != EINTR)
            throw std::runtime_error("Preview control socket poll failed");
    }
    (void)endpoint.receive(); // Flush final errors/readiness while the socket is still alive.
    return worker.exit_code();
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 5 || std::string_view(argv[1]) != "--worker") {
        std::cerr << "This preview worker is launched by vng_editor_demo, not run directly.\n";
        return 2;
    }
    const auto control = parse_integer<int>(argv[2]);
    const auto memory = parse_integer<int>(argv[3]);
    const auto generation = parse_integer<vng::u64>(argv[4]);
    if (!control || !memory || !generation || *control < 0 || *memory < 0 || *generation == 0) {
        std::cerr << "Invalid preview worker descriptors or generation.\n";
        return 2;
    }
    auto endpoint = preview::WorkerEndpoint::attach(*control, *memory, *generation);
    if (!endpoint) {
        std::cerr << endpoint.error().message << '\n';
        return 1;
    }
    try {
        return run(*endpoint, *control, *generation);
    } catch (const std::exception& error) {
        std::cerr << "Preview worker exception: " << error.what() << '\n';
        (void)endpoint->send("error\nPreview worker exception: " + std::string(error.what()));
        (void)endpoint->receive();
        return 1;
    } catch (...) {
        std::cerr << "Preview worker failed with a non-standard exception.\n";
        (void)endpoint->send("error\nPreview worker failed with a non-standard exception.");
        (void)endpoint->receive();
        return 1;
    }
}
