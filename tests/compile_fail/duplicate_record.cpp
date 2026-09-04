#include <vng/gfx/gfx.hpp>

struct Position : vng::gfx::Semantic<vng::Vec2> {};

using InvalidRecord = vng::gfx::Record<Position, Position>;
InvalidRecord force_instantiation;
