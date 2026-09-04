#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <vng/opengl/context_access.hpp>
#include <vng/opengl/default_framebuffer.hpp>
#include <vng/opengl/diagnostic.hpp>

namespace vng::opengl::detail {

// Append-only context history. The legacy take_* APIs use independent
// consumed indices, so diagnostic captures can read from old cursors without
// being affected by either consumer.
struct DiagnosticStore final {
    std::mutex mutex;
    std::vector<DebugMessage> debug_messages;
    std::vector<Diagnostic> lifecycle_diagnostics;
    std::size_t consumed_debug_messages{};
    std::size_t consumed_lifecycle_diagnostics{};
};

struct ContextState final {
    CurrentContextAccess access;
    int major{};
    int minor{};
    int maximum_draw_buffers{};
    DefaultFramebufferCapabilities default_framebuffer;
    bool debug_output_installed{};
    std::shared_ptr<DiagnosticStore> diagnostics;

    // Immediate OpenGL submission still needs one logical default-target
    // frame owner. A monotonically changing token prevents stale Frame values
    // from operating on a newer scope while remaining safe in destructors.
    std::atomic_uint64_t next_frame_generation{1};
    std::atomic_uint64_t active_frame_generation{};

    // Command streams are scoped by both the active frame generation and this
    // context-owned epoch. Keeping the epoch here avoids allocating a separate
    // shared authority object for every frame: Commands already retains the
    // ContextState through its lightweight Device facade. Only the latest
    // epoch issued for the active frame is accepted.
    std::atomic_uint64_t next_command_epoch{1};
    std::atomic_uint64_t active_command_epoch{};

    ~ContextState();

    [[nodiscard]] bool is_current() const noexcept {
        return access.valid()
            && std::this_thread::get_id() == access.owner_thread
            && access.is_current(access.lifetime->native_identity());
    }

    [[nodiscard]] std::expected<void, Diagnostic> require_current(
        std::string_view operation) const {
        if (!access.lifetime || !access.lifetime->alive()) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::context_expired,
                .message = std::string(operation) + ": the owning context no longer exists",
            });
        }
        if (std::this_thread::get_id() != access.owner_thread) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::wrong_thread,
                .message = std::string(operation) + ": called from a thread other than the context owner",
            });
        }
        if (!access.is_current(access.lifetime->native_identity())) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::context_not_current,
                .message = std::string(operation) + ": the owning OpenGL context is not current",
            });
        }
        return {};
    }

    [[nodiscard]] std::uint64_t try_begin_frame() noexcept {
        if (active_frame_generation.load(std::memory_order_acquire) != 0) {
            return 0;
        }

        auto generation = next_frame_generation.fetch_add(
            1, std::memory_order_relaxed);
        while (generation == 0) {
            generation = next_frame_generation.fetch_add(
                1, std::memory_order_relaxed);
        }
        std::uint64_t expected = 0;
        if (!active_frame_generation.compare_exchange_strong(
                expected,
                generation,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return 0;
        }
        active_command_epoch.store(0, std::memory_order_release);
        return generation;
    }

    [[nodiscard]] bool frame_is_active(std::uint64_t generation) const noexcept {
        return generation != 0
            && active_frame_generation.load(std::memory_order_acquire)
                == generation;
    }

    [[nodiscard]] bool end_frame(std::uint64_t generation) noexcept {
        if (generation == 0) {
            return false;
        }
        const auto ended = active_frame_generation.compare_exchange_strong(
            generation,
            0,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
        if (ended) {
            active_command_epoch.store(0, std::memory_order_release);
        }
        return ended;
    }

    [[nodiscard]] std::uint64_t renew_command_stream(
        std::uint64_t frame_generation) noexcept
    {
        if (!frame_is_active(frame_generation)) {
            return 0;
        }

        auto epoch = next_command_epoch.fetch_add(
            1, std::memory_order_relaxed);
        while (epoch == 0) {
            epoch = next_command_epoch.fetch_add(
                1, std::memory_order_relaxed);
        }
        active_command_epoch.store(epoch, std::memory_order_release);

        // Commands are owner-thread objects, but retain a defensive second
        // generation check so an overlapping end cannot publish a usable
        // stream after its frame has closed.
        if (!frame_is_active(frame_generation)) {
            std::uint64_t expected = epoch;
            (void)active_command_epoch.compare_exchange_strong(
                expected,
                0,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
            return 0;
        }
        return epoch;
    }

    [[nodiscard]] bool command_stream_is_active(
        std::uint64_t frame_generation,
        std::uint64_t command_epoch) const noexcept
    {
        return command_epoch != 0
            && frame_is_active(frame_generation)
            && active_command_epoch.load(std::memory_order_acquire)
                == command_epoch;
    }

    void record_lifecycle_failure(std::string message) noexcept {
        try {
            if (!diagnostics) {
                return;
            }
            std::lock_guard lock(diagnostics->mutex);
            diagnostics->lifecycle_diagnostics.push_back(Diagnostic{
                .code = ErrorCode::context_not_current,
                .message = std::move(message),
            });
        } catch (...) {
            // Destructors must not throw. The GL handle is intentionally leaked
            // when its context cannot be used safely.
        }
    }
};

[[nodiscard]] inline Diagnostic incompatible_device(std::string_view object) {
    return Diagnostic{
        .code = ErrorCode::incompatible_device,
        .message = std::string(object) + " belongs to a different OpenGL device/context",
    };
}

} // namespace vng::opengl::detail
