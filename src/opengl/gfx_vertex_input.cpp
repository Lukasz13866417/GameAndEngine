#include <vng/opengl/gfx_vertex_input.hpp>

#include <vng/opengl/buffer.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vng::opengl {

std::expected<VertexFormat, Diagnostic> translate_vertex_format(
    const gfx::VertexFormat& format) {
    if (!gfx::valid_vertex_format(format)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "gfx::VertexFormat has an invalid physical encoding descriptor",
        });
    }

    VertexComponent component{};
    switch (format.encoding) {
    case gfx::ComponentEncoding::Float16:
        component = VertexComponent::float16;
        break;
    case gfx::ComponentEncoding::Float32:
        component = VertexComponent::float32;
        break;
    case gfx::ComponentEncoding::Signed8:
        component = VertexComponent::sint8;
        break;
    case gfx::ComponentEncoding::Unsigned8:
        component = VertexComponent::uint8;
        break;
    case gfx::ComponentEncoding::Signed32:
        component = VertexComponent::sint32;
        break;
    case gfx::ComponentEncoding::Unsigned32:
        component = VertexComponent::uint32;
        break;
    case gfx::ComponentEncoding::Signed2_10_10_10Rev:
        component = VertexComponent::sint_2_10_10_10_rev;
        break;
    }

    return VertexFormat{
        .component = component,
        .component_count = format.component_count,
        .normalized = format.interpretation == gfx::AttributeInterpretation::Normalized,
        .delivery = format.interpretation == gfx::AttributeInterpretation::Integer
            ? VertexDelivery::integer
            : VertexDelivery::floating,
    };
}

std::expected<void, Diagnostic> configure_vertex_input(
    VertexArray& vertex_array,
    const gfx::ResolvedVertexInput& input,
    std::span<const ResolvedStreamBuffer> stream_buffers) {
    std::unordered_map<std::uint32_t, const ResolvedStreamBuffer*> supplied;
    for (const auto& stream : stream_buffers) {
        if (stream.buffer == nullptr || stream.binding >= input.stream_count ||
            !supplied.emplace(stream.binding, &stream).second) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "configure_vertex_input received a null buffer, out-of-range binding, or duplicate stream binding",
            });
        }
    }

    struct BindingShape final {
        std::size_t stride{};
        std::uint32_t divisor{};
    };
    std::unordered_map<std::uint32_t, BindingShape> shapes;
    std::vector<VertexAttribute> attributes;
    attributes.reserve(input.attributes.size());

    for (const auto& attribute : input.attributes) {
        const auto supplied_stream = supplied.find(attribute.binding);
        if (supplied_stream == supplied.end()) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "configure_vertex_input is missing a buffer for resolved binding "
                    + std::to_string(attribute.binding),
            });
        }
        if (attribute.binding >= input.stream_count ||
            !gfx::valid_vertex_format(attribute.format) ||
            attribute.stride == 0
            || attribute.stride > std::numeric_limits<std::uint32_t>::max()
            || attribute.offset > std::numeric_limits<std::uint32_t>::max()
            || attribute.format.byte_size > attribute.stride
            || attribute.offset > attribute.stride - attribute.format.byte_size) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "Resolved vertex attribute has an invalid binding, format, stride, or field extent",
            });
        }
        if (attribute.offset % attribute.format.byte_alignment != 0 ||
            supplied_stream->second->base_offset % attribute.format.byte_alignment != 0) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "Resolved vertex field offset and stream base offset must satisfy the format alignment",
            });
        }
        const auto [shape, inserted] = shapes.emplace(
            attribute.binding,
            BindingShape{attribute.stride, attribute.divisor});
        if (!inserted && (shape->second.stride != attribute.stride
                || shape->second.divisor != attribute.divisor)) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "Resolved attributes disagree about their shared stream shape",
            });
        }
        auto format = translate_vertex_format(attribute.format);
        if (!format) {
            return std::unexpected(std::move(format.error()));
        }
        attributes.push_back(VertexAttribute{
            .location = attribute.location,
            .binding = attribute.binding,
            .relative_offset = static_cast<std::uint32_t>(attribute.offset),
            .format = *format,
        });
    }

    std::vector<VertexBufferBinding> bindings;
    bindings.reserve(shapes.size());
    for (const auto& [binding, shape] : shapes) {
        const auto* supplied_stream = supplied.at(binding);
        bindings.push_back(VertexBufferBinding{
            .binding = binding,
            .buffer = supplied_stream->buffer,
            .offset = supplied_stream->base_offset,
            .stride = static_cast<std::uint32_t>(shape.stride),
            .divisor = shape.divisor,
        });
    }
    std::ranges::sort(bindings, {}, &VertexBufferBinding::binding);
    return vertex_array.configure(bindings, attributes);
}

} // namespace vng::opengl
