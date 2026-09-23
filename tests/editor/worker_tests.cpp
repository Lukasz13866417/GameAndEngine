#include "../../examples/editor/project.hpp"
#include "../../examples/editor/document_patch.hpp"
#include "../../examples/editor/edits.hpp"
#include "../../examples/editor/playback.hpp"
#include "../../examples/editor/position_edits.hpp"
#include "../../examples/editor/rotation_edits.hpp"
#include "../../examples/editor/scale_edits.hpp"
#include "../../examples/editor/selection_edits.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/preview_viewport.hpp"
#include "../../examples/editor/settings.hpp"
#include "../../examples/editor/viewport_session.hpp"

#include <vng/editor/inspector.hpp>
#include <vng/editor/preview.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace editor = vng::editor;
namespace preview = vng::editor::preview;
namespace project = editor_example;
using Clock = std::chrono::steady_clock;

struct Failure final : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ContextUnavailable final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void check(bool condition, std::string message) {
    if (!condition)
        throw Failure{std::move(message)};
}

template <class T, class Error> T take(std::expected<T, Error> result, std::string_view operation) {
    if (!result)
        throw Failure{std::string(operation) + ": " + result.error().message};
    if constexpr (!std::same_as<T, void>)
        return std::move(*result);
}

bool missing_context(std::string_view text) {
    // Only explicit environment/context failures may skip this test. Shader,
    // resource, protocol, assertion, and generic worker failures are failures.
    constexpr std::string_view markers[]{
        "GLFW initialization failed",
        "Failed to initialize GLFW",
        "glfwInit failed",
        "Failed to create a GLFW window",
        "X11: The DISPLAY environment variable is missing",
        "Wayland: Failed to connect to display",
        "GLX: Failed to create context",
        "EGL: Failed to create context",
        "OpenGL 4.6 core is required",
        "OpenGL 4.6 or newer is required",
        "OpenGL 4.6 was requested, but the context is",
        "The current OpenGL context is not a core-profile context",
        "GLAD failed to load OpenGL procedures for the current context"};
    return std::ranges::any_of(
        markers, [&](auto marker) { return text.find(marker) != std::string_view::npos; });
}

std::size_t bright_pixels_any_extent(const preview::PreviewFrame& frame) {
    check(frame.rgba.size() == static_cast<std::size_t>(frame.info.width) * frame.info.height * 4,
          "Worker returned incomplete RGBA pixels");
    std::size_t bright{};
    for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
        bright += std::to_integer<unsigned>(frame.rgba[i]) > 80 ||
                  std::to_integer<unsigned>(frame.rgba[i + 1]) > 80 ||
                  std::to_integer<unsigned>(frame.rgba[i + 2]) > 80;
        check(frame.rgba[i + 3] == std::byte{255}, "Worker presentation pixels should be opaque");
    }
    return bright;
}

std::size_t bright_pixels(const preview::PreviewFrame& frame) {
    check(frame.info.width == 640 && frame.info.height == 480,
          "Worker returned an unexpected viewport size");
    return bright_pixels_any_extent(frame);
}

bool equivalent_pixels(const std::vector<std::byte>& left, const std::vector<std::byte>& right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (std::abs(std::to_integer<int>(left[i]) - std::to_integer<int>(right[i])) > 1)
            return false;
    return true;
}

std::map<std::string, double> fields(std::string_view payload) {
    std::map<std::string, double> result;
    while (!payload.empty()) {
        const auto end = payload.find(';');
        const auto item = payload.substr(0, end);
        const auto equals = item.find('=');
        check(equals != std::string_view::npos, "Malformed worker status field");
        const auto text = item.substr(equals + 1);
        double number{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
              "Malformed worker numeric status");
        check(result.emplace(std::string(item.substr(0, equals)), number).second,
              "Duplicate worker status field");
        if (end == std::string_view::npos)
            break;
        payload.remove_prefix(end + 1);
    }
    return result;
}

struct Harness final {
    project::State authored;
    preview::PreviewSession session;
    std::optional<editor::Schema> schema;
    std::optional<project::State> acknowledgement;
    vng::u64 acknowledgements{}, stale_rejections{}, bound_rejections{}, schemas{}, revisions{},
        acknowledged_revision{};
    vng::u64 statuses{}, statistic_replies{}, resyncs{}, infos{};
    bool allow_stale{}, allow_bounds{}, allow_resync{}, ever_activated{}, playing{}, linked{true};
    vng::Extent2D viewport = project::default_preview_extent;
    std::map<std::string, double> stats;
    std::string info;

    Harness(project::State state, preview::PreviewSession preview)
        : authored(std::move(state)), session(std::move(preview)) {}

    void send_snapshot(vng::u64 generation = 0) {
        take(session.send(project::encode_viewport_size(viewport), generation),
             "Set worker viewport");
        auto payload = take(project::encode(authored), "Encode project snapshot");
        take(session.send("snapshot\n" + payload, generation), "Send project snapshot");
    }

    void pump() {
        for (const auto& event : session.poll()) {
            if (event.kind == preview::EventKind::candidate_started) {
                send_snapshot(event.generation);
            } else if (event.kind == preview::EventKind::activated) {
                ever_activated = true;
                std::cout << "Preview generation " << event.generation << " activated\n";
            } else if (event.kind == preview::EventKind::message) {
                const std::string_view packet = event.message;
                if (packet.starts_with("schema\n")) {
                    auto incoming =
                        take(editor::decode_schema(packet.substr(7)), "Decode worker inspector");
                    check(incoming.stamp.generation == event.generation,
                          "Inspector generation differs from its sending worker");
                    if (!schema || incoming.stamp.generation >= schema->stamp.generation)
                        schema = std::move(incoming);
                    ++schemas;
                } else if (packet.starts_with("state\n")) {
                    acknowledgement =
                        take(project::decode(packet.substr(6)), "Decode acknowledged worker state");
                    ++acknowledgements;
                } else if (packet.starts_with("state_patch\n")) {
                    auto patch = take(project::decode_patch(packet.substr(12)), "Decode acknowledged property patch");
                    acknowledgement = authored;
                    take(project::apply_patch(*acknowledgement, patch), "Apply acknowledged property patch");
                    ++acknowledgements;
                } else if (packet.starts_with("info\n")) {
                    info = packet.substr(5);
                    ++infos;
                } else if (packet.starts_with("revision\n")) {
                    const auto text = packet.substr(9);
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                                        acknowledged_revision);
                    check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
                          "Malformed revision acknowledgement");
                    ++revisions;
                } else if (packet.starts_with("status\n")) {
                    const auto status = fields(packet.substr(7));
                    playing = status.at("play") != 0;
                    linked = status.at("link") != 0;
                    ++statuses;
                } else if (packet.starts_with("stats\n")) {
                    stats = fields(packet.substr(6));
                    ++statistic_replies;
                } else if (packet.starts_with("resync\n")) {
                    check(allow_resync, "Unexpected resync: " + std::string(packet.substr(7)));
                    ++resyncs;
                } else if (packet.starts_with("error\n")) {
                    const auto error = packet.substr(6);
                    if (allow_stale &&
                        error.find("Stale editor event:") != std::string_view::npos) {
                        ++stale_rejections;
                    } else if (allow_bounds &&
                               (error == "Scene settings exceed editor limits" ||
                                error == "Scene instance settings exceed editor limits" ||
                                error == "Scene instance transform exceeds editor limits" ||
                                error == "Position values must be finite and within +/-" +
                                    std::to_string(static_cast<int>(project::scene_coordinate_limit)))) {
                        ++bound_rejections;
                    } else if (!ever_activated && missing_context(error)) {
                        throw ContextUnavailable{std::string(error)};
                    } else
                        throw Failure{"Worker error: " + std::string(error)};
                } else
                    throw Failure{"Unknown worker message envelope"};
            } else if (event.kind == preview::EventKind::worker_failed ||
                       event.kind == preview::EventKind::build_failed) {
                if (!ever_activated && missing_context(session.logs()))
                    throw ContextUnavailable{std::string(session.logs())};
                throw Failure{event.message + "\n" + std::string(session.logs())};
            }
        }
    }

    template <class Predicate> void until(std::string_view stage, Predicate done) {
        const auto deadline = Clock::now() + std::chrono::seconds{120};
        while (Clock::now() < deadline) {
            pump();
            if (done())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        throw Failure{"Timed out waiting for " + std::string(stage) + ": " +
                      std::string(session.status()) + "\n" + std::string(session.logs())};
    }

    // After "play\n0": wait until Play has stopped, then for a frame newer than
    // any seen by then, so a Play frame still in flight is never mistaken for
    // the restored editor view.
    void until_stopped(vng::u64 last_play_frame) {
        until("Play stopped", [&] { return !playing && session.latest_frame()->info.frame_id >= last_play_frame; });
        const auto stopped = session.latest_frame()->info.frame_id;
        until("first frame after Play stopped", [&] {
            return matching_frame(authored.document.revision) && session.latest_frame()->info.frame_id > stopped;
        });
    }
    [[nodiscard]] bool matching_frame(vng::u64 revision, vng::u64 generation = 0) const {
        if (!generation)
            generation = session.active_generation();
        return generation != 0 && session.latest_frame() &&
               session.latest_frame()->info.generation == generation &&
               session.latest_frame()->info.scene_revision == revision;
    }

    std::map<std::string, double> query_stats() {
        const auto before = statistic_replies;
        take(session.send("stats"), "Request worker counters");
        until("worker counter reply", [&] { return statistic_replies > before; });
        return stats;
    }

    template <class Predicate> std::map<std::string, double> until_stats(Predicate done) {
        const auto deadline = Clock::now() + std::chrono::seconds{10};
        while (Clock::now() < deadline) {
            auto current = query_stats();
            if (done(current))
                return current;
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        throw Failure{"Native Play counters did not progress"};
    }
};

void inspect_time(Harness& h, vng::f32 time) {
    h.authored.viewport.time = time;
    ++h.authored.viewport.sequence;
    const auto view = project::encode_viewport_request({h.authored.document.revision, h.authored.viewport});
    check(bool(view), "Encode pose inspection");
    take(h.session.send_latest("view\n" + *view), "Inspect pose");
    h.until("pose inspection frame", [&] {
        const auto& frame = h.session.latest_frame();
        return frame && frame->info.view_sequence == h.authored.viewport.sequence;
    });
}

void large_timeline_test(Harness& h) {
    project::State next{.document = {.revision = h.authored.document.revision + 1,
                                   .mesh = h.authored.document.mesh}};
    next.viewport.sequence = h.authored.viewport.sequence + 1;
    project::sun_settings(next, 2)->visible = false;
    next.viewport.selected_object = 1;
    next.viewport.time = 0;
    for (unsigned i = 1; i < 65; ++i)
        take(project::instantiate(next, project::BlueprintId::mesh), "Add animated copy");
    for (const auto& instance : next.document.instances) {
        if (!project::is_mesh_instance(next, instance.id)) continue;
        for (const auto property : {"position", "rotation", "scale", "brightness"}) {
            vng::timeline::Value value = instance.transform.position;
            if (property == std::string_view{"rotation"}) value = instance.transform.rotation;
            if (property == std::string_view{"scale"}) value = instance.transform.scale;
            if (property == std::string_view{"brightness"}) value = 1.F;
            take(project::key_property(next, {instance.id,property}, 0, value), "Key animated copy");
        }
    }
    check(next.document.timeline.tracks().size() == 260, "Large timeline fixture must cross the old cap");
    project::Settings settings;
    settings.timeline_track_limit = 1024;
    take(h.session.send("settings\n" + project::encode_settings(settings)), "Apply large track preference");
    h.authored = std::move(next);
    h.send_snapshot();
    h.until("preview with more than 256 tracks", [&] { return h.matching_frame(h.authored.document.revision); });
    check(bright_pixels_any_extent(*h.session.latest_frame()) > 100, "Large timeline did not render");
    // The authoring budget is not a playback/transport gate. Lowering it must
    // not invalidate existing scenes or a future restored history snapshot.
    settings.timeline_track_limit = 1;
    take(h.session.send("settings\n" + project::encode_settings(settings)), "Lower authoring preference");
    ++h.authored.document.revision;
    h.send_snapshot();
    h.until("existing tracks survive a smaller preference", [&] { return h.matching_frame(h.authored.document.revision); });
    const auto base = h.authored.document.revision;
    take(project::key_property(h.authored, {1,"brightness"}, 0, 0.7F), "Edit existing track above budget");
    ++h.authored.document.revision;
    project::DocumentChanges changes; changes.properties.insert({1,"brightness"});
    const auto patch = take(project::capture_patch(base, h.authored, changes), "Capture large timeline edit");
    take(h.session.send("patch\n" + take(project::encode_patch(patch), "Encode large timeline edit")), "Send large timeline edit");
    h.until("compact edit above old track cap", [&] { return h.matching_frame(h.authored.document.revision); });
    std::cout << "Track-limit integration passed: 260-track snapshots, lower preferences and compact edits.\n";
}

void fps_settings_test(Harness& h) {
    project::Settings settings{.ui_fps = 30, .preview_fps = 1, .play_fps = 1, .debug_fps = 1};
    take(h.session.send("settings\n" + project::encode_settings(settings)), "Apply worker frame caps");
    const auto initial = h.query_stats();
    check(initial.at("preview_fps") == 1 && initial.at("play_fps") == 1 && initial.at("debug_fps") == 1,
          "Worker did not apply independent frame caps");
    const project::PlaybackEdit play{h.authored.document.revision, h.authored.document.revision + 1, 0, false};
    take(project::apply_playback_edit(h.authored, play), "Begin slow-frame preview playback");
    take(h.session.send("playback\n" + take(project::encode_playback_edit(play), "Encode playback")), "Play slow preview");
    const auto preview = h.until_stats([](const auto& stats) { return stats.at("time") > .65; });
    check(preview.at("rendered_frames") - initial.at("rendered_frames") <= 2,
          "Embedded preview exceeded its one-frame-per-second cap");
    check(preview.at("time") < 1.5, "Slow render cap delayed playback/control handling");
    take(h.session.send("link\n0"), "Disable debug during capped native Play");
    h.until("debug disconnect", [&] { return !h.linked; });
    take(h.session.send("play\n1"), "Start capped independent Play");
    h.until("capped native Play", [&] { return h.playing; });
    const auto native = h.query_stats();
    check(initial.at("vsync")==0 && native.at("vsync")==1,
          "VSync must apply to independent Play, not the hidden preview");
    const auto revision=native.at("revision"), uploads=native.at("full_mesh_uploads");
    settings.vsync=vng::window::VSync::off;
    take(h.session.send("settings\n" + project::encode_settings(settings)), "Disable native Play VSync");
    const auto unsynchronized=h.query_stats();
    check(unsynchronized.at("vsync")==0 && unsynchronized.at("revision")==revision &&
          unsynchronized.at("full_mesh_uploads")==uploads,
          "Live VSync changes must not edit the scene or recreate geometry");
    settings.vsync=vng::window::VSync::on;
    take(h.session.send("settings\n" + project::encode_settings(settings)), "Enable native Play VSync");
    check(h.query_stats().at("vsync")==1,"Live Play VSync did not apply");
    const auto advanced = h.until_stats([&](const auto& stats) {
        return stats.at("time") > native.at("time") + .65;
    });
    check(advanced.at("presented_frames") - native.at("presented_frames") <= 2,
          "Native Play ignored its cap while debugging was disabled");
    check(advanced.at("readbacks") == native.at("readbacks"), "Disconnected capped Play read back pixels");
    check(advanced.at("full_mesh_uploads") == initial.at("full_mesh_uploads"), "Settings reuploaded mesh geometry");
    take(h.session.send("play\n0"), "Stop capped Play");
    h.until("capped Play stopped", [&] { return !h.playing; });
    check(h.query_stats().at("vsync")==0,"Stopping Play left the hidden worker synchronized to display");
    for (const auto rate : {144U, 0U}) {
        settings.debug_fps = rate;
        take(h.session.send("settings\n" + project::encode_settings(settings)),
             "Set unrestricted debug-preview FPS");
        check(h.query_stats().at("debug_fps") == rate,
              "Worker rejected or clamped unrestricted debug-preview FPS");
    }
    take(h.session.send("settings\n" + project::encode_settings(project::Settings{})), "Restore worker caps");
    take(h.session.send("link\n1"), "Restore debug link");
    h.authored.viewport.paused = true;
    ++h.authored.document.revision;
    h.send_snapshot();
    h.until("restored preview after settings test", [&] { return h.matching_frame(h.authored.document.revision); });
    std::cout << "Settings passed: independent preview/Play caps, responsive clock at 1 FPS, no extra uploads/readbacks.\n";
}

void native_play_test(Harness& h) {
    const auto before_patch = h.query_stats();
    const auto selected = h.authored.viewport.selected_vertex;
    auto point = h.authored.document.mesh.position(selected);
    point.x += 0.02F;
    const project::VertexEdit edit{
        h.authored.document.revision, h.authored.document.revision + 1, {{selected, point}}};
    take(project::apply_edit(h.authored, edit), "Apply local compact vertex patch");
    const auto encoded = take(project::encode_edit(edit), "Encode compact vertex patch");
    take(h.session.send("vertices\n" + encoded), "Send compact vertex patch");
    h.until("compact patch acknowledgement and completed frame", [&] {
        return h.acknowledged_revision == h.authored.document.revision &&
               h.matching_frame(h.authored.document.revision) && h.schema &&
               h.schema->stamp.revision == h.authored.document.revision;
    });
    const auto after_patch = h.query_stats();
    check(after_patch.at("full_mesh_uploads") == before_patch.at("full_mesh_uploads"),
          "A vertex-only patch recreated the GPU mesh");
    check(after_patch.at("vertex_updates") > before_patch.at("vertex_updates"),
          "A vertex-only patch did not update the retained vertex allocation");
    h.allow_resync = true;
    take(h.session.send("vertices\n" + encoded), "Replay stale compact patch");
    h.until("compact patch base-revision rejection", [&] { return h.resyncs == 1; });
    h.allow_resync = false;
    check(h.query_stats().at("revision") == static_cast<double>(h.authored.document.revision),
          "A stale compact patch changed worker revision");

    const auto status_before = h.statuses;
    take(h.session.send("link\n0"), "Disconnect editor debug bridge");
    h.until("disconnected debug status", [&] { return h.statuses > status_before && !h.linked; });
    const auto idle = h.query_stats();
    const auto retained_id = h.session.latest_frame()->info.frame_id;
    const auto retained_pixels = h.session.latest_frame()->rgba;
    const auto schemas = h.schemas, acknowledgements = h.acknowledgements, infos = h.infos,
               revisions = h.revisions;
    std::this_thread::sleep_for(std::chrono::milliseconds{120});
    const auto still_idle = h.query_stats();
    check(still_idle.at("rendered_frames") == idle.at("rendered_frames") &&
              still_idle.at("readbacks") == idle.at("readbacks"),
          "Disconnected paused editor preview was not idle");

    take(h.session.send("play\n1"), "Start worker-owned native Play");
    h.until("native Play visible status", [&] { return h.playing && !h.linked; });
    const auto native = h.until_stats([&](const auto& stats) {
        return stats.at("presented_frames") >= idle.at("presented_frames") + 3 &&
               stats.at("time") > static_cast<double>(h.authored.viewport.time) + 0.03 &&
               stats.at("visible") == 1;
    });
    check(native.at("visible") == 1, "Play did not show the worker-owned native window");
    check(native.at("readbacks") == idle.at("readbacks"),
          "Disconnected native Play performed CPU readbacks");
    check(h.session.latest_frame()->info.frame_id == retained_id &&
              h.session.latest_frame()->rgba == retained_pixels,
          "Disconnected native Play still published editor images");

    // Ordinary edits are deliberately ignored while disconnected. Launch is
    // an explicit one-shot transfer, not a hidden re-enabling of debug IPC.
    (*editor_example::mesh_settings(h.authored, 1)).brightness = 0.7F;
    ++h.authored.document.revision;
    h.send_snapshot();
    const auto ignored = h.query_stats();
    check(ignored.at("revision") == static_cast<double>(h.authored.document.revision - 1),
          "Disconnected native Play applied an ordinary editor snapshot");
    take(h.session.send("close"), "Request native-window close through its regular event path");
    h.until("native close returns to editor mode", [&] { return !h.playing && !h.linked; });
    const auto closed = h.query_stats();
    check(closed.at("visible") == 0, "Native close did not hide the retained window");
    check(closed.at("readbacks") == idle.at("readbacks"),
          "Closing disconnected Play captured a preview image");

    take(h.session.send("launch\n" +
                        take(project::encode(h.authored), "Encode offline launch scene")),
         "Launch current offline-authored scene without debug streaming");
    h.until("offline one-shot launch", [&] { return h.playing && !h.linked; });
    const auto launched = h.until_stats([&](const auto& stats) {
        return stats.at("revision") == static_cast<double>(h.authored.document.revision) &&
               stats.at("presented_frames") >= closed.at("presented_frames") + 3 &&
               stats.at("time") > static_cast<double>(h.authored.viewport.time) + 0.05 &&
               stats.at("visible") == 1;
    });
    check(launched.at("visible") == 1 && launched.at("readbacks") == idle.at("readbacks"),
          "Offline launch failed to reuse/show the window or performed readback");
    check(h.schemas == schemas && h.acknowledgements == acknowledgements && h.infos == infos &&
              h.revisions == revisions,
          "Disconnected mode sent inspector, state, revision, or diagnostic traffic");
    check(h.session.latest_frame()->info.frame_id == retained_id,
          "Offline launch published a debug frame");

    take(h.session.send("link\n1"), "Reconnect debug bridge during native Play");
    h.send_snapshot(); // Authored time remains unchanged: this must not rewind Play.
    h.until("reconnected native preview with current scene", [&] {
        return h.playing && h.linked && h.matching_frame(h.authored.document.revision) &&
               h.session.latest_frame()->info.frame_id > retained_id;
    });
    check(h.session.latest_frame()->info.time >= launched.at("time") - 0.02,
          "Reconnecting the editor rewound the private native Play clock");
    const auto linked = h.query_stats();
    check(linked.at("readbacks") > launched.at("readbacks") && linked.at("preview_rescales") > 0,
          "Linked native Play did not copy a scaled preview from the native-sized GPU frame");
    const auto observed_at = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{260});
    const auto later = h.query_stats();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - observed_at).count();
    check(later.at("presented_frames") > linked.at("presented_frames"),
          "Native rendering stopped while linked");
    check(later.at("readbacks") - linked.at("readbacks") <= std::ceil(elapsed * 10.0) + 2,
          "Linked native Play captured previews at native rendering frequency");

    const auto native_last = h.session.latest_frame()->info.frame_id;
    take(h.session.send("play\n0"), "Stop native Play while keeping the debug bridge");
    h.until("paused authored editor preview after Stop", [&] {
        return !h.playing && h.linked && h.matching_frame(h.authored.document.revision) &&
               h.session.latest_frame()->info.frame_id > native_last &&
               h.session.latest_frame()->info.time == static_cast<double>(h.authored.viewport.time);
    });
    const auto stopped = h.query_stats();
    check(stopped.at("visible") == 0, "Stop destroyed or failed to hide the Play window");
    std::this_thread::sleep_for(std::chrono::milliseconds{120});
    const auto settled = h.query_stats();
    check(settled.at("presented_frames") == stopped.at("presented_frames") &&
              settled.at("readbacks") == stopped.at("readbacks"),
          "Stopped paused preview kept rendering or reading back");
    check(h.acknowledgements == acknowledgements,
          "Native Play silently authored simulation-time changes");
    std::cout << "Native Play passed: GPU-only disconnected rendering, compact patches, offline "
                 "launch, reconnect, close/reopen, and unchanged authored time.\n";
}

void camera_and_selection_test(Harness& h) {
    const auto before = h.query_stats();
    const auto pixels = h.session.latest_frame()->rgba;
    // The editor camera is private view state on the latest-value lane: no
    // authored revision, acknowledgement, or document/mesh work.
    h.authored.viewport.editor_camera = {35.F, 22.F, 6.F, {.3F, .15F, 0}};
    ++h.authored.viewport.sequence;
    const auto view = project::encode_viewport_request({h.authored.document.revision, h.authored.viewport});
    check(bool(view), "Encode editor camera view");
    take(h.session.send_latest("view\n" + *view), "Send editor camera view");
    h.until("editor camera view and changed pixels", [&] {
        const auto& frame = h.session.latest_frame();
        return frame && frame->info.view_sequence == h.authored.viewport.sequence && frame->info.settled;
    });
    check(!equivalent_pixels(h.session.latest_frame()->rgba, pixels),
          "Editor camera change did not change the rendered view");
    const auto after = h.query_stats();
    check(after.at("full_mesh_uploads") == before.at("full_mesh_uploads") &&
              after.at("vertex_updates") == before.at("vertex_updates") &&
              after.at("vertex_bytes_uploaded") == before.at("vertex_bytes_uploaded") &&
              after.at("revision") == before.at("revision"),
          "Camera navigation touched GPU mesh storage or the authored revision");

    const auto camera_pixels = h.session.latest_frame()->rgba;
    for (vng::u32 selected : {0U, 2U, 1U}) {
        h.authored.viewport.selected_object = selected;
        h.authored.viewport.selected_vertex = 0;
        ++h.authored.document.revision;
        h.send_snapshot();
        h.until("scene selection and its inspector", [&] {
            return h.schema && h.schema->stamp.object == selected &&
                   h.schema->stamp.revision == h.authored.document.revision &&
                   h.matching_frame(h.authored.document.revision);
        });
        check(h.schema->controls.empty() == (selected == 0),
              "Empty selection exposed an object's controls");
        check(equivalent_pixels(camera_pixels, h.session.latest_frame()->rgba),
              "Selection alone changed the production scene or camera");
    }
    std::cout << "Camera/selection passed: private view lane, unchanged GPU storage and revision, "
                 "empty selection and object inspectors.\n";
}

void scene_camera_test(Harness& h) {
    auto original = h.authored;
    h.authored.viewport.mode = project::ViewMode::scene;
    (*editor_example::sun_settings(h.authored, 2)).visible = false;
    (*editor_example::mesh_settings(h.authored, 1)).visible = true;
    (*editor_example::instance_transform(h.authored, 1)).position = {};
    h.authored.viewport.paused = true;
    h.authored.viewport.time = 0;
    h.authored.document.timeline = {};
    h.authored.document.keyframe_names.clear();
    // Without a scene camera, Play holds the editor's view from when it started.
    h.authored.viewport.editor_camera = {0, 0, 6, {}};
    ++h.authored.document.revision;
    h.send_snapshot();
    h.until("camera-less scene", [&] { return h.matching_frame(h.authored.document.revision); });
    const auto still_frame = h.session.latest_frame()->info.frame_id;
    take(h.session.send("play\n1"), "Play a scene without a camera");
    h.until("camera-less Play frame", [&] {
        return h.playing && h.matching_frame(h.authored.document.revision) &&
               h.session.latest_frame()->info.frame_id > still_frame;
    });
    check(bright_pixels_any_extent(*h.session.latest_frame()) > 100,
          "Camera-less Play did not start from the editor view");
    h.authored.viewport.editor_camera = {0, 0, 6, {40, 0, 0}};
    ++h.authored.viewport.sequence;
    const auto away = project::encode_viewport_request({h.authored.document.revision, h.authored.viewport});
    check(bool(away), "Encode editor navigation during Play");
    take(h.session.send_latest("view\n" + *away), "Navigate the editor view during Play");
    h.until("Play frame after editor navigation", [&] {
        const auto& frame = h.session.latest_frame();
        return frame && h.playing && frame->info.view_sequence == h.authored.viewport.sequence;
    });
    check(bright_pixels_any_extent(*h.session.latest_frame()) > 100,
          "Camera-less Play followed the editor's live navigation");
    const auto held_frame = h.session.latest_frame()->info.frame_id;
    take(h.session.send("play\n0"), "Stop camera-less Play");
    h.until_stopped(held_frame);

    // The editor view sees the mesh; the scene camera deliberately looks away.
    h.authored.viewport.editor_camera = {0, 0, 6, {}};
    ++h.authored.viewport.sequence;
    const auto camera = take(project::ensure_camera(h.authored, {0, 0, 6, {40, 0, 0}}), "Add a scene camera");
    ++h.authored.document.revision;
    h.send_snapshot();
    h.until("separate editor and scene cameras", [&] {
        return h.matching_frame(h.authored.document.revision);
    });
    check(bright_pixels(*h.session.latest_frame()) > 100,
          "The embedded preview did not look through the editor camera");
    const auto uploads = h.query_stats();
    const auto inspection_frame = h.session.latest_frame()->info.frame_id;
    take(h.session.send("play\n1"), "Start independent Play through the scene camera");
    h.until("independent Play scene-camera frame", [&] {
        return h.playing && h.matching_frame(h.authored.document.revision) &&
               h.session.latest_frame()->info.frame_id > inspection_frame;
    });
    check(bright_pixels_any_extent(*h.session.latest_frame()) == 0,
          "Independent Play used the editor camera instead of the scene camera");

    // Moving the camera instance is an ordinary authored patch, and Play follows it.
    const auto base_revision = h.authored.document.revision;
    project::place_camera(*project::find_instance(h.authored, camera), {-30, 10, 6, {}});
    ++h.authored.document.revision;
    project::DocumentChanges moved;
    for (const auto* property : {"position", "rotation", "zoom", "focus"}) moved.properties.insert({camera, property});
    const auto patch = take(project::capture_patch(base_revision, h.authored, moved), "Capture camera move");
    take(h.session.send("patch\n" + take(project::encode_patch(patch), "Encode camera move")), "Send camera move");
    h.until("camera move acknowledged during Play", [&] {
        return h.acknowledged_revision == h.authored.document.revision && h.matching_frame(h.authored.document.revision);
    });
    check(bright_pixels_any_extent(*h.session.latest_frame()) > 100,
          "Moving the scene camera did not update independent Play");

    // Changing only the editor camera must not steer Play.
    h.authored.viewport.editor_camera = {0, 0, 6, {40, 0, 0}};
    ++h.authored.document.revision;
    h.send_snapshot();
    h.until("editor-camera-only snapshot during independent Play", [&] {
        return h.matching_frame(h.authored.document.revision);
    });
    check(bright_pixels_any_extent(*h.session.latest_frame()) > 100,
          "An editor camera change replaced the scene camera in independent Play");

    const auto play_frame = h.session.latest_frame()->info.frame_id;
    take(h.session.send("play\n0"), "Stop independent Play and restore the editor view");
    h.until_stopped(play_frame);
    check(bright_pixels(*h.session.latest_frame()) == 0,
          "Stopping independent Play did not restore the editor camera");
    const auto after = h.query_stats();
    check(after.at("full_mesh_uploads") == uploads.at("full_mesh_uploads") &&
              after.at("vertex_updates") == uploads.at("vertex_updates") &&
              after.at("vertex_bytes_uploaded") == uploads.at("vertex_bytes_uploaded"),
          "Camera edits and navigation unnecessarily uploaded mesh geometry");

    // An actual camera timeline must drive independent Play; a static pose is
    // not sufficient. The instance itself looks at the mesh, so only the keyed
    // cut at one second can turn it away.
    h.authored.viewport.editor_camera = {0, 0, 6, {}};
    take(project::key_camera(h.authored, camera, 0, {0, 0, 6, {}}), "Key opening camera shot");
    take(project::key_camera(h.authored, camera, 1, {0, 0, 6, {40, 0, 0}}, vng::timeline::Interpolation::hold),
         "Key held camera cut");
    ++h.authored.document.revision;
    h.send_snapshot();
    h.until("camera timeline snapshot", [&] {
        return h.matching_frame(h.authored.document.revision);
    });
    take(h.session.send("play\n1"), "Play camera timeline independently");
    h.until("independent playback reaches held camera cut", [&] {
        return h.playing && h.matching_frame(h.authored.document.revision) &&
               h.session.latest_frame()->info.time >= 1.1;
    });
    check(bright_pixels_any_extent(*h.session.latest_frame()) == 0,
          "Independent playback did not evaluate its camera timeline cut");
    const auto final_play_frame = h.session.latest_frame()->info.frame_id;
    take(h.session.send("play\n0"), "Stop camera timeline playback");
    h.until_stopped(final_play_frame);
    check(bright_pixels(*h.session.latest_frame()) > 100,
          "Stopping independent playback did not restore the editor view");

    original.document.revision = h.authored.document.revision + 1;
    h.authored = std::move(original);
    h.send_snapshot();
    h.until("restored scene after scene-camera regression", [&] {
        return h.matching_frame(h.authored.document.revision);
    });
    std::cout << "Scene camera passed: Play looks through the camera instance, follows authored moves "
                 "and keyed cuts, and ignores editor navigation.\n";
}

void timeline_test(Harness& h) {
    (*editor_example::sun_settings(h.authored, 2)).visible = false;
    h.authored.viewport.mode = project::ViewMode::scene;
    h.authored.viewport.time = 0;
    h.authored.viewport.paused = true;
    (*editor_example::instance_transform(h.authored, 1)).position = {};
    h.authored.viewport.editor_camera.yaw = h.authored.viewport.editor_camera.pitch = 0;
    h.authored.viewport.editor_camera.target = {};
    take(project::key_property(h.authored, {1, "position"}, 4, vng::Vec3{1.5F, 0, 0}),
         "Key animated mesh position");
    take(project::key_property(h.authored, {1, "visible"}, 4, false), "Key held visibility");
    ++h.authored.document.revision;
    h.send_snapshot();
    h.until("timeline snapshot frame", [&] { return h.matching_frame(h.authored.document.revision); });
    const auto initial_pixels = h.session.latest_frame()->rgba;
    const auto uploads = h.query_stats();
    std::string final_packet;
    for (const auto time : {2.0F, 3.99F, 4.0F, 0.0F}) {
        const project::PlaybackEdit edit{h.authored.document.revision, h.authored.document.revision + 1, time, true};
        take(project::apply_playback_edit(h.authored, edit), "Apply authored timeline scrub");
        final_packet = take(project::encode_playback_edit(edit), "Encode timeline scrub");
        take(h.session.send("playback\n" + final_packet), "Send timeline scrub");
        h.until("scrub revision/frame/inspector", [&] {
            return h.acknowledged_revision == h.authored.document.revision &&
                   h.matching_frame(h.authored.document.revision) && h.schema &&
                   h.schema->stamp.revision == h.authored.document.revision;
        });
        const auto& frame = *h.session.latest_frame();
        check(frame.info.time == static_cast<double>(time),
              "Scrub frame time differs from request");
        if (time == 4)
            check(bright_pixels(frame) == 0,
                  "Held visibility key did not switch off at its timestamp");
        else
            check(bright_pixels(frame) > 100,
                  "Held visibility switched off before the arriving key");
        if (time == 0)
            check(equivalent_pixels(frame.rgba, initial_pixels),
                  "Scrubbing back did not restore pixels");
        else
            check(!equivalent_pixels(frame.rgba, initial_pixels),
                  "Timeline key did not affect actual draw");
    }
    const auto after = h.query_stats();
    check(after.at("full_mesh_uploads") == uploads.at("full_mesh_uploads") &&
              after.at("vertex_updates") == uploads.at("vertex_updates") &&
              after.at("vertex_bytes_uploaded") == uploads.at("vertex_bytes_uploaded"),
          "Timeline scrubbing uploaded geometry");
    const auto resyncs = h.resyncs;
    h.allow_resync = true;
    take(h.session.send("playback\n" + final_packet), "Replay stale timeline scrub");
    h.until("stale scrub rejection", [&] { return h.resyncs > resyncs; });
    h.allow_resync = false;
    const auto pixels = h.session.latest_frame()->rgba;
    const auto generation = h.session.active_generation();
    take(h.session.request_reload(), "Reload worker with authored keyframes");
    h.until("replacement worker timeline", [&] {
        return h.session.active_generation() > generation && h.matching_frame(h.authored.document.revision);
    });
    check(equivalent_pixels(pixels, h.session.latest_frame()->rgba),
          "Worker reload lost keyframes or changed evaluated rendering");
    std::cout << "Timeline passed: compact scrub, interpolated position, arriving-key visibility, "
                 "unchanged GPU buffers, stale rejection, and reload.\n";
}

void viewport_resolution_test(Harness& h) {
    const auto before = h.query_stats();
    const auto original_revision = h.authored.document.revision;
    const auto original_generation = h.session.active_generation();
    const auto original_pixels = h.session.latest_frame()->rgba;
    for (const auto extent : {vng::Extent2D{1280, 960}, vng::Extent2D{1500, 1125},
                              vng::Extent2D{700, 900}, vng::Extent2D{640, 480}}) {
        h.viewport = extent;
        const auto old_frame = h.session.latest_frame()->info.frame_id;
        take(h.session.send(project::encode_viewport_size(extent)), "Resize preview");
        h.until("matching native-resolution frame", [&] {
            const auto& frame = h.session.latest_frame();
            return h.matching_frame(original_revision, original_generation) && frame &&
                   frame->info.frame_id > old_frame && frame->info.width == extent.width &&
                   frame->info.height == extent.height;
        });
        check(h.session.latest_frame()->rgba.size() ==
                  std::size_t(extent.width) * extent.height * 4,
              "Resized preview has incomplete pixels");
        const auto after = h.query_stats();
        check(after.at("preview_width") == extent.width &&
                  after.at("preview_height") == extent.height,
              "Worker did not accept requested physical viewport size");
        check(after.at("full_mesh_uploads") == before.at("full_mesh_uploads") &&
                  after.at("vertex_updates") == before.at("vertex_updates"),
              "Viewport resizing rebuilt or uploaded unchanged mesh geometry");
        check(after.at("revision") == static_cast<double>(original_revision),
              "Resizing modified scene revision");
    }
    check(equivalent_pixels(h.session.latest_frame()->rgba, original_pixels),
          "Resizing back changed the paused scene");
    std::cout
        << "Preview resized to 1500x1125 and portrait dimensions without re-uploading geometry.\n";
}

void imported_blueprint_test(Harness& h) {
    struct TemporaryDirectory {
        std::filesystem::path path;
        TemporaryDirectory() {
            char name[] = "/tmp/vng-worker-import-XXXXXX";
            const auto* created = ::mkdtemp(name);
            check(created != nullptr, "Cannot create imported-mesh test directory");
            path = created;
        }
        ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } files;
    auto initial = project::State{.document = {.revision = h.authored.document.revision + 1, .mesh = h.authored.document.mesh}};
    initial.viewport.sequence = h.authored.viewport.sequence + 1; // Newer than any view request already sent.
    initial.viewport.selected_object = 1;
    initial.viewport.editor_camera = {.yaw = 0, .pitch = 0, .distance = 6};
    project::instance_transform(initial, 1)->position = {-1.2F, 0, 0};
    project::sun_settings(initial, 2)->visible = false;
    h.authored = std::move(initial);
    h.viewport = {640, 480};
    h.send_snapshot();
    h.until("cube-only baseline before mesh import", [&] { return h.matching_frame(h.authored.document.revision); });
    const auto original_geometry = h.authored.document.mesh.document();
    const auto original_pixels = h.session.latest_frame()->rgba;
    const auto baseline = h.query_stats();
    const auto same_left = [](const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
        if (a.size() != 640 * 480 * 4 || b.size() != a.size()) return false;
        for (std::size_t y = 0; y < 480; ++y) {
            const auto row = static_cast<std::ptrdiff_t>(y * 640 * 4);
            if (!std::equal(a.begin() + row, a.begin() + row + 320 * 4, b.begin() + row)) return false;
        }
        return true;
    };
    vng::content::vmesh::Document triangle;
    triangle.vertex_count = 3;
    triangle.metadata["name"] = "Worker imported RGB triangle";
    triangle.vertex_fields = {
        {"position", {vng::content::vmesh::ScalarType::Float32, 3},
         std::vector<vng::f32>{-.8F, -.7F, 0, .8F, -.7F, 0, 0, .8F, 0}},
        {"color/0", {vng::content::vmesh::ScalarType::Float32, 3},
         std::vector<vng::f32>{0, 1, .1F, 0, 1, .1F, 0, 1, .1F}}};
    triangle.faces = {{0, 1, 2}};
    const auto path = files.path / "triangle.vmesh";
    take(vng::content::vmesh::write_vmesh(path, triangle), "Write import fixture");
    const auto instance = take(project::import_mesh(h.authored, path), "Import reusable triangle blueprint");
    const auto blueprint = project::find_instance(h.authored, instance)->blueprint;
    check(blueprint != project::BlueprintId::mesh, "Import reused the built-in blueprint identity");
    project::instance_transform(h.authored, instance)->position = {1.2F, 0, 0};
    project::instance_transform(h.authored, instance)->scale = .7F;
    ++h.authored.document.revision;
    h.send_snapshot();
    h.until("worker imported second mesh asset and inspector", [&] {
        return h.matching_frame(h.authored.document.revision) && h.schema &&
               h.schema->stamp.object == instance && h.schema->stamp.revision == h.authored.document.revision;
    });
    const auto both = h.session.latest_frame()->rgba;
    check(both != original_pixels, "Imported geometry did not affect the worker image");
    check(same_left(both, original_pixels), "Import replaced or changed the existing cube");
    check(h.authored.document.mesh.document() == original_geometry, "Import changed the built-in authored mesh");
    const auto loaded = h.query_stats();
    check(loaded.at("full_mesh_uploads") == baseline.at("full_mesh_uploads") + 1,
          "Import must create exactly one additional GPU geometry allocation");
    const project::VertexEdit edit{h.authored.document.revision, h.authored.document.revision + 1,
                                   {{2, {0, .15F, 0}}}, static_cast<vng::u32>(blueprint)};
    take(project::apply_edit(h.authored, edit), "Apply imported asset vertex patch locally");
    take(h.session.send("vertices\n" + take(project::encode_edit(edit), "Encode asset-tagged patch")),
         "Patch imported worker geometry");
    h.until("imported asset patch acknowledged and rendered", [&] {
        return h.acknowledged_revision == h.authored.document.revision && h.matching_frame(h.authored.document.revision) &&
               h.schema && h.schema->stamp.revision == h.authored.document.revision;
    });
    const auto edited_pixels = h.session.latest_frame()->rgba;
    check(edited_pixels != both, "Imported vertex patch did not alter the triangle");
    check(same_left(edited_pixels, both), "Imported vertex patch was routed to the built-in cube");
    check(h.authored.document.mesh.document() == original_geometry, "Imported patch changed CPU cube geometry");
    const auto updated = h.query_stats();
    check(updated.at("full_mesh_uploads") == loaded.at("full_mesh_uploads"),
          "Imported vertex patch recreated a mesh allocation");
    check(updated.at("vertex_updates") == loaded.at("vertex_updates") + 1,
          "Imported vertex patch did not make exactly one bounded GPU update");
    check(updated.at("vertex_bytes_uploaded") == loaded.at("vertex_bytes_uploaded") + 16,
          "One imported vertex uploaded more than its typed GPU record");
    // Index 3 exists in the retained cube, but not this triangle. This catches
    // worker-side validation/rollback still consulting State.mesh accidentally.
    h.allow_resync = true;
    const auto resyncs = h.resyncs;
    const project::VertexEdit invalid{h.authored.document.revision, h.authored.document.revision + 1,
                                      {{3, {0, 0, 0}}}, static_cast<vng::u32>(blueprint)};
    take(h.session.send("vertices\n" + take(project::encode_edit(invalid), "Encode out-of-asset patch")),
         "Send imported patch with an index outside that asset");
    h.until("import-specific vertex bounds rejection", [&] { return h.resyncs > resyncs; });
    h.allow_resync = false;
    check(h.query_stats().at("revision") == static_cast<double>(h.authored.document.revision),
          "Rejected imported patch changed the authoritative revision");
    check(h.session.latest_frame()->rgba == edited_pixels, "Rejected imported patch changed the visible scene");

    const auto before_diagnostic = h.session.latest_frame()->info.frame_id;
    take(h.session.send("diagnostic\n1"), "Diagnose imported mesh");
    h.until("imported source-face diagnostic provenance", [&] {
        return h.matching_frame(h.authored.document.revision) && h.session.latest_frame()->info.frame_id > before_diagnostic &&
               h.info.find("blueprint #" + std::to_string(static_cast<vng::u32>(blueprint))) != std::string::npos;
    });
    check(bright_pixels(*h.session.latest_frame()) > 100, "Imported diagnostic capture contains no source faces");
    const auto before_normal = h.session.latest_frame()->info.frame_id;
    take(h.session.send("diagnostic\n0"), "Restore imported production view");
    h.until("production image after imported diagnostic", [&] {
        return h.matching_frame(h.authored.document.revision) && h.session.latest_frame()->info.frame_id > before_normal;
    });
    check(equivalent_pixels(h.session.latest_frame()->rgba, edited_pixels),
          "Imported diagnostics changed subsequent production output");
    const auto generation = h.session.active_generation();
    take(h.session.request_reload(), "Reload worker with distinct authored mesh blueprints");
    h.until("reloaded worker preserves imported geometry and edits", [&] {
        return h.session.active_generation() > generation && h.matching_frame(h.authored.document.revision) &&
               h.schema && h.schema->stamp.generation == h.session.active_generation() &&
               h.schema->stamp.object == instance;
    });
    check(equivalent_pixels(h.session.latest_frame()->rgba, edited_pixels),
          "Worker reload lost an imported blueprint or its edited vertex");
    const auto restarted = h.query_stats();
    check(restarted.at("full_mesh_uploads") == 2, "Reload did not realize both retained mesh blueprints");
    std::cout << "Imported blueprints passed: distinct cube + RGB triangle, asset-local patch, "
                 "bounds rejection, diagnostic provenance, and worker reload.\n";
}

void imported_position_test(Harness& h) {
    // Exercise the asset that exposed the regression: its full scene payload
    // exceeds 3 MB, but every unkeyed move must still be exactly 48 bytes.
    auto initial = project::State{.document = {.revision = h.authored.document.revision + 1, .mesh = h.authored.document.mesh}};
    initial.viewport.sequence = h.authored.viewport.sequence + 1; // Newer than any view request already sent.
    initial.viewport.editor_camera = {.yaw = 25, .pitch = 25, .distance = 10};
    initial.document.keyframe_names[3] = "Editable pose";
    project::mesh_settings(initial, 1)->visible = false;
    project::sun_settings(initial, 2)->visible = false;
    const auto path = std::filesystem::path(VNG_EDITOR_MESH_PATH).parent_path() / "spaceship.vmesh";
    const auto object = take(project::import_mesh(initial, path), "Import spaceship drag fixture");
    const auto blueprint = project::find_instance(initial, object)->blueprint;
    project::instance_transform(initial, object)->scale = .5F;
    h.authored = std::move(initial);
    h.send_snapshot();
    h.until("spaceship position-test baseline", [&] {
        return h.matching_frame(h.authored.document.revision) && h.schema &&
               h.schema->stamp.object == object && h.schema->stamp.revision == h.authored.document.revision;
    });
    check(bright_pixels(*h.session.latest_frame()) > 100,
          "Spaceship movement fixture is not visible");
    const auto initial_pixels = h.session.latest_frame()->rgba;
    const auto before = take(project::capture_position(h.authored, object), "Capture drag start");
    const auto counters = h.query_stats();
    const auto state_replies = h.acknowledgements;
    std::vector<double> move_ms;
    std::string last_packet;
    vng::u64 accepted_edits{};

    const auto send_position = [&](bool unkeyed = false) {
        const auto base = h.authored.document.revision++;
        auto edit = take(project::position_edit(base, h.authored, object), "Capture compact position");
        last_packet = take(project::encode_position_edit(edit), "Encode compact position");
        check(last_packet.size() < 512, "Small position track unexpectedly contains scene data");
        if (unkeyed)
            check(last_packet.size() == 48, "Unkeyed drag serialized more than one position");
        const auto started = Clock::now();
        take(h.session.send("position\n" + last_packet), "Send compact spaceship position");
        h.until("position revision, inspector and rendered result", [&] {
            return h.acknowledged_revision == h.authored.document.revision && h.matching_frame(h.authored.document.revision) &&
                   h.schema && h.schema->stamp.object == object &&
                   h.schema->stamp.revision == h.authored.document.revision;
        });
        move_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
        ++accepted_edits;
        check(h.session.latest_frame()->info.time == static_cast<double>(h.authored.viewport.time),
              "Dragging advanced the paused playhead");
        check(h.acknowledgements == state_replies, "Dragging sent a full authored scene reply");
    };
    for (int step = 0; step < 16; ++step) {
        take(project::apply_position_value(h.authored, object,
                                           {-.75F + static_cast<float>(step) * .1F, .2F, 0}),
             "Move authored spaceship");
        send_position(true);
    }
    const auto committed = take(project::capture_position(h.authored, object), "Capture drag result");
    const auto committed_pixels = h.session.latest_frame()->rgba;
    check(!equivalent_pixels(committed_pixels, initial_pixels),
          "Compact positions did not move the rendered spaceship");
    const auto position_control = std::ranges::find(h.schema->controls, std::string{"position"},
                                                   &editor::Control::key);
    check(position_control != h.schema->controls.end() && position_control->fields.size() == 1 &&
              std::get<vng::Vec3>(position_control->fields.front().value) == committed.base_position,
          "Inspector did not report the final accepted position");

    // Cancel and Undo both restore the captured property, not the entire mesh
    // document. Redo restores the exact committed pose through the same path.
    take(project::restore_position(h.authored, before), "Cancel spaceship drag");
    send_position(true);
    check(equivalent_pixels(h.session.latest_frame()->rgba, initial_pixels),
          "Cancelling drag did not restore the original spaceship pixels");
    take(project::restore_position(h.authored, committed), "Redo spaceship drag");
    send_position(true);
    check(equivalent_pixels(h.session.latest_frame()->rgba, committed_pixels),
          "Redo did not restore the committed spaceship position");
    take(project::restore_position(h.authored, before), "Undo spaceship drag");
    send_position(true);
    check(equivalent_pixels(h.session.latest_frame()->rgba, initial_pixels),
          "Undo did not restore the original spaceship position");

    // An already animated position edits a key at the paused playhead, never
    // the unrelated authored base. Test worker evaluation against the same
    // unkeyed pose, then restore the exact pre-drag key set (no extra key).
    inspect_time(h, 3);
    take(project::key_property(h.authored, {object, "position"}, 6, vng::Vec3{-.5F, 0, 0}),
         "Create spaceship position animation");
    send_position();
    const auto keyed_before = take(project::capture_position(h.authored, object), "Capture original keys");
    const auto keyed_initial_pixels = h.session.latest_frame()->rgba;
    const vng::Vec3 keyed_position{.6F, .3F, 0};
    take(project::apply_position_value(h.authored, object, keyed_position), "Move keyed spaceship at playhead");
    send_position();
    const auto keyed_after = take(project::capture_position(h.authored, object), "Capture edited keys");
    check(keyed_after.base_position == keyed_before.base_position && keyed_after.track &&
              keyed_before.track && keyed_after.track->keys.size() == keyed_before.track->keys.size() + 1,
          "Dragging keyed position changed its base or failed to insert the playhead key");
    const auto keyed_pixels = h.session.latest_frame()->rgba;
    take(project::restore_position(h.authored, {object, keyed_position, std::nullopt}),
         "Use unkeyed equivalent pose");
    send_position(true);
    check(equivalent_pixels(h.session.latest_frame()->rgba, keyed_pixels),
          "Worker keyed position differs from its evaluated unkeyed pose");
    take(project::restore_position(h.authored, keyed_after), "Restore keyed drag result");
    send_position();
    check(equivalent_pixels(h.session.latest_frame()->rgba, keyed_pixels),
          "Restoring the keyed drag changed rendered position");
    take(project::restore_position(h.authored, keyed_before), "Cancel keyed drag exactly");
    send_position();
    check(equivalent_pixels(h.session.latest_frame()->rgba, keyed_initial_pixels),
          "Keyed cancel left behind an extra key or changed the previous curve");

    h.allow_resync = true;
    const auto rejected = h.resyncs;
    take(h.session.send("position\n" + last_packet), "Replay stale compact position");
    h.until("stale compact position rejection", [&] { return h.resyncs > rejected; });
    const auto missing_rejections = h.resyncs;
    const project::PositionEdit missing{h.authored.document.revision, h.authored.document.revision + 1,
                                       {999999, {}, std::nullopt}};
    take(h.session.send("position\n" +
                        take(project::encode_position_edit(missing), "Encode missing-object position")),
         "Reject position for an object absent from this scene");
    h.until("missing-object position rejection", [&] { return h.resyncs > missing_rejections; });
    h.allow_resync = false;
    check(h.query_stats().at("revision") == static_cast<double>(h.authored.document.revision),
          "Stale compact position mutated the worker revision");

    const auto frame_id = h.session.latest_frame()->info.frame_id;
    take(h.session.send("diagnostic\n1"), "Diagnose moved spaceship source geometry");
    h.until("moved spaceship diagnostic provenance", [&] {
        return h.matching_frame(h.authored.document.revision) && h.session.latest_frame()->info.frame_id > frame_id &&
               h.info.find("blueprint #" + std::to_string(static_cast<vng::u32>(blueprint))) != std::string::npos;
    });
    check(bright_pixels(*h.session.latest_frame()) > 100,
          "Moved spaceship diagnostic output lost its source geometry");
    const auto after = h.query_stats();
    for (const auto key : {"scene_encode_calls", "scene_decode_calls", "mesh_update_calls",
                           "full_mesh_uploads", "vertex_updates", "vertex_bytes_uploaded"})
        check(after.at(key) == counters.at(key), std::string("Position drag performed forbidden work: ") + key);
    check(after.at("position_edits") == counters.at("position_edits") + static_cast<double>(accepted_edits),
          "Worker did not accept precisely the valid compact position edits");
    check(h.acknowledgements == state_replies, "Compact drag emitted full authored scene snapshots");
    std::ranges::sort(move_ms);
    std::cout << "Spaceship position regression passed: " << accepted_edits
              << " compact edits, keyed/cancel/undo/provenance, zero scene encodes/decodes or mesh updates; "
              << "median message-to-frame " << move_ms[move_ms.size() / 2] << " ms.\n";
}

void compact_selection_test(Harness& h) {
    // Keep the imported 21k-vertex ship from the position regression resident.
    // Selecting it must cost the same 32-byte payload as selecting the cube.
    const auto imported = h.authored.viewport.selected_object;
    const auto* geometry = project::instance_mesh(h.authored, imported);
    check(geometry && geometry->size() > 20000, "Selection regression requires the imported spaceship");
    const auto frame_before = h.session.latest_frame()->info.frame_id;
    take(h.session.send("diagnostic\n0"), "Use production color for selection regression");
    h.until("selection baseline production frame", [&] {
        return h.matching_frame(h.authored.document.revision) &&
               h.session.latest_frame()->info.frame_id > frame_before;
    });
    const auto pixels = h.session.latest_frame()->rgba;
    const auto counters = h.query_stats();
    const auto state_replies = h.acknowledgements;
    std::string last_packet;
    std::vector<double> schema_ms;
    const std::array<std::array<vng::u32, 2>, 8> selections{{{1, 0}, {2, 0}, {0, 0},
        {imported, 20000}, {1, 1}, {imported, static_cast<vng::u32>(geometry->size() - 1)},
        {0, 0}, {imported, 0}}};
    for (const auto selection : selections) {
        const auto base = h.authored.document.revision;
        h.authored.viewport.selected_object = selection[0];
        h.authored.viewport.selected_vertex = selection[1];
        ++h.authored.document.revision;
        const auto edit = take(project::selection_edit(base, h.authored), "Capture compact selection");
        last_packet = take(project::encode_selection_edit(edit), "Encode compact selection");
        check(last_packet.size() == 32, "Selection packet contains data beyond view-state IDs");
        const auto started = Clock::now();
        take(h.session.send("selection\n" + last_packet), "Send compact selection");
        h.until("selected-object inspector without scene resend", [&] {
            return h.acknowledged_revision == h.authored.document.revision && h.schema &&
                   h.schema->stamp.object == selection[0] &&
                   h.schema->stamp.revision == h.authored.document.revision;
        });
        schema_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
        if (selection[0]) {
            check(std::ranges::any_of(h.schema->controls, [](const auto& control) {
                      return control.kind == editor::Kind::translation_gizmo && control.key == "position";
                  }), "Selected object did not expose its position gizmo");
        } else {
            check(h.schema->controls.empty(), "Deselection left stale controls enabled");
        }
        h.until("compact selection frame revision", [&] { return h.matching_frame(h.authored.document.revision); });
        check(equivalent_pixels(pixels, h.session.latest_frame()->rgba),
              "Selecting an object changed authored production rendering");
        check(h.session.latest_frame()->info.time == static_cast<double>(h.authored.viewport.time),
              "Selection advanced the paused playback clock");
        check(h.acknowledgements == state_replies, "Selection emitted a complete scene reply");
    }
    h.allow_resync = true;
    const auto reject = [&](std::string wire, std::string_view reason) {
        const auto previous = h.resyncs;
        take(h.session.send("selection\n" + wire), "Send invalid compact selection");
        h.until(reason, [&] { return h.resyncs > previous; });
    };
    reject(last_packet, "stale compact selection rejection");
    reject(take(project::encode_selection_edit({h.authored.document.revision, h.authored.document.revision + 1, 999999, 0}),
                "Encode missing-object selection"), "missing-object selection rejection");
    reject(take(project::encode_selection_edit({h.authored.document.revision, h.authored.document.revision + 1, 1, 20000}),
                "Encode out-of-range cube vertex"), "destination vertex selection rejection");
    auto malformed = last_packet;
    malformed[7] = 2;
    reject(std::move(malformed), "unknown selection protocol version rejection");
    h.allow_resync = false;
    const auto after = h.query_stats();
    check(after.at("revision") == static_cast<double>(h.authored.document.revision) &&
              after.at("selected_object") == imported && after.at("selected_vertex") == 0,
          "Rejected selection mutated worker state");
    for (const auto key : {"scene_encode_calls", "scene_decode_calls", "mesh_update_calls",
                           "full_mesh_uploads", "vertex_updates", "vertex_bytes_uploaded"})
        check(after.at(key) == counters.at(key), std::string("Selection performed forbidden work: ") + key);
    check(after.at("selection_edits") == counters.at("selection_edits") + static_cast<double>(selections.size()),
          "Worker did not accept exactly the valid compact selections");
    check(h.acknowledgements == state_replies, "Selection emitted a complete authored snapshot");
    std::ranges::sort(schema_ms);
    std::cout << "Compact selection regression passed: " << selections.size()
              << " selections, zero scene encodes/decodes or mesh updates; median message-to-schema "
              << schema_ms[schema_ms.size() / 2] << " ms.\n";
}


void imported_rotation_test(Harness& h) {
    inspect_time(h, 0);
    // Reuse the resident 21k-vertex ship: rotations must not re-upload geometry
    // or serialize its multi-megabyte scene on either side of the bridge.
    const auto object = h.authored.viewport.selected_object;
    const auto* mesh = project::instance_mesh(h.authored, object);
    check(mesh && mesh->size() > 20000, "Rotation regression requires the imported spaceship");
    const auto blueprint = project::find_instance(h.authored, object)->blueprint;
    const auto before = take(project::capture_rotation(h.authored, object), "Capture original rotation");
    const auto initial_pixels = h.session.latest_frame()->rgba;
    const auto counters = h.query_stats();
    const auto state_replies = h.acknowledgements;
    const auto before_position = take(project::capture_position(h.authored, object), "Retain independent position");
    const auto cube = h.authored.document.mesh.document();
    std::string last_packet;
    vng::u64 accepted{};

    const auto send_rotation = [&](bool unkeyed = false) {
        const auto base = h.authored.document.revision++;
        const auto edit = take(project::rotation_edit(base, h.authored, object), "Capture compact rotation");
        last_packet = take(project::encode_rotation_edit(edit), "Encode compact rotation");
        check(last_packet.size() < 512, "Rotation packet unexpectedly contains scene data");
        if (unkeyed) check(last_packet.size() == 48, "Unkeyed rotation is not exactly 48 bytes");
        take(h.session.send("rotation\n" + last_packet), "Send spaceship rotation");
        h.until("rotation revision, inspector and rendered result", [&] {
            return h.acknowledged_revision == h.authored.document.revision && h.matching_frame(h.authored.document.revision) &&
                   h.schema && h.schema->stamp.object == object &&
                   h.schema->stamp.revision == h.authored.document.revision;
        });
        ++accepted;
        check(h.session.latest_frame()->info.time == static_cast<double>(h.authored.viewport.time),
              "Rotating advanced the paused playhead");
        check(h.acknowledgements == state_replies, "Rotating emitted a full authored scene reply");
    };
    for (int step = 1; step <= 8; ++step) {
        take(project::apply_rotation_value(h.authored, object,
                                           {10.F, 5.F * static_cast<float>(step), -15.F}),
             "Rotate authored spaceship");
        send_rotation(true);
    }
    const auto committed = take(project::capture_rotation(h.authored, object), "Capture rotation result");
    const auto committed_pixels = h.session.latest_frame()->rgba;
    check(!equivalent_pixels(committed_pixels, initial_pixels), "Rotation did not change the rendered spaceship");
    const auto transform = std::ranges::find(h.schema->controls, std::string{"transform"}, &editor::Control::key);
    check(transform != h.schema->controls.end(), "Rotation did not refresh the transform inspector");
    const auto field = std::ranges::find(transform->fields, std::string{"rotation"}, &editor::Field::key);
    check(field != transform->fields.end() && std::get<vng::Vec3>(field->value) == committed.base_rotation,
          "Inspector did not report the accepted rotation");

    take(project::restore_rotation(h.authored, before), "Cancel spaceship rotation");
    send_rotation(true);
    check(equivalent_pixels(h.session.latest_frame()->rgba, initial_pixels),
          "Rotation cancel did not restore original pixels");
    take(project::restore_rotation(h.authored, committed), "Redo spaceship rotation");
    send_rotation(true);
    check(equivalent_pixels(h.session.latest_frame()->rgba, committed_pixels),
          "Rotation redo did not restore committed pixels");
    take(project::restore_rotation(h.authored, before), "Undo spaceship rotation");
    send_rotation(true);

    inspect_time(h, 3);
    take(project::key_property(h.authored, {object, "rotation"}, 6, vng::Vec3{0, 65, 0},
                               vng::timeline::Interpolation::hold), "Create rotation animation");
    send_rotation();
    const auto keyed_before = take(project::capture_rotation(h.authored, object), "Capture rotation track");
    const auto keyed_initial_pixels = h.session.latest_frame()->rgba;
    const vng::Vec3 pose{15, 32, -20};
    take(project::apply_rotation_value(h.authored, object, pose), "Rotate at paused playhead");
    send_rotation();
    const auto keyed_after = take(project::capture_rotation(h.authored, object), "Capture edited rotation track");
    check(keyed_after.base_rotation == keyed_before.base_rotation && keyed_after.track && keyed_before.track &&
          keyed_after.track->keys.size() == keyed_before.track->keys.size() + 1,
          "Rotation drag modified base or failed to insert a playhead key");
    const auto keyed_pixels = h.session.latest_frame()->rgba;
    take(project::restore_rotation(h.authored, {object, pose, std::nullopt}), "Use equivalent unkeyed rotation");
    send_rotation(true);
    check(equivalent_pixels(h.session.latest_frame()->rgba, keyed_pixels),
          "Worker's keyed and equivalent unkeyed rotations disagree");
    take(project::restore_rotation(h.authored, keyed_before), "Cancel exact keyed rotation");
    send_rotation();
    check(equivalent_pixels(h.session.latest_frame()->rgba, keyed_initial_pixels),
          "Rotation cancellation left a key or changed the preceding curve");

    h.allow_resync = true;
    const auto reject = [&](const std::string& payload) {
        const auto old = h.resyncs;
        take(h.session.send("rotation\n" + payload), "Reject invalid rotation");
        h.until("invalid rotation resynchronization", [&] { return h.resyncs > old; });
    };
    reject(last_packet); // Stale revision.
    reject(take(project::encode_rotation_edit(
        {h.authored.document.revision, h.authored.document.revision + 1, {999999, {}, {}}}),
        "Encode missing-instance rotation"));
    auto malformed = last_packet;
    malformed[7] = 2;
    reject(malformed);
    h.allow_resync = false;
    check(h.query_stats().at("revision") == static_cast<double>(h.authored.document.revision),
          "Rejected rotation changed worker revision");

    const auto old_frame = h.session.latest_frame()->info.frame_id;
    take(h.session.send("diagnostic\n1"), "Diagnose rotated mesh source faces");
    h.until("rotated spaceship diagnostic provenance", [&] {
        return h.matching_frame(h.authored.document.revision) && h.session.latest_frame()->info.frame_id > old_frame &&
               h.info.find("blueprint #" + std::to_string(static_cast<vng::u32>(blueprint))) != std::string::npos;
    });
    check(bright_pixels(*h.session.latest_frame()) > 100, "Rotating lost diagnostic mesh geometry");
    check(take(project::capture_position(h.authored, object), "Check independent position") == before_position,
          "Rotation changed the position/base animation property");
    check(h.authored.document.mesh.document() == cube, "Rotation changed the unrelated cube blueprint");
    const auto after = h.query_stats();
    for (const auto key : {"scene_encode_calls", "scene_decode_calls", "mesh_update_calls",
                           "full_mesh_uploads", "vertex_updates", "vertex_bytes_uploaded"})
        check(after.at(key) == counters.at(key), std::string("Rotation performed forbidden work: ") + key);
    check(after.at("rotation_edits") == counters.at("rotation_edits") + static_cast<double>(accepted),
          "Worker did not accept precisely the valid compact rotation edits");
    std::cout << "Spaceship rotation passed: live pixels, keyed/cancel/undo/provenance; zero scene encodes/decodes or uploads.\n";
}
void imported_scale_test(Harness& h) {
    const auto object=h.authored.viewport.selected_object;
    const auto before=take(project::capture_scale(h.authored,object),"Capture original scale");
    const auto baseline=h.query_stats();
    const auto replies=h.acknowledgements;
    auto pixels=h.session.latest_frame()->rgba;
    unsigned accepted{};
    const auto send=[&] {
        const auto base=h.authored.document.revision++;
        const auto edit=take(project::scale_edit(base,h.authored,object),"Capture scale packet");
        const auto bytes=take(project::encode_scale_edit(edit),"Encode scale packet");
        check(bytes.size()<512,"Scale packet contains whole scene data");
        take(h.session.send("scale\n"+bytes),"Send scale");
        h.until("scaled image and compact ACK",[&] {
            return h.acknowledged_revision==h.authored.document.revision &&
                h.matching_frame(h.authored.document.revision) && h.schema &&
                h.schema->stamp.revision==h.authored.document.revision;
        });
        ++accepted;
    };
    for(const float scale:{.35F,.7F}) {
        take(project::apply_scale_value(h.authored,object,scale),"Apply scale locally");
        send();
        check(h.session.latest_frame()->rgba!=pixels,"Scaling did not change rendered pixels");
        pixels=h.session.latest_frame()->rgba;
    }
    take(project::restore_scale(h.authored,before),"Restore original scale"); send();
    const auto after=h.query_stats();
    for(const auto key:{"scene_encode_calls","scene_decode_calls","mesh_update_calls",
                       "full_mesh_uploads","vertex_updates","vertex_bytes_uploaded"})
        check(after.at(key)==baseline.at(key),std::string("Scale performed forbidden work: ")+key);
    check(h.acknowledgements==replies,"Scale returned an entire authored scene");
    check(after.at("scale_edits")==baseline.at("scale_edits")+accepted,"Scale packet was not accepted");
    std::cout<<"Spaceship scale passed: changed pixels; zero scene serialization or mesh uploads.\n";
}
void mixed_document_patch_test(Harness& h) {
    const auto baseline = h.query_stats();
    const auto base = h.authored.document.revision;
    project::DocumentChanges changes;
    changes.vertices[1] = {0, 2};
    check(!h.authored.document.mesh_assets.empty(), "Mixed patch test needs imported geometry");
    const auto asset = h.authored.document.mesh_assets.front().id;
    changes.vertices[static_cast<vng::u32>(asset)] = {1};
    auto* imported = project::mesh_geometry(h.authored, asset);
    auto p = imported->position(1); p.x += .01F;
    take(imported->set_position(1, p), "Edit imported blueprint position");
    for (auto index : {0U, 2U}) {
        auto point = h.authored.document.mesh.position(index); point.x += .01F;
        take(h.authored.document.mesh.set_position(index, point), "Edit cube position");
    }
    changes.properties = {{1, "brightness"}, {2, "bloom"}};
    project::mesh_settings(h.authored, 1)->brightness = .8F;
    project::sun_settings(h.authored, 2)->bloom = .3F;
    ++h.authored.document.revision;
    auto patch = take(project::capture_patch(base, h.authored, changes), "Capture mixed patch");
    // Blueprint order must not imply selection or affect transaction semantics.
    std::ranges::reverse(patch.vertices);
    const auto wire = take(project::encode_patch(patch), "Encode mixed patch");
    check(wire.size() < 1024, "Mixed patch unexpectedly includes mesh/scene data");
    take(h.session.send("patch\n" + wire), "Send mixed patch");
    h.until("mixed authored patch acknowledged and rendered", [&] {
        return h.acknowledged_revision == h.authored.document.revision && h.matching_frame(h.authored.document.revision);
    });
    const auto after = h.query_stats();
    for (const auto key : {"scene_encode_calls", "scene_decode_calls", "full_mesh_uploads"})
        check(after.at(key) == baseline.at(key), std::string("Mixed patch did unrelated work: ") + key);
    check(after.at("document_patches") == baseline.at("document_patches") + 1, "Mixed patch was not applied");
    check(after.at("mesh_update_calls") == baseline.at("mesh_update_calls") + 2, "Mixed patch did not target exactly two blueprints");
    check(after.at("vertex_bytes_uploaded") == baseline.at("vertex_bytes_uploaded") + 3 * 16,
          "Mixed patch uploaded unrelated vertex records");
    std::cout << "Mixed patch passed: two blueprints + appearance, 3 records uploaded, zero scene serialization.\n";
}

void viewport_timing_test(Harness& h) {
    const auto baseline=h.query_stats();
    const auto schema_count=h.schemas;
    const auto revision=h.authored.document.revision;
    const auto previous_frame=h.session.latest_frame()->info.frame_id;
    const auto distance=h.authored.viewport.editor_camera.distance;
    auto view=h.authored.viewport;
    view.sequence+=1000;
    view.smooth_zoom=true;
    view.editor_camera.distance=std::clamp(distance*.8F,project::camera_min_distance,project::camera_max_distance);
    const auto now=vng::monotonic_ns();
    editor::InteractionOrigin origin{9001,now,now,1,now,0,false};
    auto bytes=project::encode_viewport_request({revision,view});
    check(bool(bytes),"Cannot encode viewport target");
    take(h.session.send_latest(editor::traced_message("view\n"+*bytes,origin)),"Send latest viewport target");
    h.until("first frame incorporating separately sequenced viewport input",[&] {
        const auto& frame=h.session.latest_frame();
        return frame && frame->info.frame_id>previous_frame && frame->info.view_sequence==view.sequence;
    });
    const auto first=h.session.latest_frame()->info;
    check(first.scene_revision==revision,"Private zoom advanced document revision");
    check(first.has_view,"Rendered frame did not carry actual view metadata");
    check(first.interaction.origin.id==origin.id,"Presented frame lost interaction identity");
    check(first.interaction.origin.sent_ns<=first.interaction.worker_received_ns &&
          first.interaction.worker_received_ns<=first.interaction.worker_applied_ns &&
          first.interaction.worker_applied_ns<=first.interaction.render_started_ns &&
          first.interaction.render_started_ns<=first.interaction.readback_ready_ns &&
          first.interaction.readback_ready_ns<=first.interaction.published_ns &&
          first.interaction.published_ns<=first.interaction.ui_received_ns,"Noncausal cross-process timestamps");
    h.until("zoom target settled without another input request",[&] {
        const auto& frame=h.session.latest_frame();
        return frame && frame->info.view_sequence==view.sequence && frame->info.settled;
    });
    check(h.session.latest_frame()->info.view_camera[2]==view.editor_camera.distance,"Settled camera differs from target");
    // Supersede a burst before delivery. No ACK and no historical input queue.
    for (unsigned i=0;i<32;++i) {
        ++view.sequence;
        view.editor_camera.yaw=static_cast<float>(i)-16;
        bytes=project::encode_viewport_request({revision,view});
        check(bool(bytes),"Cannot encode latest camera burst");
        take(h.session.send_latest("view\n"+*bytes),"Coalesce camera burst");
    }
    h.until("newest absolute viewport target wins",[&] {
        const auto& frame=h.session.latest_frame();
        return frame && frame->info.view_sequence==view.sequence && frame->info.settled;
    });
    check(h.session.latest_frame()->info.view_camera[0]==view.editor_camera.yaw,"Old camera overwrote newest target");
    const auto after=h.query_stats();
    for (const auto key:{"scene_encode_calls","scene_decode_calls","mesh_update_calls","full_mesh_uploads","vertex_updates",
                         "inspector_rebuilds", "schema_sends"})
        check(after.at(key)==baseline.at(key),std::string("Private camera caused unnecessary work: ")+key);
    check(h.schemas==schema_count,"Camera-only input rebuilt inspector schema");
    check(after.at("revision")==static_cast<double>(revision),"Latest-value lane modified authored revision");
    h.authored.viewport=view;
    std::cout << "Viewport boundary/timing passed: independent sequence, latest burst, actual camera, settled zoom, zero document/mesh/schema work.\n";
}

void mesh_draft_test(Harness& h) {
    const auto mesh=take(editor::EditableMesh::load(VNG_EDITOR_MESH_PATH),"Load draft fixture");
    project::State clean{.document={.mesh=mesh}};
    clean.document.revision=h.authored.document.revision+1;
    clean.viewport.sequence=h.authored.viewport.sequence+1;
    clean.viewport.selected_object=1;clean.viewport.time=0;
    project::sun_settings(clean,2)->visible=false;
    h.authored=std::move(clean);h.send_snapshot();
    h.until("published scene baseline",[&]{return h.matching_frame(h.authored.document.revision);});
    const auto original=h.session.latest_frame()->rgba;
    take(project::begin_mesh_draft(h.authored,project::BlueprintId::mesh),"Begin cube draft");
    auto* draft=project::mesh_edit_geometry(h.authored,project::BlueprintId::mesh);
    for(vng::u32 i=0;i<draft->size();++i) {auto p=draft->position(i);p.x*=2;take(draft->set_position(i,p),"Scale draft only");}
    ++h.authored.document.revision;h.send_snapshot();
    h.until("draft snapshot in scene view",[&]{return h.matching_frame(h.authored.document.revision);});
    check(equivalent_pixels(original,h.session.latest_frame()->rgba),"Draft snapshot leaked into scene rendering");
    const auto show=[&](project::ViewMode mode) {
        h.authored.viewport.mode=mode;h.authored.viewport.selected_vertex=0;
        ++h.authored.viewport.sequence;
        auto request=project::encode_viewport_request({h.authored.document.revision,h.authored.viewport});
        check(bool(request),"Encode draft view");
        take(h.session.send_latest("view\n"+*request),"Switch draft view");
        h.until("draft/applied view presented",[&]{const auto& frame=h.session.latest_frame();return frame && frame->info.scene_revision==h.authored.document.revision && frame->info.view_sequence==h.authored.viewport.sequence;});
    };
    show(project::ViewMode::mesh);
    const auto draft_pixels=h.session.latest_frame()->rgba;
    const auto before=h.query_stats();
    const project::VertexEdit edit{h.authored.document.revision,h.authored.document.revision+1,{{0,{2,1,1}}},1};
    take(project::apply_edit(h.authored,edit),"Apply draft position patch");
    take(h.session.send("vertices\n"+take(project::encode_edit(edit),"Encode draft patch")),"Send draft patch");
    h.until("draft patch rendered",[&]{return h.matching_frame(h.authored.document.revision);});
    const auto after=h.query_stats();
    check(after.at("scene_encode_calls")==before.at("scene_encode_calls") && after.at("scene_decode_calls")==before.at("scene_decode_calls"),"Draft drag serialized a scene");
    show(project::ViewMode::scene);
    check(equivalent_pixels(original,h.session.latest_frame()->rgba),"View switch published draft positions");
    // A delayed draft patch must not overwrite the GPU's published scene copy.
    const project::VertexEdit delayed{h.authored.document.revision,h.authored.document.revision+1,{{1,{2,-1,1}}},1};
    take(project::apply_edit(h.authored,delayed),"Update hidden draft");
    take(h.session.send("vertices\n"+take(project::encode_edit(delayed),"Encode hidden draft patch")),"Send hidden draft patch");
    h.until("hidden draft patch acknowledged",[&]{return h.matching_frame(h.authored.document.revision);});
    check(equivalent_pixels(original,h.session.latest_frame()->rgba),"Hidden draft patch modified scene pixels");
    show(project::ViewMode::mesh);
    check(!equivalent_pixels(draft_pixels,h.session.latest_frame()->rgba),"Returning to draft lost streamed edits");
    show(project::ViewMode::scene);
    take(project::apply_mesh_draft(h.authored,project::BlueprintId::mesh),"Publish draft");
    ++h.authored.document.revision;h.send_snapshot();
    h.until("explicit Apply published",[&]{return h.matching_frame(h.authored.document.revision);});
    check(!equivalent_pixels(original,h.session.latest_frame()->rgba),"Apply did not change scene pixels");
    std::cout<<"Mesh drafts passed: view isolation, tiny patches, hidden edits, explicit publication changes pixels.\n";
}

void integration_test() {
    auto mesh = take(editor::EditableMesh::load(VNG_EDITOR_MESH_PATH), "Load bundled editor mesh");
    project::State initial{.document = {.mesh = std::move(mesh)}};
    initial.viewport.paused = true;
    initial.viewport.selected_object = 2;
    const preview::PreviewConfig options{
        .build_command = {"/usr/bin/true"},
        .build_directory = std::filesystem::path(VNG_EDITOR_WORKER_PATH).parent_path(),
        .worker_executable = VNG_EDITOR_WORKER_PATH,
        .max_width = 1600,
        .max_height = 1200,
        .build_timeout = std::chrono::seconds{10},
        .startup_timeout = std::chrono::seconds{120},
    };
    Harness h{std::move(initial),
              take(preview::PreviewSession::create(options), "Launch actual preview worker")};
    h.until("initial rendered scene and inspector", [&] {
        return h.matching_frame(h.authored.document.revision) && h.schema &&
               h.schema->stamp ==
                   editor::Stamp{2, h.session.active_generation(), h.authored.document.revision,h.authored.viewport.sequence};
    });
    const auto first_generation = h.session.active_generation();
    check(bright_pixels(*h.session.latest_frame()) > 1000, "Initial sun and mesh preview is black");
    check(h.session.latest_frame()->info.time == static_cast<double>(h.authored.viewport.time),
          "Paused preview changed simulation time");
    check(h.info.find("Final sRGB") != std::string::npos,
          "Worker did not report its production presentation path");
    check(std::ranges::any_of(h.schema->controls, [](const auto& c) { return c.key == "surface"; }),
          "Sun did not describe its surface editor");

    const editor::Event apply{
        h.schema->stamp, "surface", editor::Phase::apply, {{"bloom", 0.4F}, {"white_spots", true}}};
    const auto packet = take(editor::encode_event(apply), "Encode inspector Apply");
    const auto initial_revision = h.authored.document.revision;
    const auto before_apply_stats = h.query_stats();
    take(h.session.send("event\n" + packet, first_generation), "Send inspector Apply");
    h.until("effect callback, authoritative acknowledgement, and rendered result", [&] {
        return h.acknowledgement && h.acknowledgement->document.revision == initial_revision + 1 &&
               h.schema && h.schema->stamp.revision == initial_revision + 1 &&
               h.matching_frame(initial_revision + 1);
    });
    h.authored = *h.acknowledgement;
    const auto after_apply_stats = h.query_stats();
    for (const auto key : {"scene_encode_calls", "scene_decode_calls", "mesh_update_calls", "full_mesh_uploads", "vertex_updates"})
        check(after_apply_stats.at(key) == before_apply_stats.at(key), std::string("Appearance callback did unrelated work: ") + key);
    check((*editor_example::sun_settings(h.authored, 2)).bloom == 0.4F && (*editor_example::sun_settings(h.authored, 2)).white_spots,
          "Apply did not update native SunSettings");
    const auto accepted = take(project::encode(h.authored), "Encode accepted state");
    const auto accepted_pixels = h.session.latest_frame()->rgba;
    const auto accepted_frame_id = h.session.latest_frame()->info.frame_id;
    const auto old_schemas = h.schemas;

    h.allow_stale = true;
    take(h.session.send("event\n" + packet, first_generation), "Replay stale inspector event");
    h.until("stale-event rejection and unchanged authoritative state",
            [&] { return h.stale_rejections == 1 && h.schemas > old_schemas; });
    h.allow_stale = false;
    check(take(project::encode(*h.acknowledgement), "Encode stale-event response") == accepted,
          "Rejected stale event changed the authored state");
    check(h.session.latest_frame()->info.frame_id == accepted_frame_id &&
              h.session.latest_frame()->rgba == accepted_pixels,
          "Rejected stale event unnecessarily replaced the paused preview");

    // A finite Vec3 passes the generic inspector's type checks, but must not
    // bypass the coordinate safety envelope and kill the preview during encode.
    const auto before_bounds_schema = h.schemas;
    const editor::Event outside{
        h.schema->stamp, "position", editor::Phase::apply, {{"position", vng::Vec3{project::scene_coordinate_limit + 1, 0, 0}}}};
    h.allow_bounds = true;
    take(h.session.send("event\n" +
                        take(editor::encode_event(outside), "Encode out-of-bounds gizmo")),
         "Send out-of-bounds gizmo");
    h.until("project-level bounds rejection and transaction rollback",
            [&] { return h.bound_rejections == 1 && h.schemas > before_bounds_schema; });
    h.allow_bounds = false;
    check(take(project::encode(*h.acknowledgement), "Encode bounds-rejection response") == accepted,
          "Project-level validation failure changed the authored state or revision");
    check(h.session.active_generation() == first_generation &&
              h.session.latest_frame()->rgba == accepted_pixels,
          "Invalid gizmo value destroyed the working preview");

    // Move the camera-nearest authored corner, including duplicated face
    // vertices, so this is an actual visible mesh edit rather than a hidden
    // back-face change that only happens to produce a new revision stamp.
    h.authored.viewport.selected_object = 1;
    std::optional<vng::u32> selected;
    float nearest = 2.0F;
    for (vng::u32 i = 0; i < h.authored.document.mesh.size(); ++i) {
        auto point = project::project_vertex(h.authored, i, {640, 480});
        if (point && point->z < nearest) {
            nearest = point->z;
            selected = i;
        }
    }
    check(selected.has_value(), "Bundled mesh has no visible editable vertex");
    const auto original = h.authored.document.mesh.position(*selected);
    const auto coincident = h.authored.document.mesh.coincident(*selected);
    take(h.authored.document.mesh.translate(coincident, {0.45F, 0.2F, 0}), "Translate welded mesh corner");
    check(h.authored.document.mesh.position(*selected) != original, "CPU mesh edit had no effect");
    h.authored.viewport.selected_vertex = *selected;
    ++h.authored.document.revision;
    h.send_snapshot();
    h.until("uploaded edited mesh", [&] {
        return h.matching_frame(h.authored.document.revision) && h.schema && h.schema->stamp.object == 1 &&
               h.schema->stamp.revision == h.authored.document.revision;
    });
    check(h.session.latest_frame()->rgba != accepted_pixels,
          "Moving a visible mesh corner did not change rendered pixels");
    const auto production_pixels = h.session.latest_frame()->rgba;
    const auto mesh_frame_id = h.session.latest_frame()->info.frame_id;

    take(h.session.send("diagnostic\n1"), "Enable enhanced diagnostic output");
    h.until("diagnostic source-face render", [&] {
        return h.matching_frame(h.authored.document.revision) &&
               h.session.latest_frame()->info.frame_id > mesh_frame_id &&
               h.info.find("Mesh source faces") != std::string::npos;
    });
    check(bright_pixels(*h.session.latest_frame()) > 1000,
          "Enhanced diagnostic image contains no mesh");
    check(h.session.latest_frame()->rgba != production_pixels,
          "Diagnostic output is indistinguishable from production color");
    const auto diagnostic_frame_id = h.session.latest_frame()->info.frame_id;
    take(h.session.send("diagnostic\n0"), "Restore production output");
    h.until("restored production image", [&] {
        return h.matching_frame(h.authored.document.revision) &&
               h.session.latest_frame()->info.frame_id > diagnostic_frame_id &&
               h.info.find("Final sRGB") != std::string::npos;
    });
    check(equivalent_pixels(h.session.latest_frame()->rgba, production_pixels),
          "Diagnostic capture changed subsequent normal rendering");

    take(h.session.request_reload(), "Build and replace actual project worker");
    check(h.session.active_generation() == first_generation,
          "Reload discarded the working preview before its replacement was ready");
    check(h.session.latest_frame()->rgba == production_pixels,
          "Reload discarded the last completed image");
    h.until("replacement worker with preserved authored edits", [&] {
        return h.session.active_generation() > first_generation &&
               h.matching_frame(h.authored.document.revision) && h.schema &&
               h.schema->stamp.generation == h.session.active_generation() &&
               h.schema->stamp.revision == h.authored.document.revision;
    });
    check(!h.session.busy(), "Reload remained busy after candidate activation");
    check(equivalent_pixels(h.session.latest_frame()->rgba, production_pixels),
          "Recompiled/restarted preview did not reproduce the authored scene within one byte");
    check(h.session.latest_frame()->info.time == static_cast<double>(h.authored.viewport.time),
          "Reload lost paused preview time");
    viewport_resolution_test(h);
    native_play_test(h);
    camera_and_selection_test(h);
    scene_camera_test(h);
    timeline_test(h);
    fps_settings_test(h);
    imported_blueprint_test(h);
    imported_position_test(h);
    compact_selection_test(h);
    imported_rotation_test(h);
    imported_scale_test(h);
    mixed_document_patch_test(h);
    viewport_timing_test(h);
    mesh_draft_test(h);
    large_timeline_test(h);
    std::cout << "Actual worker integration passed: scene + inspector Apply + stale rejection + "
                 "welded mesh edit + diagnostic render + reload.\n";
}
} // namespace

int main() {
    try {
        integration_test();
        return 0;
    } catch (const ContextUnavailable& error) {
        std::cout << "SKIP: OpenGL preview context unavailable: " << error.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "Editor worker integration failed: " << error.what() << '\n';
        return 1;
    }
}
