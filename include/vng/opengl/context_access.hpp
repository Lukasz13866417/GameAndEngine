#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <vng/render/color.hpp>

namespace vng::opengl {

// Deliberately contains no GLFW types. A window-system backend supplies these
// callbacks after making one of its contexts current.
using ProcedureAddress = void (*)();
using ProcedureResolver = ProcedureAddress (*)(const char* name);
using CurrentContextQuery = bool (*)(const void* native_identity) noexcept;

class ContextLifetime final {
public:
    explicit ContextLifetime(const void* native_identity) noexcept
        : native_identity_(native_identity) {}

    [[nodiscard]] const void* native_identity() const noexcept {
        return native_identity_;
    }

    [[nodiscard]] bool alive() const noexcept {
        return alive_.load(std::memory_order_acquire);
    }

    void invalidate() noexcept {
        alive_.store(false, std::memory_order_release);
    }

    // A native API can retain raw callback user pointers until the context is
    // destroyed. Window backends therefore keep callback state alive with the
    // context token, even if the Device that installed the callback goes away
    // while this context is not current.
    void retain(std::shared_ptr<void> resource) {
        std::lock_guard lock(retained_mutex_);
        retained_.push_back(std::move(resource));
    }

private:
    const void* native_identity_{};
    std::atomic_bool alive_{true};
    std::mutex retained_mutex_;
    std::vector<std::shared_ptr<void>> retained_;
};

struct CurrentContextAccess final {
    std::shared_ptr<ContextLifetime> lifetime;
    ProcedureResolver resolve{};
    CurrentContextQuery is_current{};
    std::thread::id owner_thread{};

    // A context integration may require that Device observe a particular
    // physical default-framebuffer encoding. This is not a capability claim:
    // Device always queries OpenGL and rejects a mismatch. Providers without
    // a creation-time requirement leave it empty.
    std::optional<render::ColorEncoding>
        required_default_framebuffer_encoding;

    [[nodiscard]] bool valid() const noexcept {
        return lifetime && lifetime->alive() && lifetime->native_identity() != nullptr
            && resolve != nullptr && is_current != nullptr
            && owner_thread != std::thread::id{};
    }
};

} // namespace vng::opengl
