#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <typeindex>
#include <type_traits>
#include <utility>
#include <vector>

#include <vng/gfx/mesh.hpp>
#include <vng/gfx/vertex_layout.hpp>
#include <vng/opengl/buffer.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/gfx_vertex_input.hpp>
#include <vng/opengl/graphics_pipeline.hpp>
#include <vng/opengl/program.hpp>
#include <vng/opengl/vertex_array.hpp>
#include <vng/shader/interface.hpp>

namespace vng::opengl {

namespace detail {

[[nodiscard]] inline Diagnostic mesh_upload_diagnostic(
    const gfx::MeshDiagnostic& diagnostic)
{
    return Diagnostic{
        .code = ErrorCode::invalid_argument,
        .message = "GpuMesh upload rejected the CPU mesh: " + diagnostic.message,
    };
}

template<class Inputs>
concept VertexInputInterface = shader::Interface<Inputs>
    && Inputs::stage == shader::StageKind::vertex
    && Inputs::direction == shader::InterfaceDirection::input;

} // namespace detail

// An immutable OpenGL snapshot of a CPU mesh. Vertex streams remain split in
// the same order as the source Mesh, while faces become one u32 element buffer.
// MeshInfo and explicit edges stay CPU-side, and later CPU edits require a new
// upload. Shader-specific vertex arrays are created lazily because attribute
// locations belong to a shader input contract, not to the mesh alone.
template<gfx::RecordType... RecordTypes>
    requires (sizeof...(RecordTypes) > 0)
class GpuMesh final {
public:
    using cpu_mesh_type = gfx::Mesh<RecordTypes...>;
    using layout_type = gfx::VertexLayout<
        gfx::Stream<RecordTypes, gfx::PerVertex>...>;

    static std::expected<GpuMesh, Diagnostic> upload(
        const Device& device,
        const cpu_mesh_type& mesh)
    {
        if (auto valid = mesh.validate(); !valid) {
            return std::unexpected(detail::mesh_upload_diagnostic(valid.error()));
        }

        constexpr auto maximum_gl_buffer_size = static_cast<std::size_t>(
            std::numeric_limits<std::intptr_t>::max());
        constexpr auto maximum_draw_indices = static_cast<std::size_t>(
            std::numeric_limits<std::int32_t>::max());
        constexpr auto maximum_buffer_indices = maximum_gl_buffer_size
            / sizeof(std::uint32_t);
        constexpr auto maximum_index_count = maximum_draw_indices < maximum_buffer_indices
            ? maximum_draw_indices
            : maximum_buffer_indices;
        if (mesh.face_count() > maximum_index_count / 3U) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "GpuMesh upload rejected a face count that exceeds the OpenGL draw-count range",
            });
        }

        std::expected<void, Diagnostic> ranges_valid{};
        mesh.for_each_vertex_stream([&](const auto& stream) {
            using Stream = std::remove_cvref_t<decltype(stream)>;
            using Record = typename Stream::record_type;
            if (!ranges_valid) {
                return;
            }
            if constexpr (Record::stride > std::numeric_limits<std::uint32_t>::max()) {
                ranges_valid = std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "GpuMesh upload rejected a vertex stride that cannot be represented by OpenGL",
                });
            } else if (stream.byte_size() > maximum_gl_buffer_size) {
                ranges_valid = std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "GpuMesh upload rejected a vertex stream larger than GLsizeiptr",
                });
            }
        });
        if (!ranges_valid) {
            return std::unexpected(std::move(ranges_valid.error()));
        }

        const auto index_count = static_cast<std::uint32_t>(mesh.face_count() * 3U);
        std::vector<std::uint32_t> indices;
        indices.reserve(index_count);
        for (const auto& face : mesh.faces()) {
            indices.insert(indices.end(), face.vertices.begin(), face.vertices.end());
        }

        if (auto current = device.require_current("GpuMesh::upload"); !current) {
            return std::unexpected(std::move(current.error()));
        }

        std::vector<Buffer> vertex_buffers;
        vertex_buffers.reserve(sizeof...(RecordTypes));
        std::expected<void, Diagnostic> streams_uploaded{};
        mesh.for_each_vertex_stream([&](const auto& stream) {
            if (!streams_uploaded) {
                return;
            }

            std::expected<Buffer, Diagnostic> uploaded = stream.empty()
                ? Buffer::create(device, BufferCreateInfo{.size = 1})
                : Buffer::from_bytes(device, stream.bytes());
            if (!uploaded) {
                streams_uploaded = std::unexpected(std::move(uploaded.error()));
                return;
            }
            vertex_buffers.push_back(std::move(*uploaded));
        });
        if (!streams_uploaded) {
            return std::unexpected(std::move(streams_uploaded.error()));
        }

        std::expected<Buffer, Diagnostic> index_buffer = indices.empty()
            ? Buffer::create(device, BufferCreateInfo{.size = sizeof(std::uint32_t)})
            : Buffer::from_bytes(device, std::as_bytes(std::span{indices}));
        if (!index_buffer) {
            return std::unexpected(std::move(index_buffer.error()));
        }

        return GpuMesh{
            std::move(vertex_buffers),
            std::move(*index_buffer),
            static_cast<std::uint32_t>(mesh.vertex_count()),
            index_count,
            mesh.topology_fingerprint(),
        };
    }

    GpuMesh(GpuMesh&& other) noexcept
        : vertex_buffers_(std::move(other.vertex_buffers_)),
          index_buffer_(std::move(other.index_buffer_)),
          vertex_count_(std::exchange(other.vertex_count_, 0)),
          index_count_(std::exchange(other.index_count_, 0)),
          topology_fingerprint_(std::exchange(
              other.topology_fingerprint_, {})),
          vertex_arrays_(std::move(other.vertex_arrays_))
    {}

    GpuMesh& operator=(GpuMesh&& other) noexcept
    {
        if (this != &other) {
            // Tear down VAOs before replacing the buffers referenced by them.
            // OpenGL would defer deleted-buffer reclamation, but explicit
            // ordering keeps ownership and lifecycle diagnostics unsurprising.
            vertex_arrays_.clear();
            vertex_buffers_ = std::move(other.vertex_buffers_);
            index_buffer_ = std::move(other.index_buffer_);
            vertex_count_ = std::exchange(other.vertex_count_, 0);
            index_count_ = std::exchange(other.index_count_, 0);
            topology_fingerprint_ = std::exchange(
                other.topology_fingerprint_, {});
            vertex_arrays_ = std::move(other.vertex_arrays_);
        }
        return *this;
    }
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;
    ~GpuMesh() = default;

    [[nodiscard]] std::uint32_t vertex_count() const noexcept
    {
        return vertex_count_;
    }

    [[nodiscard]] std::uint32_t index_count() const noexcept
    {
        return index_count_;
    }

    [[nodiscard]] std::uint32_t face_count() const noexcept
    {
        return index_count_ / 3U;
    }

    [[nodiscard]] gfx::MeshTopologyFingerprint topology_fingerprint() const noexcept
    {
        return topology_fingerprint_;
    }

    [[nodiscard]] static consteval std::size_t vertex_stream_count() noexcept
    {
        return sizeof...(RecordTypes);
    }

    [[nodiscard]] std::size_t cached_vertex_input_count() const noexcept
    {
        return vertex_arrays_.size();
    }

    // Optional compile-time prewarming hook. Ordinary rendering should call
    // draw(device, pipeline), which derives the contract from the compiled
    // pipeline and binds both pipeline and VAO.
    template<detail::VertexInputInterface Inputs>
    [[nodiscard]] std::expected<void, Diagnostic> prepare_vertex_input(
        const Device& device)
    {
        static_assert(
            gfx::SatisfiesVertexInputs<layout_type, Inputs>,
            "GpuMesh vertex records do not satisfy the requested shader::VertexInputs");

        auto vertex_array = vertex_array_for<Inputs>(device);
        if (!vertex_array) {
            return std::unexpected(std::move(vertex_array.error()));
        }
        return {};
    }

    // Runtime-contract prewarming for factories that receive an already
    // linked program. This resolves and caches the inferred VAO without
    // binding it, so a validating renderer factory can fail before returning
    // an object that could never draw with its pipeline.
    [[nodiscard]] std::expected<void, Diagnostic> prepare_vertex_input(
        const Device& device,
        const GraphicsPipeline& pipeline)
    {
        auto vertex_array = resolved_vertex_array_for(
            device, pipeline.program());
        if (!vertex_array) {
            return std::unexpected(std::move(vertex_array.error()));
        }
        return {};
    }

    // Resolves and binds only the vertex-input state. This is the efficient
    // path after a GraphicsPipeline has already bound its owned program.
    // Program metadata remains the source of truth for semantic identity and
    // attribute location.
    [[nodiscard]] std::expected<void, Diagnostic> bind_vertex_input(
        const Device& device,
        const GraphicsPipeline& pipeline)
    {
        return bind_vertex_input(device, pipeline.program());
    }

    // Expert path for integrations that intentionally manage a raw backend
    // program instead of a GraphicsPipeline.
    [[nodiscard]] std::expected<void, Diagnostic> bind_vertex_input(
        const Device& device,
        const Program& program)
    {
        auto vertex_array = resolved_vertex_array_for(device, program);
        if (!vertex_array) {
            return std::unexpected(std::move(vertex_array.error()));
        }
        return (*vertex_array)->bind();
    }

    // Preferred standalone path: establish the pipeline state and bind this
    // mesh's inferred vertex input as one operation.
    [[nodiscard]] std::expected<void, Diagnostic> bind(
        const Device& device,
        const GraphicsPipeline& pipeline)
    {
        if (auto bound_pipeline = pipeline.bind(device); !bound_pipeline) {
            return bound_pipeline;
        }
        return bind_vertex_input(device, pipeline);
    }

    // Expert path for callers that intentionally manage a raw backend
    // program. This cannot establish the fixed-function pipeline state.
    [[nodiscard]] std::expected<void, Diagnostic> bind(
        const Device& device,
        const Program& program)
    {
        auto vertex_array = resolved_vertex_array_for(device, program);
        if (!vertex_array) {
            return std::unexpected(std::move(vertex_array.error()));
        }
        if (auto bound_program = program.bind(); !bound_program) {
            return bound_program;
        }
        return (*vertex_array)->bind();
    }

    [[nodiscard]] std::expected<void, Diagnostic> draw(
        const Device& device,
        const GraphicsPipeline& pipeline,
        std::uint32_t instance_count = 1)
    {
        if (auto bound = bind(device, pipeline); !bound) {
            return bound;
        }
        return device.draw_elements_instanced(
            Primitive::triangles,
            IndexFormat::u32,
            index_count_,
            0,
            instance_count);
    }

    // Expert raw-program draw path.
    [[nodiscard]] std::expected<void, Diagnostic> draw(
        const Device& device,
        const Program& program,
        std::uint32_t instance_count = 1)
    {
        if (auto bound = bind(device, program); !bound) {
            return bound;
        }
        return device.draw_elements_instanced(
            Primitive::triangles,
            IndexFormat::u32,
            index_count_,
            0,
            instance_count);
    }

    [[nodiscard]] std::expected<void, Diagnostic> draw_bound(
        const Device& device,
        const GraphicsPipeline& bound_pipeline,
        std::uint32_t instance_count = 1)
    {
        if (auto bound = bind_vertex_input(device, bound_pipeline); !bound) {
            return bound;
        }
        return device.draw_elements_instanced(
            Primitive::triangles,
            IndexFormat::u32,
            index_count_,
            0,
            instance_count);
    }

    // Expert raw-program path; the caller must already have bound it.
    [[nodiscard]] std::expected<void, Diagnostic> draw_bound(
        const Device& device,
        const Program& bound_program,
        std::uint32_t instance_count = 1)
    {
        if (auto bound = bind_vertex_input(device, bound_program); !bound) {
            return bound;
        }
        return device.draw_elements_instanced(
            Primitive::triangles,
            IndexFormat::u32,
            index_count_,
            0,
            instance_count);
    }

private:
    [[nodiscard]] std::expected<VertexArray*, Diagnostic>
    resolved_vertex_array_for(
        const Device& device,
        const Program& program)
    {
        if (!program.belongs_to(device)) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::incompatible_device,
                .message = "GpuMesh received a program from a different OpenGL device/context",
            });
        }
        if (!program.vertex_inputs()) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "GpuMesh requires a program compiled from a typed vng GLSL artifact; raw GLSL has no semantic contract",
            });
        }
        return vertex_array_for(device, *program.vertex_inputs());
    }

    struct VertexInputKey final {
        std::vector<std::type_index> semantics;
        std::vector<std::uint32_t> locations;

        friend bool operator==(const VertexInputKey&,
                               const VertexInputKey&) = default;
    };

    struct CachedVertexArray final {
        VertexInputKey inputs;
        VertexArray vertex_array;
    };

    GpuMesh(
        std::vector<Buffer> vertex_buffers,
        Buffer index_buffer,
        std::uint32_t vertex_count,
        std::uint32_t index_count,
        gfx::MeshTopologyFingerprint topology_fingerprint) noexcept
        : vertex_buffers_(std::move(vertex_buffers)),
          index_buffer_(std::move(index_buffer)),
          vertex_count_(vertex_count),
          index_count_(index_count),
          topology_fingerprint_(topology_fingerprint)
    {}

    template<detail::VertexInputInterface Inputs>
    [[nodiscard]] std::expected<VertexArray*, Diagnostic> vertex_array_for(
        const Device& device)
    {
        const auto resolved = gfx::resolve_vertex_input<Inputs, layout_type>();
        auto key = key_for<typename Inputs::semantics>();
        return vertex_array_for(device, resolved, std::move(key));
    }

    template<class Semantics>
    [[nodiscard]] static VertexInputKey key_for()
    {
        return key_for_list(Semantics{});
    }

    template<class... Tags>
    [[nodiscard]] static VertexInputKey key_for_list(gfx::TypeList<Tags...>)
    {
        VertexInputKey key;
        key.semantics = {std::type_index{typeid(Tags)}...};
        key.locations.reserve(sizeof...(Tags));
        for (std::uint32_t location = 0;
             location < static_cast<std::uint32_t>(sizeof...(Tags));
             ++location) {
            key.locations.push_back(location);
        }
        return key;
    }

    [[nodiscard]] std::expected<VertexArray*, Diagnostic> vertex_array_for(
        const Device& device,
        const std::vector<VertexInputMetadata>& metadata)
    {
        gfx::ResolvedVertexInput resolved;
        resolved.stream_count = sizeof...(RecordTypes);
        resolved.attributes.reserve(metadata.size());

        VertexInputKey key;
        key.semantics.reserve(metadata.size());
        key.locations.reserve(metadata.size());
        std::vector<const VertexInputMetadata*> ordered_inputs;
        ordered_inputs.reserve(metadata.size());
        for (const auto& input : metadata) {
            if (!input.location) {
                return std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "GpuMesh cannot bind a program vertex input without an explicit location",
                });
            }
            ordered_inputs.push_back(&input);
        }
        std::ranges::sort(ordered_inputs, {}, [](const auto* input) {
            return *input->location;
        });

        for (const auto* input_pointer : ordered_inputs) {
            const auto& input = *input_pointer;
            if (std::ranges::contains(key.semantics, input.semantic_type)
                || std::ranges::contains(key.locations, *input.location)) {
                return std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "GpuMesh received duplicate semantic or location metadata from a program",
                });
            }

            bool found = false;
            std::size_t binding = 0;
            auto find_in_record = [&]<class Record>() {
                Record::for_each_field([&](auto descriptor) {
                    using Field = std::remove_cvref_t<decltype(descriptor)>;
                    if (!found
                        && input.semantic_type
                            == std::type_index{typeid(typename Field::semantic_type)}) {
                        resolved.attributes.push_back(gfx::ResolvedVertexAttribute{
                            .semantic_name = input.semantic_name,
                            .location = *input.location,
                            .binding = static_cast<std::uint32_t>(binding),
                            .offset = Field::offset,
                            .stride = Record::stride,
                            .divisor = 0,
                            .format = Field::format,
                        });
                        found = true;
                    }
                });
                ++binding;
            };
            (find_in_record.template operator()<RecordTypes>(), ...);

            if (!found) {
                return std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "GpuMesh does not provide shader vertex semantic "
                        + input.semantic_name,
                });
            }
            key.semantics.push_back(input.semantic_type);
            key.locations.push_back(*input.location);
        }
        return vertex_array_for(device, std::move(resolved), std::move(key));
    }

    [[nodiscard]] std::expected<VertexArray*, Diagnostic> vertex_array_for(
        const Device& device,
        gfx::ResolvedVertexInput resolved,
        VertexInputKey key)
    {
        for (auto& cached : vertex_arrays_) {
            if (cached.inputs == key) {
                if (!index_buffer_.belongs_to(device)) {
                    return std::unexpected(Diagnostic{
                        .code = ErrorCode::incompatible_device,
                        .message = "GpuMesh belongs to a different OpenGL device/context",
                    });
                }
                return &cached.vertex_array;
            }
        }

        if (!index_buffer_.belongs_to(device)) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::incompatible_device,
                .message = "GpuMesh belongs to a different OpenGL device/context",
            });
        }

        // Mesh stream ordinals are a CPU concept. Compact only the streams
        // consumed by this shader into dense GL binding indices, so unused
        // high-numbered streams do not consume binding slots.
        std::vector<std::uint32_t> used_streams;
        used_streams.reserve(resolved.attributes.size());
        for (const auto& attribute : resolved.attributes) {
            if (!std::ranges::contains(used_streams, attribute.binding)) {
                used_streams.push_back(attribute.binding);
            }
        }
        std::ranges::sort(used_streams);
        for (auto& attribute : resolved.attributes) {
            const auto found = std::ranges::lower_bound(
                used_streams, attribute.binding);
            attribute.binding = static_cast<std::uint32_t>(
                std::distance(used_streams.begin(), found));
        }
        resolved.stream_count = used_streams.size();

        auto vertex_array = VertexArray::create(device);
        if (!vertex_array) {
            return std::unexpected(std::move(vertex_array.error()));
        }

        std::vector<ResolvedStreamBuffer> streams;
        streams.reserve(used_streams.size());
        for (std::size_t binding = 0; binding < used_streams.size(); ++binding) {
            const auto source_stream = used_streams[binding];
            if (source_stream >= vertex_buffers_.size()) {
                return std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "GpuMesh resolved an invalid internal vertex stream",
                });
            }
            streams.push_back(ResolvedStreamBuffer{
                .binding = static_cast<std::uint32_t>(binding),
                .buffer = &vertex_buffers_[source_stream],
                .base_offset = 0,
            });
        }

        if (auto configured = configure_vertex_input(*vertex_array, resolved, streams);
            !configured) {
            return std::unexpected(std::move(configured.error()));
        }
        if (auto elements = vertex_array->set_element_buffer(index_buffer_); !elements) {
            return std::unexpected(std::move(elements.error()));
        }

        vertex_arrays_.push_back(CachedVertexArray{
            .inputs = std::move(key),
            .vertex_array = std::move(*vertex_array),
        });
        return &vertex_arrays_.back().vertex_array;
    }

    // Member order is intentional: cached VAOs are destroyed before the
    // buffers they reference.
    std::vector<Buffer> vertex_buffers_;
    Buffer index_buffer_;
    std::uint32_t vertex_count_{};
    std::uint32_t index_count_{};
    gfx::MeshTopologyFingerprint topology_fingerprint_{};
    std::vector<CachedVertexArray> vertex_arrays_;
};

template<gfx::RecordType... RecordTypes>
[[nodiscard]] std::expected<GpuMesh<RecordTypes...>, Diagnostic> upload_mesh(
    const Device& device,
    const gfx::Mesh<RecordTypes...>& mesh)
{
    return GpuMesh<RecordTypes...>::upload(device, mesh);
}

} // namespace vng::opengl
