#pragma once
#include "project.hpp"
#include <vng/gfx/mesh.hpp>
#include <algorithm>

namespace editor_example {
// Private mesh-view state, not geometry, a draft, or a scene-file property.
// Sent only when visibility changes, separately from cheap camera requests.
struct MeshVisibility {
    BlueprintId blueprint{BlueprintId::mesh};
    vng::gfx::MeshTopologyFingerprint topology{};
    std::vector<vng::u32> hidden_faces; // sorted, unique source-face IDs
    friend bool operator==(const MeshVisibility&,const MeshVisibility&)=default;
};
struct MeshVisibilityRequest {
    vng::u64 required_document_revision{};
    MeshVisibility visibility;
};
inline std::string encode_mesh_visibility(const MeshVisibilityRequest& request) {
    std::string bytes{"VNGHIDE\1",8};
    const auto integer=[&](vng::u64 n,unsigned width) {
        for(unsigned i=0;i<width;++i) bytes+=static_cast<char>((n>>(8*i))&255);
    };
    integer(request.required_document_revision,8);
    const auto& v=request.visibility;
    integer(static_cast<vng::u32>(v.blueprint),4);
    integer(v.topology.low,8);integer(v.topology.high,8);
    for(auto id:v.hidden_faces) integer(id,4);
    return bytes;
}
inline std::expected<MeshVisibilityRequest,std::string> decode_mesh_visibility(std::string_view bytes) {
    if(bytes.size()<36 || (bytes.size()-36)%4 || bytes.substr(0,8)!=std::string_view{"VNGHIDE\1",8})
        return std::unexpected("Invalid mesh visibility packet");
    std::size_t offset=8;
    const auto integer=[&](unsigned width) {
        vng::u64 value{};
        for(unsigned i=0;i<width;++i) value|=vng::u64(static_cast<unsigned char>(bytes[offset++]))<<(8*i);
        return value;
    };
    MeshVisibilityRequest request;
    request.required_document_revision=integer(8);
    auto& v=request.visibility;
    v.blueprint=static_cast<BlueprintId>(integer(4));
    v.topology.low=integer(8);v.topology.high=integer(8);
    v.hidden_faces.reserve((bytes.size()-36)/4);
    while(offset<bytes.size()) {
        const auto id=static_cast<vng::u32>(integer(4));
        if(!v.hidden_faces.empty() && id<=v.hidden_faces.back())
            return std::unexpected("Mesh visibility face IDs must be sorted and unique");
        v.hidden_faces.push_back(id);
    }
    if(!request.required_document_revision) return std::unexpected("Invalid mesh visibility revision");
    return request;
}
} // namespace editor_example
