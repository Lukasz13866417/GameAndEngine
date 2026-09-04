#include <vng/content/content.hpp>

struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};

using Vertex = vng::gfx::Record<Position>;

void force_instantiation()
{
    auto schema = vng::content::vmesh::schema<Vertex>();
    schema.map("color", Color{});
}
