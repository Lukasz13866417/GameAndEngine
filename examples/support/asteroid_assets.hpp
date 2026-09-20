#pragma once
#include <vng/content/vmesh.hpp>

namespace example::asteroids {
// Offline, original geometry. The saved scene embeds these as shared blueprints.
[[nodiscard]] vng::content::vmesh::Document rock_mesh(vng::u32 variant);
}
