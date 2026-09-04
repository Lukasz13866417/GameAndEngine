#include <vng/gfx/gfx.hpp>

struct Position : vng::gfx::Semantic<vng::Vec3> {};

using Geometry = vng::gfx::Record<Position>;
using DuplicateGeometry = vng::gfx::Record<Position>;
using InvalidMesh = vng::gfx::Mesh<Geometry, DuplicateGeometry>;

static_assert(sizeof(InvalidMesh) > 0);
