#pragma once
#include <vng/editor/limits.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vng/core/types.hpp>
#include <vng/editor/interaction_timing.hpp>

namespace vng::editor::preview {

struct Diagnostic { std::string message; };
template<class T> using Result = std::expected<T, Diagnostic>;

inline constexpr std::size_t max_message_bytes = vng::editor::max_message_bytes;

struct FrameInfo {
    std::uint32_t width{}, height{};
    std::uint64_t generation{}, scene_revision{}, frame_id{};
    double time{};
    std::uint64_t view_sequence{};
    // Actual rendered orbit pose, not the newest requested camera target.
    // Generic preview transport treats these application-defined floats as metadata.
    // yaw, pitch, orbit distance, target xyz, optical magnification.
    std::array<float, 7> view_camera{0,0,0,0,0,0,1};
    float view_far_plane{};
    std::uint32_t view_mode{}, inspected_mesh{};
    bool has_view{}, settled{true};
    InteractionTrace interaction{};
};
// Top-left-origin, tightly packed RGBA8. The application defines its color encoding.
struct PreviewFrame {
    FrameInfo info;
    std::vector<std::byte> rgba;
};

// Worker arguments are: --worker CONTROL_FD MEMORY_FD GENERATION.
// attach takes ownership of both descriptors, including on failure.
class WorkerEndpoint {
public:
    WorkerEndpoint(WorkerEndpoint&&) noexcept;
    WorkerEndpoint& operator=(WorkerEndpoint&&) noexcept;
    ~WorkerEndpoint();
    static Result<WorkerEndpoint> attach(int control_fd, int memory_fd,
                                         std::uint64_t generation);
    // Queues a bounded message. receive() flushes pending nonblocking writes.
    Result<void> send(std::string_view message);
    Result<std::vector<std::string>> receive();
    Result<void> ready();
    // False means the UI currently owns the shared image. No wait, no write;
    // retain/retry the latest complete frame (including an isolated final edit).
    Result<bool> try_publish(FrameInfo info, std::span<const std::byte> rgba);
    [[nodiscard]] Extent2D capacity() const noexcept;
    [[nodiscard]] bool closed() const noexcept;
private:
    struct Impl;
    explicit WorkerEndpoint(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

struct PreviewConfig {
    // Executed directly, never via a shell. Example: {"cmake", "--build", "build",
    // "--target", "vng_editor_worker", "-j", "4"}.
    std::vector<std::string> build_command;
    std::filesystem::path build_directory;
    std::filesystem::path worker_executable;
    std::uint32_t max_width{1280}, max_height{800};
    std::chrono::milliseconds build_timeout{std::chrono::minutes(5)};
    std::chrono::milliseconds startup_timeout{std::chrono::seconds(30)};
};

enum class EventKind {
    candidate_started, activated, message, build_finished, build_failed, worker_failed
};
struct PreviewEvent {
    EventKind kind{};
    std::uint64_t generation{};
    std::string message;
};

// Owns only its own child processes. poll never waits for a build, a worker,
// or the shared-image lock. Destruction terminates/reaps owned children.
// A restart runs trusted project code; this is fault isolation, not a sandbox.
class PreviewSession {
public:
    PreviewSession(PreviewSession&&) noexcept;
    PreviewSession& operator=(PreviewSession&&) noexcept;
    ~PreviewSession();
    static Result<PreviewSession> create(PreviewConfig config);
    Result<void> request_reload();
    std::vector<PreviewEvent> poll();
    // A generation of zero addresses the active worker, or the startup candidate
    // when there is no active worker. Explicit generations reject stale events.
    Result<void> send(std::string_view message, std::uint64_t generation = 0);
    // One replaceable pending value per generation, independent of document
    // acknowledgements. Never use this lane for commands/deltas/transactions.
    Result<void> send_latest(std::string_view message, std::uint64_t generation = 0);
    // Nonblocking delivery of queued requests for active/startup workers. Call
    // after collecting one UI tick's input to send its coalesced latest value
    // without waiting for the next poll(). Backpressure retains pending work;
    // poll() also retries it. This does not receive events or copy preview pixels.
    Result<void> flush_requests();
    [[nodiscard]] const std::optional<PreviewFrame>& latest_frame() const noexcept;
    // Transfers the completed image without copying pixels. latest_frame()
    // becomes empty until another image arrives; the caller retains display ownership.
    [[nodiscard]] std::optional<PreviewFrame> take_latest_frame() noexcept;
    [[nodiscard]] std::uint64_t active_generation() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] std::string_view status() const noexcept;
    [[nodiscard]] std::string_view logs() const noexcept;
private:
    struct Impl;
    explicit PreviewSession(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

} // namespace vng::editor::preview
