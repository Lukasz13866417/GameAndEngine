#pragma once

#include "spaceship_renderer.hpp"

#include <filesystem>
#include <vng/content/content.hpp>

namespace example::spaceflight {

inline vng::content::Result<Mesh> load_ship(const std::filesystem::path& path)
{
    auto schema = vng::content::vmesh::schema<Vertex>();
    schema.map("position", vng::gfx::Position{});
    schema.map("normal", vng::gfx::Normal{});
    schema.map("color/0", vng::gfx::Color{});
    schema.map("emission", Emission{});
    return vng::content::vmesh::load(path, schema);
}

} // namespace example::spaceflight
