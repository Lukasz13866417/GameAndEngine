#pragma once
#include <vng/content/vmesh.hpp>
#include "earth_clouds.hpp"

namespace example::earth {
// Offline, deterministic asset authoring. The result is an ordinary editable,
// instanced mesh with position/normal/color/emission, not a runtime effect.
// +Y = north; Greenwich faces +Z; longitude increases toward +X.
// Shorelines are deliberately simplified illustrations, not survey data.
[[nodiscard]] vng::content::vmesh::Document make_mesh(CloudSettings = {});
// Copy an authored Earth, retaining geometry/cloud edits and custom fields.
// Only land colors change: modest dry-belt expansion and warmer vegetation.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document>
make_savannah_variant(const vng::content::vmesh::Document&);
[[nodiscard]] vng::content::vmesh::Document make_cloud_mesh(CloudSettings, vng::u32 source = 0);
// Display names never identify blueprint behavior. Previously generated Earth
// assets are recognized by their original generator metadata.
[[nodiscard]] bool is_earth(const vng::content::vmesh::Document&);
[[nodiscard]] vng::content::Result<CloudSettings> cloud_settings(const vng::content::vmesh::Document&);
void write_cloud_settings(vng::content::vmesh::Document&, const CloudSettings&);
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document>
rebuild_clouds(const vng::content::vmesh::Document&, CloudSettings);
// Whole formations move rigidly about Earth's center: altitude, hand edits,
// custom attributes and neighboring formations are preserved. Rebuild retains
// their orientation. Legacy meshes need an explicit cloud rebuild first.
[[nodiscard]] bool has_cloud_ownership(const vng::content::vmesh::Document&);
[[nodiscard]] vng::content::Result<std::vector<CloudFormation>>
cloud_formations(const vng::content::vmesh::Document&);
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document>
move_cloud(const vng::content::vmesh::Document&, vng::u32 formation, vng::Vec2 longitude_latitude);
// Heading turns around the formation's radial axis, retaining surface placement.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document>
rotate_cloud(const vng::content::vmesh::Document&, vng::u32 formation, vng::f32 degrees);
enum class CloudKind { bank, spiral };
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document>
add_cloud(const vng::content::vmesh::Document&, CloudKind, vng::Vec2 longitude_latitude = {0,20});
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document>
remove_cloud(const vng::content::vmesh::Document&, vng::u32 formation);
}
