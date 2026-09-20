#include "blueprint_mesh_controls.hpp"
#include "../support/earth_assets.hpp"
#include "../support/mesh_frame.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace editor_example {
namespace {
MeshDraftEdit move_cloud(vng::u32 id, vng::Vec2 location) {
    return {"Cloud placement", [id, location](const vng::editor::EditableMesh& source)
        -> vng::content::Result<vng::editor::EditableMesh> {
        auto moved = example::earth::move_cloud(source.document(), id, location);
        if (!moved) return std::unexpected(moved.error());
        return vng::editor::EditableMesh::create(std::move(*moved));
    }};
}
MeshDraftEdit rotate_cloud(vng::u32 id,vng::f32 angle) {
    return {"Cloud rotation",[id,angle](const vng::editor::EditableMesh& source)->vng::content::Result<vng::editor::EditableMesh> {
        auto result=example::earth::rotate_cloud(source.document(),id,angle);
        if(!result)return std::unexpected(result.error());
        return vng::editor::EditableMesh::create(std::move(*result));
    }};
}
// Earth owns its mesh-edit vocabulary, just as scene blueprints supply their
// gizmos/instance controls. Neither the application nor the UI adapter knows
// what coverage or a spiral is.
vng::editor::Result<BlueprintMeshDescription> describe_earth(vng::editor::Inspector& ui,
    const vng::editor::EditableMesh& mesh,SubmitMeshDraftEdit submit,vng::u32 selected_part) {
    namespace earth=example::earth;
    using Settings=earth::CloudSettings;
    auto settings=earth::cloud_settings(mesh.document());
    if(!settings)return std::unexpected(vng::editor::Diagnostic{settings.error().message});
    auto frame=example::mesh_frame::read(mesh.document());
    if(!frame)return std::unexpected(vng::editor::Diagnostic{frame.error().message});
    BlueprintMeshDescription description;
    description.hint="Rebuild replaces hand edits in generated parts.";
    if(earth::has_cloud_ownership(mesh.document())) {
        for(auto kind:{earth::CloudKind::bank,earth::CloudKind::spiral})
            ui.action(kind==earth::CloudKind::bank?"add_cloud_bank":"add_cloud_spiral",[submit,kind] {
                submit({"New cloud",[kind](const vng::editor::EditableMesh& source)->vng::content::Result<vng::editor::EditableMesh> {
                    auto result=earth::add_cloud(source.document(),kind);
                    if(!result)return std::unexpected(result.error());
                    return vng::editor::EditableMesh::create(std::move(*result));
                },true});
            },kind==earth::CloudKind::bank?"Add cloud bank":"Add spiral cloud");
        description.part_field="earth/cloud";
        auto formations=earth::cloud_formations(mesh.document());
        if(!formations)return std::unexpected(vng::editor::Diagnostic{formations.error().message});
        for(const auto& cloud:*formations)description.parts.push_back({cloud.id,std::string(cloud.name)});
        const auto found=std::ranges::find(*formations,selected_part,&earth::CloudFormation::id);
        if(found!=formations->end()) {
            struct Location {vng::f32 longitude,latitude;};
            auto edit=ui.edit("earth_cloud_location",Location{found->location.x,found->location.y},"Cloud formation / surface position");
            edit.slider("longitude",&Location::longitude,-180,180,"Longitude (degrees)");
            edit.slider("latitude",&Location::latitude,-90,90,"Latitude (degrees)");
            edit.apply("Move cloud",[submit,id=found->id](const Location& next) {
                submit(move_cloud(id,{next.longitude,next.latitude}));
            });
            struct Turn {vng::f32 degrees{};};
            auto turn=ui.edit("earth_cloud_heading",Turn{},"Cloud rotation / surface normal");
            turn.slider("degrees",&Turn::degrees,-180,180,"Turn (degrees)");
            turn.apply("Rotate cloud",[submit,id=found->id](const Turn& next){submit(rotate_cloud(id,next.degrees));});
            ui.action("remove_cloud",[submit,id=found->id] {
                submit({"Remove cloud",[id](const vng::editor::EditableMesh& source)->vng::content::Result<vng::editor::EditableMesh> {
                    auto result=earth::remove_cloud(source.document(),id);
                    if(!result)return std::unexpected(result.error());
                    return vng::editor::EditableMesh::create(std::move(*result));
                }});
            },"Remove cloud");
            if (settings->visible) {
                constexpr auto radians = std::numbers::pi_v<vng::f32>/180;
                const auto lon = found->location.x*radians, lat = found->location.y*radians;
                const auto radius = 1.F+.018F*settings->altitude;
                description.gizmo = MeshPartGizmo{
                    {{}, {radius*std::sin(lon)*std::cos(lat), radius*std::sin(lat), radius*std::cos(lon)*std::cos(lat)},
                        radius, std::string(found->name), *frame, true},
                    [id=found->id](vng::Vec3 point) {
                        const auto length = std::hypot(point.x,point.y,point.z);
                        return move_cloud(id,{std::atan2(point.x,point.z)/radians,
                            std::asin(std::clamp(point.y/length,-1.F,1.F))/radians});
                    },[id=found->id](vng::f32 angle){return rotate_cloud(id,angle);}};
            }
            description.hint=settings->visible
                ? "G: move / R: turn / arrows: nudge."
                : "Clouds hidden. Coordinates edit\ntheir saved surface placement.";
            return description;
        }
    } else description.hint="Rebuild once to edit formations (replaces clouds).";
    if(!description.parts.empty()) description.hint="4: click a cloud / 5: whole mesh.\nRebuild replaces cloud hand edits.";
    auto clouds=ui.edit("earth_clouds",*settings,"Earth clouds / mesh draft");
    clouds.toggle("visible",&Settings::visible,"Clouds visible");
    clouds.slider("coverage",&Settings::coverage,.25F,2,"Coverage");
    clouds.slider("puff_size",&Settings::puff_size,.5F,1.8F,"Puff size");
    clouds.slider("spiral_size",&Settings::spiral_size,.5F,1.8F,"Spiral size");
    clouds.slider("edge_scatter",&Settings::edge_scatter,0,2,"Edge scatter");
    clouds.slider("altitude",&Settings::altitude,1,3,"Altitude");
    clouds.slider("relief",&Settings::relief,0,2,"Height variation");
    clouds.apply("Rebuild clouds",[submit=std::move(submit)](const Settings& next) {
        submit({"Clouds",[next](const vng::editor::EditableMesh& source)->vng::content::Result<vng::editor::EditableMesh> {
            auto rebuilt=earth::rebuild_clouds(source.document(),next);
            if(!rebuilt)return std::unexpected(rebuilt.error());
            return vng::editor::EditableMesh::create(std::move(*rebuilt));
        }});
    });
    return description;
}
}
vng::editor::Result<BlueprintMeshDescription> describe_blueprint_mesh(vng::editor::Inspector& ui,
    const vng::editor::EditableMesh& mesh,SubmitMeshDraftEdit submit,vng::u32 selected_part) {
    if(example::earth::is_earth(mesh.document()))return describe_earth(ui,mesh,std::move(submit),selected_part);
    return BlueprintMeshDescription{};
}
}
