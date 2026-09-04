#include <vng/gfx/gfx.hpp>

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};

using Inputs = vng::gfx::TypeList<Position, Color>;
using Vertex = vng::gfx::Record<Position>;
using Layout = vng::gfx::VertexLayout<vng::gfx::Stream<Vertex>>;

auto force_instantiation = vng::gfx::resolve_vertex_input<Inputs, Layout>();
