#include <vng/content/content.hpp>

struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};

using Vertex = vng::gfx::Record<Position, Color>;

auto force_instantiation = vng::content::vmesh::schema<Vertex>(
    vng::content::vmesh::map("position", Position{}));
