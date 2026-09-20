#pragma once

#include <vng/render/mesh_renderer.hpp>

namespace vng::providers {

class MeshProvider {
public:
    explicit MeshProvider(resources::Result<render::MeshSource> source) : source_(std::move(source)) {}
    [[nodiscard]] resources::Result<render::MeshSource> provide() const { return source_; }
private:
    resources::Result<render::MeshSource> source_;
};

template<class Mesh, class P = gfx::Position, class U = gfx::TexCoord<>, class C = gfx::Color>
[[nodiscard]] MeshProvider mesh(const Mesh& mesh, render::MeshFields<P,U,C> fields = {})
{ return MeshProvider{render::mesh_source(mesh, fields)}; }

} // namespace vng::providers
