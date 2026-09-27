#pragma once
#include "../editor/project.hpp"
namespace example::earth {
inline constexpr vng::u32 instance_id=1;
inline constexpr auto blueprint_id=static_cast<editor_example::BlueprintId>(3);
[[nodiscard]] vng::content::Result<editor_example::State> author_scene(const std::filesystem::path& assets);
// Explicit, idempotent asset-authoring step, not a load-time migration. Preserves
// all non-infrastructure geometry, identities and existing route endpoints.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> author_tunnel_network(
    const vng::content::vmesh::Document&);
// Deliberate art-direction reset: retain cities and natural geometry, replace
// every other addon with a focused authored network. Never called on load.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> redesign_infrastructure(
    const vng::content::vmesh::Document&);
// Add global/ocean/polar corridors once, preserving every existing part and
// hand edit. Explicit authoring operation, never a scene-load migration.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> expand_global_infrastructure(
    const vng::content::vmesh::Document&);
}
