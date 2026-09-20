#pragma once
#include "editing_session.hpp"
#include "rotation_math.hpp"
#include "../support/mesh_frame.hpp"

namespace editor_example {
struct CameraBakeOptions { bool rotation{true},scale{true}; };
struct MeshCameraBake { vng::Mat4 placement; CameraPose camera; };

// Transfer viewing rotation relative to the standard mesh view and optical
// magnification into a uniform, centroid-preserving mesh placement. Camera
// translation is never baked. Perspective lens zoom and uniform object scale
// are not identical for deep objects; framing at the center plane is preserved.
inline vng::content::Result<MeshCameraBake> mesh_camera_bake(vng::Mat4 placement,
    vng::Vec3 local_center,CameraPose pose,CameraBakeOptions options) {
    using namespace vng;
    using namespace example::mesh_frame;
    if(!valid_camera_pose(pose))return std::unexpected(error("Invalid mesh camera"));
    if(auto valid=inverse(placement);!valid)return std::unexpected(valid.error());
    MeshCameraBake result{placement,pose};
    if(!options.rotation&&!options.scale)return result;
    const CameraPose standard;
    if(options.rotation) {result.camera.yaw=standard.yaw;result.camera.pitch=standard.pitch;}
    const auto old_axes=rotation_math::matrix({-pose.pitch,pose.yaw,0});
    const auto new_axes=rotation_math::matrix({-result.camera.pitch,result.camera.yaw,0});
    const auto rotation=options.rotation?rotation_math::multiply(new_axes,rotation_math::transpose(old_axes)):
        rotation_math::matrix({});
    const float scale=options.scale?pose.zoom:1.F;
    if(result.camera.yaw==pose.yaw&&result.camera.pitch==pose.pitch&&scale==1)return result;
    const auto center=point(placement,local_center);
    auto delta=Mat4::identity();
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)delta[c][r]=static_cast<float>(rotation[r][c])*scale;
    const auto rotated_center=vector(delta,center);
    for(unsigned c=0;c<3;++c)delta[3][c]=center[c]-rotated_center[c];
    result.placement=compose(delta,placement);
    const Vec3 offset{pose.target.x-center.x,pose.target.y-center.y,pose.target.z-center.z};
    const auto target=rotation_math::apply(rotation,offset);
    for(unsigned c=0;c<3;++c)result.camera.target[c]=center[c]+target[c];
    if(options.scale) {
        result.camera.zoom=1;
        // Retain the center's screen position after removing optical zoom.
        const auto eye=camera(result.camera,ViewMode::mesh).position();
        const auto local=rotation_math::apply(rotation_math::transpose(new_axes),
            {center.x-eye.x,center.y-eye.y,center.z-eye.z});
        const auto pan=rotation_math::apply(new_axes,{(1-scale)*local.x,(1-scale)*local.y,0});
        for(unsigned c=0;c<3;++c)result.camera.target[c]+=pan[c];
    }
    if(!valid_camera_pose(result.camera))return std::unexpected(error("Baked camera framing exceeds the viewing range"));
    if(auto valid=inverse(result.placement);!valid)return std::unexpected(valid.error());
    return result;
}

// One ordinary placement transaction: published geometry/instances stay intact
// until Apply mesh to scene, and Save on disk uses the existing geometry baker.
inline vng::content::Result<bool> bake_mesh_camera(EditingSession& editing,CameraBakeOptions options) {
    using namespace vng;
    const auto& state=editing.state();
    const auto* mesh=editable_mesh(state);
    if(state.viewport.mode!=ViewMode::mesh||state.viewport.pilot_camera||!mesh||!mesh->size())
        return std::unexpected(example::mesh_frame::error("Camera baking needs a mesh-edit view"));
    const auto id=state.viewport.inspected_mesh;
    auto baked=mesh_camera_bake(mesh_placement(state,id,true),mesh->center(),state.viewport.editor_camera,options);
    if(!baked)return std::unexpected(baked.error());
    if(auto begun=editing.begin_mesh_transform(id);!begun)return std::unexpected(begun.error());
    auto changed=editing.mesh_transform(baked->placement);
    if(!changed) {(void)editing.cancel();return std::unexpected(changed.error());}
    auto committed=editing.commit();if(!committed)return std::unexpected(committed.error());
    editing.viewport().editor_camera=baked->camera;
    editing.viewport().smooth_zoom=false;
    return *committed;
}
}
