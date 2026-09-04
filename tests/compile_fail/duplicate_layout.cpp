#include <vng/gfx/gfx.hpp>

struct Position : vng::gfx::Semantic<vng::Vec2> {};

using First = vng::gfx::Record<Position>;
using Second = vng::gfx::Record<Position>;
using InvalidLayout = vng::gfx::VertexLayout<
    vng::gfx::Stream<First>,
    vng::gfx::Stream<Second>>;

InvalidLayout force_instantiation;
