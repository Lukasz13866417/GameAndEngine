#include "presentation.hpp"

#include <glad/gl.h>
#include <png.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace example {
namespace {
using namespace vng;
using resources::Result;

resources::Diagnostic invalid(std::string message)
{ return {.code = resources::ErrorCode::invalid_argument, .message = std::move(message)}; }

resources::Diagnostic failure(std::string message)
{ return {.code = resources::ErrorCode::operation_failed, .message = std::move(message)}; }

template<class Function>
Result<void> checked_gl(std::string_view operation, Function&& function)
{
    while (glGetError() != GL_NO_ERROR) {}
    std::forward<Function>(function)();
    const auto error = glGetError();
    if (error == GL_NO_ERROR) return {};
    while (glGetError() != GL_NO_ERROR) {}
    return std::unexpected(resources::to_diagnostic(opengl::Diagnostic{
        .code = opengl::ErrorCode::operation_failed,
        .message = std::string(operation) + " failed with OpenGL error " + std::to_string(error)}));
}

// Restore source/default framebuffer selections as well as current bindings.
class CopyScope final {
public:
    static Result<CopyScope> capture(GLuint source)
    {
        CopyScope scope;
        scope.source_ = source;
        GLint maximum{};
        auto captured = checked_gl("capture display-copy state", [&] {
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &scope.read_framebuffer_);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &scope.draw_framebuffer_);
            scope.srgb_ = glIsEnabled(GL_FRAMEBUFFER_SRGB);
            scope.scissor_ = glIsEnabledi(GL_SCISSOR_TEST, 0);
            glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maximum);
        });
        if (!captured) return std::unexpected(std::move(captured.error()));
        if (maximum <= 0 || maximum > 1024)
            return std::unexpected(invalid("Invalid OpenGL draw-buffer limit"));
        scope.draw_buffers_.resize(static_cast<std::size_t>(maximum));
        scope.active_ = true;
        auto selected = checked_gl("capture display framebuffer selections", [&] {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
            glGetIntegerv(GL_READ_BUFFER, &scope.source_read_buffer_);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            for (GLint i = 0; i < maximum; ++i) {
                GLint buffer{};
                glGetIntegerv(GL_DRAW_BUFFER0 + static_cast<GLenum>(i), &buffer);
                scope.draw_buffers_[static_cast<std::size_t>(i)] = static_cast<GLenum>(buffer);
            }
        });
        if (!selected) return std::unexpected(std::move(selected.error()));
        scope.selections_captured_ = true;
        return scope;
    }
    CopyScope(CopyScope&& other) noexcept
        : source_(other.source_), read_framebuffer_(other.read_framebuffer_),
          draw_framebuffer_(other.draw_framebuffer_), source_read_buffer_(other.source_read_buffer_),
          draw_buffers_(std::move(other.draw_buffers_)), srgb_(other.srgb_), scissor_(other.scissor_),
          active_(std::exchange(other.active_, false)), selections_captured_(other.selections_captured_) {}
    ~CopyScope() { if (active_) restore_native(); }

    Result<void> copy(Extent2D extent)
    {
        GLint samples{};
        GLboolean double_buffered{};
        auto queried = checked_gl("validate window framebuffer", [&] {
            glGetIntegerv(GL_SAMPLES, &samples);
            glGetBooleanv(GL_DOUBLEBUFFER, &double_buffered);
        });
        if (!queried) return queried;
        if (samples != 0 || !double_buffered)
            return std::unexpected(invalid("Display copy requires a single-sample, double-buffered window"));
        return checked_gl("copy encoded display surface to window", [&] {
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glDrawBuffer(GL_BACK);
            glDisablei(GL_SCISSOR_TEST, 0);
            // Disabling conversion prevents both source sRGB decoding and
            // destination encoding: the already-encoded bytes remain intact.
            glDisable(GL_FRAMEBUFFER_SRGB);
            const auto width = static_cast<GLint>(extent.width);
            const auto height = static_cast<GLint>(extent.height);
            glBlitNamedFramebuffer(source_, 0, 0, 0, width, height,
                0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        });
    }
    Result<void> restore()
    {
        if (!active_) return {};
        active_ = false;
        return checked_gl("restore display-copy state", [&] { restore_native(); });
    }
private:
    CopyScope() = default;
    void restore_native() const
    {
        if (selections_captured_) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, source_);
            glReadBuffer(static_cast<GLenum>(source_read_buffer_));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            const auto multiple = std::any_of(draw_buffers_.begin() + 1, draw_buffers_.end(),
                [](GLenum buffer) { return buffer != GL_NONE; });
            if (multiple) glDrawBuffers(static_cast<GLsizei>(draw_buffers_.size()), draw_buffers_.data());
            else glDrawBuffer(draw_buffers_.front());
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_framebuffer_));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_framebuffer_));
        if (srgb_) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
        if (scissor_) glEnablei(GL_SCISSOR_TEST, 0); else glDisablei(GL_SCISSOR_TEST, 0);
    }
    GLuint source_{};
    GLint read_framebuffer_{}, draw_framebuffer_{}, source_read_buffer_{GL_COLOR_ATTACHMENT0};
    std::vector<GLenum> draw_buffers_;
    GLboolean srgb_{}, scissor_{};
    bool active_{}, selections_captured_{};
};

Result<void> make_parent(const std::filesystem::path& path)
{
    if (path.empty()) return std::unexpected(failure("Output path is empty"));
    if (!path.has_parent_path()) return {};
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return std::unexpected(failure("Cannot create output directory: " + error.message()));
    return {};
}

class ReadbackScope final {
public:
    static Result<ReadbackScope> capture()
    {
        ReadbackScope scope;
        auto captured = checked_gl("capture screenshot readback state", [&] {
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &scope.framebuffer_);
            glGetIntegerv(GL_READ_BUFFER, &scope.read_buffer_);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &scope.pack_buffer_);
            for (std::size_t i = 0; i < parameters.size(); ++i)
                glGetIntegerv(parameters[i], &scope.pack_[i]);
        });
        if (!captured) return std::unexpected(std::move(captured.error()));
        scope.active_ = true;
        // GL_READ_BUFFER is per-framebuffer. Remember the default buffer's
        // setting separately when the caller had an offscreen target bound.
        auto default_state = checked_gl("capture default framebuffer read selection", [&] {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glGetIntegerv(GL_READ_BUFFER, &scope.default_read_buffer_);
        });
        if (!default_state) return std::unexpected(std::move(default_state.error()));
        return scope;
    }
    ReadbackScope(ReadbackScope&& other) noexcept
        : framebuffer_(other.framebuffer_), read_buffer_(other.read_buffer_),
          default_read_buffer_(other.default_read_buffer_), pack_buffer_(other.pack_buffer_),
          pack_(other.pack_), active_(std::exchange(other.active_, false)) {}
    ~ReadbackScope() { if (active_) restore_native(); }

    Result<void> read(Extent2D extent, std::span<u8> pixels)
    {
        return checked_gl("read final default back buffer", [&] {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            for (std::size_t i = 0; i < parameters.size(); ++i)
                glPixelStorei(parameters[i], parameters[i] == GL_PACK_ALIGNMENT ? 1 : 0);
            glReadPixels(0, 0, static_cast<GLsizei>(extent.width), static_cast<GLsizei>(extent.height),
                GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        });
    }
    Result<void> restore()
    {
        if (!active_) return {};
        active_ = false;
        return checked_gl("restore screenshot readback state", [&] { restore_native(); });
    }
private:
    ReadbackScope() = default;
    void restore_native() const
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(static_cast<GLenum>(default_read_buffer_));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(framebuffer_));
        glReadBuffer(static_cast<GLenum>(read_buffer_));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer_));
        for (std::size_t i = 0; i < parameters.size(); ++i)
            glPixelStorei(parameters[i], pack_[i]);
    }
    static constexpr std::array<GLenum, 8> parameters{
        GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS,
        GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES, GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST};
    GLint framebuffer_{}, read_buffer_{GL_BACK}, default_read_buffer_{GL_BACK}, pack_buffer_{};
    std::array<GLint, parameters.size()> pack_{};
    bool active_{};
};
} // namespace

Result<DisplaySurface> DisplaySurface::create(opengl::Device& device, Extent2D extent, bool depth)
{
    auto target = opengl::RenderTarget::create(device,
        {.color = gfx::ImageFormat::srgb8_alpha8, .depth = depth}, extent);
    if (!target) return std::unexpected(resources::to_diagnostic(std::move(target.error())));
    return DisplaySurface{std::move(*target)};
}

Result<void> DisplaySurface::resize(opengl::Device& device, Extent2D extent)
{ return target_.resize(device, extent); }

Result<void> DisplaySurface::copy_to_window(const opengl::Device& device) const
{
    if (!target_.belongs_to(device))
        return std::unexpected(invalid("Display surface belongs to another OpenGL context"));
    if (auto allowed = device.require_resource_update("DisplaySurface::copy_to_window"); !allowed)
        return std::unexpected(resources::to_diagnostic(std::move(allowed.error())));
    if (!device.default_framebuffer_capabilities().supports(render::ColorEncoding::linear))
        return std::unexpected(invalid("Display surface byte copy expects a linear default framebuffer"));
    const auto extent = target_.extent();
    if (extent.empty() || extent.width > static_cast<u32>(std::numeric_limits<GLint>::max())
        || extent.height > static_cast<u32>(std::numeric_limits<GLint>::max()))
        return std::unexpected(invalid("Display surface has an invalid extent"));
    auto scope = CopyScope::capture(target_.framebuffer().native_handle());
    if (!scope) return std::unexpected(std::move(scope.error()));
    if (auto copied = scope->copy(extent); !copied) return copied;
    return scope->restore();
}

// Exclusive creation prevents accidental replacement, including a race with
// another process. A failed write may leave a partial NEW file for inspection.
Result<void> write_rgba8_png(const std::filesystem::path& path, Extent2D extent,
    std::span<const u8> rgba)
{
    if (auto parent = make_parent(path); !parent) return parent;
    auto* file = std::fopen(path.c_str(), "wbx");
    if (!file) return std::unexpected(failure("Cannot create " + path.string()
        + " (existing files are not overwritten): " + std::strerror(errno)));
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = extent.width;
    image.height = extent.height;
    image.format = PNG_FORMAT_RGBA;
    const auto success = png_image_write_to_stdio(&image, file, 0, rgba.data(), 0, nullptr);
    const auto message = std::string(image.message);
    png_image_free(&image);
    const auto closed = std::fclose(file);
    if (!success || closed != 0)
        return std::unexpected(failure("Writing " + path.string() + " failed: " + message));
    return {};
}

Result<gfx::ImageData> capture_screenshot(const opengl::Device& device, Extent2D extent)
{
    if (auto current = device.require_current("save_screenshot"); !current)
        return std::unexpected(resources::to_diagnostic(std::move(current.error())));
    if (extent.empty() || extent.width > 16384 || extent.height > 16384
        || static_cast<u64>(extent.width) * extent.height > 64'000'000)
        return std::unexpected(failure("Screenshot extent is empty or exceeds the 64-megapixel limit"));
    std::vector<u8> pixels(static_cast<std::size_t>(extent.width) * extent.height * 4);
    auto scope = ReadbackScope::capture();
    if (!scope) return std::unexpected(std::move(scope.error()));
    if (auto read = scope->read(extent, pixels); !read) return std::unexpected(read.error());
    if (auto restored = scope->restore(); !restored) return std::unexpected(restored.error());
    // OpenGL's rows begin at the bottom; PNG and analysis images begin at top.
    const auto row_bytes = static_cast<std::size_t>(extent.width) * 4;
    for (u32 y = 0; y < extent.height / 2; ++y) {
        auto* first = pixels.data() + static_cast<std::size_t>(y) * row_bytes;
        auto* last = pixels.data() + static_cast<std::size_t>(extent.height - y - 1) * row_bytes;
        std::swap_ranges(first, first + row_bytes, last);
    }
    const auto bytes = std::as_bytes(std::span{pixels});
    return gfx::ImageData{extent, {bytes.begin(), bytes.end()}};
}

Result<void> save_screenshot(const opengl::Device& device, Extent2D extent,
    const std::filesystem::path& destination)
{
    auto image = capture_screenshot(device, extent);
    if (!image) return std::unexpected(image.error());
    return write_rgba8_png(destination, extent,
        {reinterpret_cast<const u8*>(image->pixels.data()), image->pixels.size()});
}

} // namespace example
