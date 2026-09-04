#pragma once

#include <expected>

#include <vng/opengl/commands.hpp>
#include <vng/opengl/device.hpp>
#include <vng/render/frame.hpp>

namespace vng::opengl {

// A target-scoped OpenGL frame. It retains only a lightweight Device facade
// sharing the context state; persistent resources continue to belong to the
// backend renderer. While active, the value proves that target selection,
// color encoding, viewport, and requested clears have happened for this
// render extent. A context admits one active Frame at a time.
class Frame final {
public:
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&& other) noexcept;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    ~Frame();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] Extent2D extent() const noexcept { return extent_; }
    [[nodiscard]] render::ColorEncoding color_encoding() const noexcept
    {
        return color_encoding_;
    }

    // Frame retains a lightweight facade for its context. Access therefore
    // remains safe on a moved-from Frame: backend calls receive an empty
    // Device and return a diagnostic instead of dereferencing a dangling
    // pointer. The native context itself must still outlive the frame.
    [[nodiscard]] const Device& device() const noexcept { return device_; }

    [[nodiscard]] bool belongs_to(const Device& device) const noexcept;

    // Starts a fresh backend-specific command stream. Acquiring another stream
    // from this Frame revokes the previous one, which prevents two stateful
    // OpenGL recorders from making contradictory assumptions.
    [[nodiscard]] Commands commands() noexcept;

    // Ending validates the context and closes its generation-owned logical
    // recording scope. A stale or moved-from value cannot close a newer frame.
    // It deliberately does not stall, submit, or present: immediate-mode OpenGL
    // has already issued the work, and presentation belongs to the window/
    // context integration. Destruction also closes the C++ scope implicitly.
    [[nodiscard]] std::expected<void, Diagnostic> end();

private:
    Frame(
        const Device& device,
        Extent2D extent,
        render::ColorEncoding color_encoding,
        u64 generation) noexcept
        : device_(device.state_),
          extent_(extent),
          color_encoding_(color_encoding),
          generation_(generation) {}

    [[nodiscard]] static std::expected<Frame, Diagnostic> acquire(
        const Device& device,
        Extent2D extent,
        render::ColorEncoding color_encoding);
    void release_noexcept() noexcept;

    Device device_;
    Extent2D extent_{};
    render::ColorEncoding color_encoding_{render::ColorEncoding::srgb};
    u64 generation_{};

    [[nodiscard]] u64 renew_command_stream() noexcept;

    friend std::expected<Frame, Diagnostic> begin_backend_frame(
        Device&, render::DefaultTarget, const render::FrameDesc&);
};

[[nodiscard]] std::expected<Frame, Diagnostic> begin_backend_frame(
    Device& device,
    render::DefaultTarget,
    const render::FrameDesc& description);

static_assert(render::Frame<Frame>);

} // namespace vng::opengl
