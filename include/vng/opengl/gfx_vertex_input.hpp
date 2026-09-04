#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <vng/gfx/format.hpp>
#include <vng/gfx/vertex_layout.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/vertex_array.hpp>

namespace vng::opengl {

class Buffer;

// Associates the backend-neutral stream binding number with an uploaded GL
// buffer. base_offset permits several streams to share one allocation.
struct ResolvedStreamBuffer final {
    std::uint32_t binding{};
    const Buffer* buffer{};
    std::size_t base_offset{};
};

[[nodiscard]] std::expected<VertexFormat, Diagnostic> translate_vertex_format(
    const gfx::VertexFormat& format);

[[nodiscard]] std::expected<void, Diagnostic> configure_vertex_input(
    VertexArray& vertex_array,
    const gfx::ResolvedVertexInput& input,
    std::span<const ResolvedStreamBuffer> stream_buffers);

} // namespace vng::opengl
