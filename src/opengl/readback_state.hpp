#pragma once
#include <glad/gl.h>

namespace vng::opengl::detail {
class ReadbackState final {
public:
    explicit ReadbackState(GLuint framebuffer) noexcept {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_framebuffer_);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pixel_pack_buffer_);
        glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment_);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &previous_pack_row_length_);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &previous_pack_skip_pixels_);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &previous_pack_skip_rows_);
        glGetIntegerv(GL_PACK_IMAGE_HEIGHT, &previous_pack_image_height_);
        glGetIntegerv(GL_PACK_SKIP_IMAGES, &previous_pack_skip_images_);
        glGetIntegerv(GL_PACK_SWAP_BYTES, &previous_pack_swap_bytes_);
        glGetIntegerv(GL_PACK_LSB_FIRST, &previous_pack_lsb_first_);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glGetIntegerv(GL_READ_BUFFER, &framebuffer_read_buffer_);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
    }

    ReadbackState(const ReadbackState&) = delete;
    ReadbackState& operator=(const ReadbackState&) = delete;

    ~ReadbackState() {
        glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment_);
        glPixelStorei(GL_PACK_ROW_LENGTH, previous_pack_row_length_);
        glPixelStorei(GL_PACK_SKIP_PIXELS, previous_pack_skip_pixels_);
        glPixelStorei(GL_PACK_SKIP_ROWS, previous_pack_skip_rows_);
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, previous_pack_image_height_);
        glPixelStorei(GL_PACK_SKIP_IMAGES, previous_pack_skip_images_);
        glPixelStorei(GL_PACK_SWAP_BYTES, previous_pack_swap_bytes_);
        glPixelStorei(GL_PACK_LSB_FIRST, previous_pack_lsb_first_);
        glBindBuffer(
            GL_PIXEL_PACK_BUFFER,
            static_cast<GLuint>(previous_pixel_pack_buffer_));
        glReadBuffer(static_cast<GLenum>(framebuffer_read_buffer_));
        glBindFramebuffer(
            GL_READ_FRAMEBUFFER,
            static_cast<GLuint>(previous_read_framebuffer_));
    }

private:
    GLint previous_read_framebuffer_{};
    GLint previous_pixel_pack_buffer_{};
    GLint previous_pack_alignment_{};
    GLint previous_pack_row_length_{};
    GLint previous_pack_skip_pixels_{};
    GLint previous_pack_skip_rows_{};
    GLint previous_pack_image_height_{};
    GLint previous_pack_skip_images_{};
    GLint previous_pack_swap_bytes_{};
    GLint previous_pack_lsb_first_{};
    GLint framebuffer_read_buffer_{};
};
} // namespace vng::opengl::detail
