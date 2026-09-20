#include <vng/editor/mesh.hpp>
#include <algorithm>
#include <cmath>
#include <set>

namespace vng::editor {
content::vmesh::Limits mesh_limits() {
    content::vmesh::Limits value;
    value.max_source_bytes=16U*1024U*1024U;
    value.max_decoded_bytes=16U*1024U*1024U;
    value.max_vertices=65536;
    value.max_faces=131072;
    value.max_edges=262144;
    value.max_scalar_values=2U*1024U*1024U;
    return value;
}
namespace {
auto invalid(std::string message) {
    content::Diagnostic diagnostic;
    diagnostic.code = content::ErrorCode::invalid_document;
    diagnostic.message = std::move(message);
    return std::unexpected(std::move(diagnostic));
}
bool finite(Vec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::abs(p.x) <= 1000000 && std::abs(p.y) <= 1000000 && std::abs(p.z) <= 1000000;
}
} // namespace
content::Result<EditableMesh> EditableMesh::create(content::vmesh::Document document) {
    if (auto valid = content::vmesh::validate(document, mesh_limits()); !valid)
        return std::unexpected(valid.error());
    if (!document.vertex_count)
        return invalid("This editor slice requires at least one vertex");
    for (std::size_t i = 0; i < document.vertex_fields.size(); ++i) {
        const auto& f = document.vertex_fields[i];
        if (f.name != "position")
            continue;
        if (f.type != content::vmesh::FieldType{content::vmesh::ScalarType::Float32, 3})
            return invalid("Mesh editing requires a float3 position field");
        EditableMesh result{std::move(document), i};
        for (u32 v = 0; v < result.size(); ++v)
            if (!finite(result.position(v)))
                return invalid("Mesh positions exceed the editing range");
        // Do not build a picking acceleration structure for transport, history
        // or worker-only geometry. The first actual picking query builds it.
        return result;
    }
    return invalid("Mesh has no position field");
}
content::Result<EditableMesh> EditableMesh::load(const std::filesystem::path& path) {
    content::vmesh::ReadOptions options;
    options.limits = mesh_limits();
    auto document = content::vmesh::read_vmesh(path, options);
    if (!document)
        return std::unexpected(document.error());
    return create(std::move(*document));
}
Vec3 EditableMesh::position(u32 vertex) const {
    if (vertex >= size())
        throw std::out_of_range("Editor vertex index");
    const auto& values =
        std::get<std::vector<f32>>(document_.vertex_fields[position_field_].values);
    const auto i = static_cast<std::size_t>(vertex) * 3;
    return {values[i], values[i + 1], values[i + 2]};
}
Vec3 EditableMesh::center() const {
    if(!geometry_)geometry_=std::make_shared<GeometryCache>();
    if(!geometry_->center) {
        std::array<double,3> sum{};
        for(u32 i=0;i<size();++i) {
            const auto p=position(i);
            for(unsigned c=0;c<3;++c)sum[c]+=p[c];
        }
        const auto count=static_cast<double>(std::max<std::size_t>(1,size()));
        geometry_->center=Vec3{f32(sum[0]/count),f32(sum[1]/count),f32(sum[2]/count)};
    }
    return *geometry_->center;
}
const spatial::TriangleBvh& EditableMesh::picking_index() const {
    if(!geometry_)geometry_=std::make_shared<GeometryCache>();
    if(!geometry_->index) {
        std::vector<Vec3> positions;positions.reserve(size());
        for(u32 i=0;i<size();++i)positions.push_back(position(i));
        std::vector<spatial::TriangleBvh::Triangle> faces;faces.reserve(document_.faces.size());
        for(auto f:document_.faces)faces.push_back({f[0],f[1],f[2]});
        geometry_->index=std::make_shared<const spatial::TriangleBvh>(positions,faces);
    }
    return *geometry_->index;
}
content::Result<void> EditableMesh::set_position(u32 vertex, Vec3 value) {
    if (vertex >= size())
        return invalid("Vertex index is outside the mesh");
    if (!finite(value))
        return invalid("Position must be finite and within the editing range");
    if(position(vertex)==value)return {};
    auto& values = std::get<std::vector<f32>>(document_.vertex_fields[position_field_].values);
    const auto i = static_cast<std::size_t>(vertex) * 3;
    values[i] = value.x;
    values[i + 1] = value.y;
    values[i + 2] = value.z;
    geometry_.reset();
    return {};
}
content::Result<void> EditableMesh::translate(std::span<const u32> vertices, Vec3 delta) {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z))
        return invalid("Translation must be finite");
    std::set<u32> unique(vertices.begin(), vertices.end());
    for (auto id : unique) {
        if (id >= size())
            return invalid("Vertex index is outside the mesh");
        const auto p = position(id);
        if (!finite({p.x + delta.x, p.y + delta.y, p.z + delta.z}))
            return invalid("Translation exceeds the editing range");
    }
    for (auto id : unique) {
        const auto p = position(id);
        (void)set_position(id, {p.x + delta.x, p.y + delta.y, p.z + delta.z});
    }
    return {};
}
std::vector<u32> EditableMesh::coincident(u32 vertex, f32 tolerance) const {
    const auto p = position(vertex);
    std::vector<u32> found;
    if (!std::isfinite(tolerance) || tolerance < 0)
        throw std::invalid_argument("Invalid weld tolerance");
    for (u32 i = 0; i < size(); ++i) {
        const auto q = position(i);
        if (std::abs(p.x - q.x) <= tolerance && std::abs(p.y - q.y) <= tolerance &&
            std::abs(p.z - q.z) <= tolerance)
            found.push_back(i);
    }
    return found;
}
} // namespace vng::editor
