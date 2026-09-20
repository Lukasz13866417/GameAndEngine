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
#include <vng/opengl/graphics_state.hpp>

namespace vng::opengl { class Program; }
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

    // One frame-owned logical command state, stored alongside native context
    // state to avoid an allocation per frame. All command handles borrow it;
    // acquiring or destroying a handle never replaces this authority.
    const Program* command_program{};
    bool command_view_ready{};

    // Desired state of the active frame. Handles carry the generation, not a
    // pointer into Frame/Commands, and cannot access a later frame's state.
    render::DepthState graphics_depth{};
    render::CullMode graphics_cull{render::CullMode::none};
    render::FrontFace graphics_front_face{render::FrontFace::counter_clockwise};
    PolygonMode graphics_polygon{PolygonMode::fill};
    render::BlendMode graphics_blend{render::BlendMode::disabled};
    bool graphics_synchronized{};
    u64 graphics_generation{};

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
        command_program = nullptr;
        command_view_ready = false;
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
        return ended;
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
