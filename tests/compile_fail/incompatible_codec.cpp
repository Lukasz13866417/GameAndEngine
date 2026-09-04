#include <vng/gfx/gfx.hpp>

struct Position : vng::gfx::Semantic<vng::Vec2> {};

using InvalidRecord = vng::gfx::Record<
    vng::gfx::as<Position, vng::gfx::unorm8x4>>;

InvalidRecord force_instantiation;
