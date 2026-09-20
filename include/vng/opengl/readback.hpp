#pragma once
#include <vng/gfx/image.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <expected>
#include <memory>
#include <optional>

namespace vng::opengl {
class Device;
class Framebuffer;
struct Rgba8Readback {
    u64 id{};
    gfx::ImageData image; // Tightly packed, top-left origin; no color conversion.
};

// Context-owned, bounded asynchronous GPU -> CPU transfer. No wait-for-GPU API.
// Three persistently mapped staging buffers; only signalled buffers are read.
// A full queue returns false, never waits or allocates more slots.
class Rgba8ReadbackQueue final {
public:
    static constexpr std::size_t capacity = 3;
    static std::expected<Rgba8ReadbackQueue, Diagnostic> create(const Device&);
    Rgba8ReadbackQueue(Rgba8ReadbackQueue&&) noexcept;
    Rgba8ReadbackQueue& operator=(Rgba8ReadbackQueue&&) noexcept;
    ~Rgba8ReadbackQueue();
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool pending() const noexcept;
    // IDs must increase; caller retains per-image metadata under that ID.
    std::expected<bool, Diagnostic> try_submit(const Framebuffer&, u32 attachment,
                                                Extent2D, u64 id);
    // Latest completed image wins. Older completed images are discarded without
    // CPU copies. Empty means not ready. Poll uses zero-timeout fence checks.
    std::expected<std::optional<Rgba8Readback>, Diagnostic> try_take();
    // Invalidates results without waiting; pending GPU storage is reclaimed by polling.
    void discard() noexcept;
private:
    struct Impl;
    explicit Rgba8ReadbackQueue(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace vng::opengl
