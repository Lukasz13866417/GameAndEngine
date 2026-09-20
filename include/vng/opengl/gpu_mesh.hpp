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
#include <vng/opengl/instance_buffer.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/gfx_vertex_input.hpp>
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

struct MeshUploadOptions final {
    // Keep static uploads immutable by default. Opt in for vertex deformation
    // without replacing element buffers or shader-specific cached VAOs.
    bool dynamic_vertices{};
    // Temporary face visibility without changing source topology/primitive IDs.
    bool dynamic_face_visibility{};
};

// An OpenGL snapshot of a CPU mesh. Vertex streams remain split in
// the same order as the source Mesh, while faces become one u32 element buffer.
// MeshInfo and explicit edges stay CPU-side. Dynamic vertex storage permits
// in-place record updates; topology changes require a new upload.
// Shader-specific vertex arrays are created lazily because attribute
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
        const cpu_mesh_type& mesh,
        MeshUploadOptions options = {})
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

            const auto storage = options.dynamic_vertices ? BufferStorage::dynamic : BufferStorage::none;
            std::expected<Buffer, Diagnostic> uploaded = stream.empty()
                ? Buffer::create(device, BufferCreateInfo{.size = 1, .storage = storage})
                : Buffer::from_bytes(device, stream.bytes(), storage);
            if (!uploaded) {
                streams_uploaded = std::unexpected(std::move(uploaded.error()));
                return;
            }
            vertex_buffers.push_back(std::move(*uploaded));
        });
        if (!streams_uploaded) {
            return std::unexpected(std::move(streams_uploaded.error()));
        }

        const auto index_storage=options.dynamic_face_visibility?BufferStorage::dynamic:BufferStorage::none;
        std::expected<Buffer, Diagnostic> index_buffer = indices.empty()
            ? Buffer::create(device, BufferCreateInfo{.size = sizeof(std::uint32_t),.storage=index_storage})
            : Buffer::from_bytes(device, std::as_bytes(std::span{indices}),index_storage);
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

    // An empty mask restores all faces. Source data stays with the caller;
    // hiding uploads only indices and preserves resident vertices/cached VAOs.
    // Degenerate triangles retain the original gl_PrimitiveID for diagnostics.
    [[nodiscard]] std::expected<void,Diagnostic> set_hidden_faces(
        const Device& device,const cpu_mesh_type& source,std::span<const u32> hidden)
    {
        if(!index_buffer_.belongs_to(device))
            return std::unexpected(Diagnostic{.code=ErrorCode::incompatible_device,
                .message="Face visibility requires the mesh's device"});
        if(auto ready=device.require_resource_update("GpuMesh::set_hidden_faces");!ready) return ready;
        if(source.topology_fingerprint()!=topology_fingerprint_ ||
           std::ranges::any_of(hidden,[&](u32 id){return id>=face_count();}))
            return std::unexpected(Diagnostic{.code=ErrorCode::invalid_argument,
                .message="Face visibility requires matching source topology and valid face IDs"});
        if(!index_count_) return {};
        std::vector<u32> indices;indices.reserve(index_count_);
        for(const auto& f:source.faces()) indices.insert(indices.end(),f.vertices.begin(),f.vertices.end());
        for(auto id:hidden) indices[3*id+1]=indices[3*id+2]=indices[3*id];
        return index_buffer_.write(0,std::as_bytes(std::span{indices}));
    }

    [[nodiscard]] static consteval std::size_t vertex_stream_count() noexcept
    {
        return sizeof...(RecordTypes);
    }

    [[nodiscard]] std::size_t cached_vertex_input_count() const noexcept
    {
        return vertex_arrays_.size();
    }

    // Record type selects its stream; offsets and byte encoding stay internal.
    // This preserves vertex count, topology, buffer identity, and cached VAOs.
    // Call at a resource-update boundary, outside an active Frame.
    template<gfx::RecordType Record>
        requires ((std::same_as<Record, RecordTypes>) || ...)
    [[nodiscard]] std::expected<void, Diagnostic> update_vertices(
        const Device& device, std::span<const Record> records, std::size_t first_vertex = 0)
    {
        constexpr std::size_t binding = [] {
            constexpr std::array matches{std::same_as<Record, RecordTypes>...};
            for (std::size_t i = 0; i < matches.size(); ++i) if (matches[i]) return i;
            return matches.size();
        }();
        if (binding >= vertex_buffers_.size() || !vertex_buffers_[binding].belongs_to(device))
            return std::unexpected(Diagnostic{.code = ErrorCode::incompatible_device,
                .message = "GpuMesh::update_vertices requires a live mesh and its creating device"});
        if (first_vertex > vertex_count_ || records.size() > vertex_count_ - first_vertex)
            return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
                .message = "GpuMesh::update_vertices exceeds its existing vertex count"});
        if (auto allowed = device.require_resource_update("GpuMesh::update_vertices"); !allowed)
            return allowed;
        return vertex_buffers_[binding].write(first_vertex * Record::stride, std::as_bytes(records));
    }

    template<gfx::RecordType Record>
        requires ((std::same_as<Record, RecordTypes>) || ...)
    [[nodiscard]] std::expected<void, Diagnostic> update_vertices(
        const Device& device, const gfx::VertexStream<Record>& records, std::size_t first_vertex = 0)
    { return update_vertices(device, std::span<const Record>{records.data(), records.size()}, first_vertex); }

    // Optional compile-time prewarming hook. Ordinary rendering should call
    // draw(device, program), which derives the contract from the compiled
    // program and binds both program and VAO.
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

    // Resolve and cache vertex input against the program contract.
    [[nodiscard]] std::expected<void, Diagnostic> prepare_vertex_input(
        const Device& device, const Program& program)
    {
        auto vertex_array = resolved_vertex_array_for(device, program);
        if (!vertex_array) return std::unexpected(std::move(vertex_array.error()));
        return {};
    }

    template<class ProgramType>
        requires requires(const ProgramType& program) {
            typename ProgramType::signature;
            { program.untyped() } -> std::same_as<const Program&>;
        }
    [[nodiscard]] std::expected<void, Diagnostic> prepare_vertex_input(
        const Device& device, const ProgramType& program)
    { return prepare_vertex_input(device, program.untyped()); }

    template<class ProgramType>
        requires requires(const ProgramType& program) {
            typename ProgramType::signature;
            { program.untyped() } -> std::same_as<const Program&>;
        }
    [[nodiscard]] std::expected<void, Diagnostic> bind_vertex_input(
        const Device& device, const ProgramType& program)
    { return bind_vertex_input(device, program.untyped()); }

    // Resolve and bind vertex input without changing the selected program.
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

    // Expert raw-program draw path.
    [[nodiscard]] std::expected<void, Diagnostic> draw(
        const Device& device,
        const Program& program,
        std::uint32_t instance_count = 1)
    {
        if (auto ready = require_arguments(program); !ready) return ready;
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

    // Expert raw-program path; the caller must already have bound it.
    [[nodiscard]] std::expected<void, Diagnostic> draw_bound(
        const Device& device,
        const Program& bound_program,
        std::uint32_t instance_count = 1)
    {
        if (auto ready = require_arguments(bound_program); !ready) return ready;
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

    // The extra stream has divisor one. Its semantic contract, locations and
    // VAO remain inferred; callers never resolve or configure attributes.
    template<gfx::RecordType Instance>
    [[nodiscard]] std::expected<void, Diagnostic> draw_bound(
        const Device& device, const Program& program, const InstanceBuffer<Instance>& instances)
    {
        using Layout=gfx::VertexLayout<gfx::Stream<RecordTypes>...,gfx::Stream<Instance,gfx::PerInstance<1>>>;
        static_assert(sizeof(Layout)>0); // also rejects duplicate stream semantics
        if(!program.belongs_to(device) || !index_buffer_.belongs_to(device) ||
           (instances.storage() && instances.storage()->native_handle() && !instances.storage()->belongs_to(device)))
            return std::unexpected(Diagnostic{.code=ErrorCode::incompatible_device,
                .message="Instanced mesh draw requires resources from the same device"});
        if(auto ready=require_arguments(program);!ready)return ready;
        if(!program.vertex_inputs())return std::unexpected(Diagnostic{.code=ErrorCode::invalid_argument,
            .message="Instanced mesh draw requires typed vertex metadata"});
        if(!instances.size())return {};
        auto vao=vertex_array_for<Instance>(device,*program.vertex_inputs(),instances.storage());
        if(!vao)return std::unexpected(vao.error());
        if(auto bound=(*vao)->bind();!bound)return bound;
        return device.draw_elements_instanced(Primitive::triangles,IndexFormat::u32,index_count_,0,instances.size());
    }

private:
    [[nodiscard]] static std::expected<void, Diagnostic> require_arguments(const Program& program)
    {
        if (program.arguments_ready()) return {};
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "GpuMesh draw requires the shader's typed arguments",
        });
    }

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
        std::type_index instance_record{typeid(void)};
        std::vector<std::type_index> semantics;
        std::vector<std::uint32_t> locations;

        friend bool operator==(const VertexInputKey&,
                               const VertexInputKey&) = default;
    };

    struct CachedVertexArray final {
        VertexInputKey inputs;
        VertexArray vertex_array;
        std::optional<std::uint32_t> instance_binding;
        std::uint32_t instance_stride{};
    };

    [[nodiscard]] std::expected<VertexArray*, Diagnostic> reuse_vertex_array(
        const Device& device, CachedVertexArray& cached, const Buffer* instances) {
        if (!index_buffer_.belongs_to(device))
            return std::unexpected(Diagnostic{.code = ErrorCode::incompatible_device,
                .message = "GpuMesh belongs to a different OpenGL device/context"});
        if (instances && cached.instance_binding) {
            // Rebind even if the GLuint looks unchanged: deleted GL names can
            // be reused, and a renderer may alternate multiple instance buffers.
            if (auto bound = cached.vertex_array.set_vertex_buffer(
                    *cached.instance_binding, *instances, 0, cached.instance_stride); !bound)
                return std::unexpected(bound.error());
        }
        return &cached.vertex_array;
    }

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

    template<class Instance = void>
    [[nodiscard]] std::expected<VertexArray*, Diagnostic> vertex_array_for(
        const Device& device,
        const std::vector<VertexInputMetadata>& metadata,
        const Buffer* instance_buffer = nullptr)
    {
        // The common path needs no allocations, sorting, semantic resolution
        // or attribute configuration. Metadata is immutable once linked.
        for (auto& cached : vertex_arrays_) {
            if (cached.inputs.instance_record != typeid(Instance) ||
                cached.inputs.semantics.size() != metadata.size()) continue;
            bool matches = true;
            for (std::size_t i = 0; i < metadata.size(); ++i)
                if (metadata[i].semantic_type != cached.inputs.semantics[i] ||
                    metadata[i].location != cached.inputs.locations[i]) { matches = false; break; }
            if (matches) return reuse_vertex_array(device, cached, instance_buffer);
        }
        gfx::ResolvedVertexInput resolved;
        resolved.stream_count = sizeof...(RecordTypes);
        resolved.attributes.reserve(metadata.size());

        VertexInputKey key;
        key.instance_record=typeid(Instance);
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
                            .divisor = binding==sizeof...(RecordTypes) ? 1U : 0U,
                            .format = Field::format,
                        });
                        found = true;
                    }
                });
                ++binding;
            };
            (find_in_record.template operator()<RecordTypes>(), ...);
            if constexpr (!std::same_as<Instance,void>)find_in_record.template operator()<Instance>();

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
        return vertex_array_for(device, std::move(resolved), std::move(key),instance_buffer);
    }

    [[nodiscard]] std::expected<VertexArray*, Diagnostic> vertex_array_for(
        const Device& device,
        gfx::ResolvedVertexInput resolved,
        VertexInputKey key,
        const Buffer* instance_buffer = nullptr)
    {
        for (auto& cached : vertex_arrays_) {
            if (cached.inputs == key) {
                return reuse_vertex_array(device, cached, instance_buffer);
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

        auto created = VertexArray::create(device);
        if (!created) return std::unexpected(created.error());
        std::optional<std::uint32_t> instance_binding;
        std::uint32_t instance_stride{};
        for (const auto& attribute : resolved.attributes)
            if (used_streams[attribute.binding] == vertex_buffers_.size()) {
                instance_binding = attribute.binding;
                instance_stride = static_cast<std::uint32_t>(attribute.stride);
                break;
            }

        std::vector<ResolvedStreamBuffer> streams;
        streams.reserve(used_streams.size());
        for (std::size_t binding = 0; binding < used_streams.size(); ++binding) {
            const auto source_stream = used_streams[binding];
            if (source_stream >= vertex_buffers_.size() && !(instance_buffer && source_stream==vertex_buffers_.size())) {
                return std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "GpuMesh resolved an invalid internal vertex stream",
                });
            }
            streams.push_back(ResolvedStreamBuffer{
                .binding = static_cast<std::uint32_t>(binding),
                .buffer = source_stream==vertex_buffers_.size()?instance_buffer:&vertex_buffers_[source_stream],
                .base_offset = 0,
            });
        }

        if (auto configured = configure_vertex_input(*created, resolved, streams);
            !configured) {
            return std::unexpected(std::move(configured.error()));
        }
        if (auto elements = created->set_element_buffer(index_buffer_); !elements) {
            return std::unexpected(std::move(elements.error()));
        }

        vertex_arrays_.push_back(CachedVertexArray{
            .inputs = std::move(key),
            .vertex_array = std::move(*created),
            .instance_binding = instance_binding,
            .instance_stride = instance_stride,
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
    const gfx::Mesh<RecordTypes...>& mesh,
    MeshUploadOptions options = {})
{
    return GpuMesh<RecordTypes...>::upload(device, mesh, options);
}

} // namespace vng::opengl
