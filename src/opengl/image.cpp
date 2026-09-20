#include <vng/opengl/image.hpp>

#include <vng/opengl/device.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace vng::opengl {
namespace {

[[nodiscard]] GLenum gl_internal_format(ImageFormat format) noexcept {
    switch (format) {
    case ImageFormat::r8: return GL_R8;
    case ImageFormat::rgba8: return GL_RGBA8;
    case ImageFormat::srgb8_alpha8: return GL_SRGB8_ALPHA8;
    case ImageFormat::rgba16f: return GL_RGBA16F;
    case ImageFormat::rg32ui: return GL_RG32UI;
    case ImageFormat::rgba32f: return GL_RGBA32F;
    case ImageFormat::rgba32i: return GL_RGBA32I;
    case ImageFormat::rgba32ui: return GL_RGBA32UI;
    case ImageFormat::depth32f: return GL_DEPTH_COMPONENT32F;
    }
    return GL_NONE;
}

void delete_failed_image(GLuint handle) noexcept {
    if (handle != 0) {
        glDeleteTextures(1, &handle);
        detail::clear_gl_errors();
    }
}

// Reset every unpack control, including host PBO and byte swapping, then
// restore them on exit. Both byte and floating-point uploads use this path.
class UnpackState final {
public:
    UnpackState() {
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer_);
        for (std::size_t i = 0; i < names_.size(); ++i) {
            glGetIntegerv(names_[i], &values_[i]);
            glPixelStorei(names_[i], i == 0 ? 1 : 0);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    ~UnpackState() {
        for (std::size_t i = 0; i < names_.size(); ++i)
            glPixelStorei(names_[i], values_[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(buffer_));
    }
private:
    static constexpr std::array<GLenum, 8> names_{
        GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,
        GL_UNPACK_SKIP_PIXELS, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES,
        GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST,
    };
    std::array<GLint, names_.size()> values_{};
    GLint buffer_{};
};

GLint wrap_mode(gfx::ImageWrap wrap) {
    switch (wrap) {
    case gfx::ImageWrap::clamp_to_edge: return GL_CLAMP_TO_EDGE;
    case gfx::ImageWrap::repeat: return GL_REPEAT;
    case gfx::ImageWrap::mirrored_repeat: return GL_MIRRORED_REPEAT;
    }
    return 0;
}

GLint min_filter(const gfx::SamplerDesc& sampler) {
    if (sampler.min_filter != gfx::ImageFilter::nearest
        && sampler.min_filter != gfx::ImageFilter::linear) return 0;
    const bool linear = sampler.min_filter == gfx::ImageFilter::linear;
    switch (sampler.mip_filter) {
    case gfx::MipmapFilter::none: return linear ? GL_LINEAR : GL_NEAREST;
    case gfx::MipmapFilter::nearest:
        return linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    case gfx::MipmapFilter::linear:
        return linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    }
    return 0;
}

} // namespace

Image2D::Image2D(
    std::shared_ptr<detail::ContextState> state,
    std::uint32_t handle,
    std::uint32_t width,
    std::uint32_t height,
    ImageFormat format, u32 mip_levels) noexcept
    : state_(std::move(state)),
      handle_(handle),
      width_(width),
      height_(height),
      format_(format), mip_levels_(mip_levels) {}

Image2D::Image2D(Image2D&& other) noexcept
    : state_(std::move(other.state_)),
      handle_(std::exchange(other.handle_, 0)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)),
      format_(other.format_), mip_levels_(std::exchange(other.mip_levels_, 0)) {}

Image2D& Image2D::operator=(Image2D&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, 0);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
        format_ = other.format_;
        mip_levels_ = std::exchange(other.mip_levels_, 0);
    }
    return *this;
}

Image2D::~Image2D() {
    release_noexcept();
}

bool Image2D::belongs_to(const Device& device) const noexcept {
    return handle_ != 0 && state_ && state_.get() == device.state_.get();
}

std::expected<Image2D, Diagnostic> Image2D::create(
    const Device& device,
    std::uint32_t width,
    std::uint32_t height,
    ImageFormat format) {
    return create(device, gfx::ImageDesc{
        .extent = {width, height}, .format = format,
    });
}

std::expected<Image2D, Diagnostic> Image2D::create(
    const Device& device, const gfx::ImageDesc& description) {
    const auto [width, height] = description.extent;
    const auto format = description.format;
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Image2D::create received an empty device",
        });
    }
    if (width == 0 || height == 0
        || width > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || height > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::create requires non-zero GLsizei-sized dimensions",
        });
    }
    if (gl_internal_format(format) == GL_NONE) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::create received an unsupported format",
        });
    }
    if (description.mip_levels == 0
        || description.mip_levels > gfx::full_mip_count(description.extent)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::create received an invalid mip-level count",
        });
    }
    if (auto current = device.state_->require_current("Image2D::create"); !current) {
        return std::unexpected(std::move(current.error()));
    }

    GLint max_size = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_MAX_TEXTURE_SIZE)",
            [&] { glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size); });
        !queried) {
        return std::unexpected(std::move(queried.error()));
    }
    if (max_size <= 0
        || width > static_cast<std::uint32_t>(max_size)
        || height > static_cast<std::uint32_t>(max_size)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D dimensions exceed GL_MAX_TEXTURE_SIZE ("
                + std::to_string(max_size) + ')',
        });
    }

    GLuint handle = 0;
    if (auto created = detail::checked_gl_call(
            "glCreateTextures(GL_TEXTURE_2D)",
            [&] { glCreateTextures(GL_TEXTURE_2D, 1, &handle); },
            ErrorCode::object_creation_failed);
        !created) {
        delete_failed_image(handle);
        return std::unexpected(std::move(created.error()));
    }
    if (handle == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::object_creation_failed,
            .message = "glCreateTextures returned zero",
        });
    }

    if (auto allocated = detail::checked_gl_call(
            "glTextureStorage2D",
            [&] {
                glTextureStorage2D(
                    handle,
                    static_cast<GLsizei>(description.mip_levels),
                    gl_internal_format(format),
                    static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height));
            });
        !allocated) {
        delete_failed_image(handle);
        return std::unexpected(std::move(allocated.error()));
    }

    if (auto configured = detail::checked_gl_call(
            "glTextureParameteri(render-target defaults)",
            [&] {
                glTextureParameteri(handle, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTextureParameteri(handle, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTextureParameteri(handle, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTextureParameteri(handle, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                if (is_depth_format(format)) {
                    glTextureParameteri(
                        handle,
                        GL_TEXTURE_COMPARE_MODE,
                        GL_NONE);
                }
            });
        !configured) {
        delete_failed_image(handle);
        return std::unexpected(std::move(configured.error()));
    }

    Image2D image(device.state_, handle, width, height, format, description.mip_levels);
    if (auto configured = image.set_sampler(description.sampler); !configured)
        return std::unexpected(std::move(configured.error()));
    return image;
}

std::expected<void, Diagnostic> Image2D::bind_to_unit(std::uint32_t unit) const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::bind_to_unit called on an empty image",
        });
    }
    if (auto current = state_->require_current("Image2D::bind_to_unit"); !current) {
        return current;
    }

    GLint max_units = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS)",
            [&] {
                glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_units);
            });
        !queried) {
        return queried;
    }
    if (max_units <= 0 || unit >= static_cast<std::uint32_t>(max_units)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D texture unit exceeds GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS",
        });
    }
    return detail::checked_gl_call(
        "glBindTextureUnit",
        [&] { glBindTextureUnit(unit, handle_); });
}

std::expected<void, Diagnostic> Image2D::clear_rgba8(
    const std::array<float, 4>& color) const {
    if (handle_ == 0 || !state_ || (format_ != ImageFormat::rgba8
        && format_ != ImageFormat::srgb8_alpha8)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rgba8 requires a non-empty rgba8 image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rgba8"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rgba8)",
        [&] { glClearTexImage(handle_, 0, GL_RGBA, GL_FLOAT, color.data()); });
}

std::expected<void, Diagnostic> Image2D::write_r8(
    u32 x, u32 y, u32 width, u32 height, std::span<const std::byte> pixels)
{
    if (!handle_ || !state_ || format_ != ImageFormat::r8
        || x > width_ || width > width_ - x
        || y > height_ || height > height_ - y
        || static_cast<u64>(width) * height != pixels.size()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::write_r8 requires a valid r8 rectangle and tightly packed pixels",
        });
    }
    if (auto current = state_->require_current("Image2D::write_r8"); !current) return current;
    if (width == 0 || height == 0) return {};
    return detail::checked_gl_call("upload r8 coverage", [&] {
        UnpackState unpack;
        glTextureSubImage2D(handle_, 0, static_cast<GLint>(x), static_cast<GLint>(y),
            static_cast<GLsizei>(width), static_cast<GLsizei>(height),
            GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    });
}

std::expected<void, Diagnostic> Image2D::clear_r8(u8 value) const
{
    if (!handle_ || !state_ || format_ != ImageFormat::r8) {
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_r8 requires an r8 image"});
    }
    if (auto current = state_->require_current("Image2D::clear_r8"); !current) return current;
    return detail::checked_gl_call("clear coverage atlas", [&] {
        glClearTexImage(handle_, 0, GL_RED, GL_UNSIGNED_BYTE, &value);
    });
}

std::expected<void, Diagnostic> Image2D::use_linear_filtering() const
{
    return set_sampler({.min_filter = gfx::ImageFilter::linear,
        .mag_filter = gfx::ImageFilter::linear});
}

std::expected<void, Diagnostic> Image2D::set_sampler(const gfx::SamplerDesc& sampler) const
{
    const auto min = min_filter(sampler);
    const auto mag = sampler.mag_filter == gfx::ImageFilter::linear ? GL_LINEAR
        : sampler.mag_filter == gfx::ImageFilter::nearest ? GL_NEAREST : 0;
    const auto wrap_u = wrap_mode(sampler.wrap_u);
    const auto wrap_v = wrap_mode(sampler.wrap_v);
    if (!handle_ || !state_ || !min || !mag || !wrap_u || !wrap_v
        || (gfx::is_integer_format(format_)
            && (sampler.min_filter != gfx::ImageFilter::nearest
                || sampler.mag_filter != gfx::ImageFilter::nearest
                || sampler.mip_filter == gfx::MipmapFilter::linear))) {
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "Image2D::set_sampler received invalid or format-incompatible filtering/wrap"});
    }
    if (auto current = state_->require_current("Image2D::set_sampler"); !current)
        return current;
    return detail::checked_gl_call("set image sampler", [&] {
        glTextureParameteri(handle_, GL_TEXTURE_MIN_FILTER, min);
        glTextureParameteri(handle_, GL_TEXTURE_MAG_FILTER, mag);
        glTextureParameteri(handle_, GL_TEXTURE_WRAP_S, wrap_u);
        glTextureParameteri(handle_, GL_TEXTURE_WRAP_T, wrap_v);
        glTextureParameteri(handle_, GL_TEXTURE_BASE_LEVEL, 0);
        glTextureParameteri(handle_, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(mip_levels_ - 1));
    });
}

std::expected<void, Diagnostic> Image2D::generate_mipmaps() const
{
    if (!handle_ || !state_ || gfx::is_integer_format(format_) || is_depth_format(format_))
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "Image2D::generate_mipmaps requires a normalized or floating color image"});
    if (auto current = state_->require_current("Image2D::generate_mipmaps"); !current)
        return current;
    if (mip_levels_ <= 1) return {};
    return detail::checked_gl_call("glGenerateTextureMipmap", [&] { glGenerateTextureMipmap(handle_); });
}

std::expected<void, Diagnostic> Image2D::write_rgba8(
    u32 x, u32 y, u32 width, u32 height, std::span<const std::byte> pixels)
{
    if (!handle_ || !state_ || (format_ != ImageFormat::rgba8 && format_ != ImageFormat::srgb8_alpha8)
        || x > width_ || width > width_ - x || y > height_ || height > height_ - y
        || static_cast<u64>(width) * height * 4 != pixels.size())
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "Image2D::write_rgba8 requires a valid RGBA8 rectangle and tightly packed pixels"});
    if (auto current = state_->require_current("Image2D::write_rgba8"); !current) return current;
    if (width == 0 || height == 0) return {};
    return detail::checked_gl_call("upload rgba8 pixels", [&] {
        UnpackState unpack;
        glTextureSubImage2D(handle_, 0, static_cast<GLint>(x), static_cast<GLint>(y),
            static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    });
}

std::expected<void, Diagnostic> Image2D::write_rgba32f(
    u32 x, u32 y, u32 width, u32 height, std::span<const float> components)
{
    if (!handle_ || !state_ || (format_ != ImageFormat::rgba16f && format_ != ImageFormat::rgba32f)
        || x > width_ || width > width_ - x || y > height_ || height > height_ - y
        || static_cast<u64>(width) * height * 4 != components.size())
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "Image2D::write_rgba32f requires a valid floating RGBA rectangle and tightly packed components"});
    if (auto current = state_->require_current("Image2D::write_rgba32f"); !current) return current;
    if (width == 0 || height == 0) return {};
    return detail::checked_gl_call("upload floating rgba pixels", [&] {
        UnpackState unpack;
        glTextureSubImage2D(handle_, 0, static_cast<GLint>(x), static_cast<GLint>(y),
            static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_FLOAT, components.data());
    });
}

std::expected<void, Diagnostic> Image2D::clear_rg32ui(
    const std::array<std::uint32_t, 2>& value) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::rg32ui) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rg32ui requires a non-empty rg32ui image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rg32ui"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rg32ui)",
        [&] {
            glClearTexImage(
                handle_, 0, GL_RG_INTEGER, GL_UNSIGNED_INT, value.data());
        });
}

std::expected<void, Diagnostic> Image2D::clear_rgba32f(
    const std::array<float, 4>& value) const {
    if (handle_ == 0 || !state_ || (format_ != ImageFormat::rgba32f && format_ != ImageFormat::rgba16f)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rgba32f requires a non-empty rgba32f image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rgba32f"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rgba32f)",
        [&] { glClearTexImage(handle_, 0, GL_RGBA, GL_FLOAT, value.data()); });
}

std::expected<void, Diagnostic> Image2D::clear_rgba32i(
    const std::array<std::int32_t, 4>& value) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::rgba32i) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rgba32i requires a non-empty rgba32i image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rgba32i"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rgba32i)",
        [&] { glClearTexImage(handle_, 0, GL_RGBA_INTEGER, GL_INT, value.data()); });
}

std::expected<void, Diagnostic> Image2D::clear_rgba32ui(
    const std::array<std::uint32_t, 4>& value) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::rgba32ui) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rgba32ui requires a non-empty rgba32ui image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rgba32ui"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rgba32ui)",
        [&] {
            glClearTexImage(
                handle_, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, value.data());
        });
}

std::expected<void, Diagnostic> Image2D::clear_depth32f(float depth) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::depth32f) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_depth32f requires a non-empty depth32f image",
        });
    }
    if (!std::isfinite(depth) || depth < 0.0F || depth > 1.0F) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_depth32f requires a finite value in [0, 1]",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_depth32f"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(depth32f)",
        [&] {
            glClearTexImage(
                handle_, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        });
}

std::expected<void, Diagnostic> Image2D::destroy() {
    if (handle_ == 0) {
        return {};
    }
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Image2D::destroy has no device state",
        });
    }
    if (auto current = state_->require_current("Image2D::destroy"); !current) {
        return current;
    }
    const GLuint handle = handle_;
    glDeleteTextures(1, &handle);
    handle_ = 0;
    width_ = 0;
    height_ = 0;
    mip_levels_ = 0;
    state_.reset();
    return {};
}

void Image2D::release_noexcept() noexcept {
    if (handle_ == 0) {
        return;
    }
    if (state_ && state_->is_current()) {
        const GLuint handle = handle_;
        glDeleteTextures(1, &handle);
    } else if (state_) {
        state_->record_lifecycle_failure(
            "Image2D destroyed without its owning context current; native handle was leaked safely");
    }
    handle_ = 0;
    width_ = 0;
    height_ = 0;
    mip_levels_ = 0;
    state_.reset();
}

std::expected<Image2D, Diagnostic> make_backend_image(
    const Device& device, const gfx::ImageDesc& description)
{
    return Image2D::create(device, description);
}

std::expected<Image2D, Diagnostic> upload_backend_image(
    const Device& device, const gfx::ImageData& data, const gfx::ImageUploadOptions& options)
{
    if (data.extent.empty() || static_cast<u64>(data.extent.width) * data.extent.height
            > std::numeric_limits<std::size_t>::max() / 4
        || static_cast<u64>(data.extent.width) * data.extent.height * 4 != data.pixels.size()
        || (options.format != ImageFormat::rgba8 && options.format != ImageFormat::srgb8_alpha8))
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "upload_image requires tightly packed RGBA8 pixels and rgba8 or srgb8_alpha8 storage"});
    auto image = Image2D::create(device, gfx::ImageDesc{
        .extent = data.extent, .format = options.format,
        .mip_levels = options.generate_mipmaps ? gfx::full_mip_count(data.extent) : 1,
        .sampler = options.sampler,
    });
    if (!image) return image;
    if (auto uploaded = image->write_rgba8(0, 0, data.extent.width, data.extent.height, data.pixels); !uploaded)
        return std::unexpected(std::move(uploaded.error()));
    if (options.generate_mipmaps) {
        if (auto generated = image->generate_mipmaps(); !generated)
            return std::unexpected(std::move(generated.error()));
    }
    return image;
}

} // namespace vng::opengl
