#pragma once
#include <vng/content/vmesh.hpp>
#include <array>
#include <vector>

// The colours of a flat-coloured mesh, as blueprint colour controls edit them:
// each distinct color/0 value is one palette entry, and recolouring an entry
// changes every vertex that carries it. CPU-only document functions.
namespace editor_example {
struct MeshColour {
    std::array<vng::f32, 4> value{}; // linear RGBA; alpha 1 for an RGB field
    std::size_t vertices{};
};
// Most common first. A mesh painted per vertex (more than `most` colours) or
// without a float RGB/RGBA color/0 field has no palette.
[[nodiscard]] std::vector<MeshColour> mesh_palette(const vng::content::vmesh::Document&, std::size_t most = 32);
// Every vertex coloured exactly `from` takes the linear RGB `to`; alpha and
// every other field are kept.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> recolour(
    vng::content::vmesh::Document, const std::array<vng::f32, 4>& from, vng::Vec3 to);
// The panel shows channels as 8-bit sRGB, the way colour pickers do; files
// keep linear values. Values above 1 (glowing colours) show as 255.
[[nodiscard]] vng::u32 to_srgb8(vng::f32 linear);
[[nodiscard]] vng::f32 from_srgb8(vng::u32 srgb);
} // namespace editor_example
