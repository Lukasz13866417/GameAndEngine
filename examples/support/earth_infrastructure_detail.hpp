#pragma once
#include "earth_infrastructure.hpp"
#include <optional>
#include <span>

namespace example::earth::detail {
std::vector<InfrastructurePart> default_infrastructure_parts();
vng::content::Result<vng::content::vmesh::Document> generate_infrastructure(InfrastructureSettings,std::span<const InfrastructurePart>,std::optional<vng::u32> only={});
void write_infrastructure_parts(vng::content::vmesh::Document&,std::span<const InfrastructurePart>);
vng::content::Result<vng::content::vmesh::Document> replace_infrastructure_geometry(
    const vng::content::vmesh::Document&,vng::content::vmesh::Document,std::optional<vng::u32> part={});
}
