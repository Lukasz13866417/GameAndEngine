#pragma once

#include "project.hpp"
#include <vng/editor/interaction_timing.hpp>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>

namespace editor_example {
// Absolute, latest-value requests. Document updates remain ordered separately;
// a request can wait for a newly imported blueprint without blocking navigation.
struct ViewportRequest {
    vng::u64 required_document_revision{};
    ViewportState view;
};
inline bool valid_viewport(const ViewportState& v) {
    const auto within = [](float x, float a, float b) { return std::isfinite(x) && x >= a && x <= b; };
    return v.sequence && v.sequence<=((vng::u64{1}<<53)-1) && static_cast<unsigned>(v.mode) <= 2 &&
        within(v.time, 0, 86400) && valid_camera_pose(v.editor_camera);
}
inline std::expected<std::string, std::string> encode_viewport_request(const ViewportRequest& r) {
    if (!r.required_document_revision || !valid_viewport(r.view))
        return std::unexpected("Invalid viewport request");
    std::string bytes{"VNGVIEW\5", 8};
    const auto integer = [&](vng::u64 n, unsigned width) {
        for (unsigned i=0; i<width; ++i) bytes += static_cast<char>((n >> (8*i)) & 255);
    };
    integer(r.required_document_revision,8); integer(r.view.sequence,8);
    integer(static_cast<unsigned>(r.view.mode),4);
    integer(static_cast<unsigned>(r.view.inspected_mesh),4);
    integer(r.view.selected_object,4); integer(r.view.selected_vertex,4);
    // Bit 2 was the retired pilot-camera flag and stays zero.
    integer(r.view.weld | (r.view.paused<<1) | (r.view.smooth_zoom<<3) |
            (r.view.show_regions<<4) | (r.view.show_world_bounds<<5) | (r.view.gizmo_only<<6),4);
    const auto& p=r.view.editor_camera;
    for (float value : {r.view.time,p.yaw,p.pitch,p.distance,p.target.x,p.target.y,p.target.z,p.zoom})
        integer(std::bit_cast<vng::u32>(value),4);
    return bytes;
}
inline std::expected<ViewportRequest,std::string> decode_viewport_request(std::string_view bytes) {
    const bool legacy = bytes.size()==72 && bytes.substr(0,8)==std::string_view{"VNGVIEW\1",8};
    const bool current = bytes.size()==76 && bytes.substr(0,8)==std::string_view{"VNGVIEW\5",8};
    const bool visibility = current || (bytes.size()==76 && bytes.substr(0,8)==std::string_view{"VNGVIEW\4",8});
    const bool annotations = bytes.size()==76 && bytes.substr(0,8)==std::string_view{"VNGVIEW\3",8};
    if (!legacy && !annotations && !visibility && (bytes.size()!=76 || bytes.substr(0,8)!=std::string_view{"VNGVIEW\2",8}))
        return std::unexpected("Unknown viewport request size or version");
    std::size_t offset=8;
    const auto integer = [&](unsigned width) {
        vng::u64 value{};
        for (unsigned i=0; i<width; ++i) value |= vng::u64(static_cast<unsigned char>(bytes[offset++])) << (8*i);
        return value;
    };
    ViewportRequest r;
    r.required_document_revision=integer(8); r.view.sequence=integer(8);
    const auto mode=integer(4);
    if (mode>2) return std::unexpected("Invalid viewport mode");
    r.view.mode=static_cast<ViewMode>(mode);
    r.view.inspected_mesh=static_cast<BlueprintId>(integer(4));
    r.view.selected_object=static_cast<vng::u32>(integer(4)); r.view.selected_vertex=static_cast<vng::u32>(integer(4));
    const auto flags=integer(4);
    if (flags>(visibility?127:annotations?63:15) || (current && (flags&4))) return std::unexpected("Invalid viewport flags");
    // Older requests may carry the retired pilot-camera bit; it has no meaning now.
    r.view.weld=flags&1; r.view.paused=flags&2; r.view.smooth_zoom=flags&8;
    r.view.show_regions=flags&16; r.view.show_world_bounds=flags&32;
    r.view.gizmo_only=flags&64;
    auto& p=r.view.editor_camera;
    for (float* value : {&r.view.time,&p.yaw,&p.pitch,&p.distance,&p.target.x,&p.target.y,&p.target.z})
        *value=std::bit_cast<float>(static_cast<vng::u32>(integer(4)));
    if (!legacy) p.zoom=std::bit_cast<float>(static_cast<vng::u32>(integer(4)));
    if (!r.required_document_revision || !valid_viewport(r.view))
        return std::unexpected("Invalid viewport values");
    return r;
}

class ViewportInbox {
public:
    void offer(ViewportRequest request, vng::editor::InteractionTrace trace = {}) {
        if (request.view.sequence <= accepted_ || (pending_ && request.view.sequence <= pending_->view.sequence)) return;
        pending_=std::move(request); trace_=trace;
    }
    // A pending absolute request may reference data in an ordered document
    // update that has not arrived yet. Keep only the newest such request.
    bool apply(State& state, vng::editor::InteractionTrace& trace) {
        if (!pending_ || pending_->required_document_revision > state.document.revision) return false;
        auto view=pending_->view;
        pending_.reset();
        accepted_=view.sequence;
        if (view.sequence < state.viewport.sequence) return false;
        // Deletions/undo can make a formerly valid target disappear.
        if (!is_mesh_blueprint(state,view.inspected_mesh)) view.inspected_mesh=BlueprintId::mesh;
        if (view.selected_object && !find_instance(state,view.selected_object)) view.selected_object=0;
        view.time=std::min(view.time,state.document.timeline_duration);
        state.viewport=view;
        const auto* geometry=editable_mesh(state);
        if (!geometry || view.selected_vertex >= geometry->size()) state.viewport.selected_vertex=0;
        trace=trace_;
        trace.worker_applied_ns=vng::monotonic_ns();
        return true;
    }
private:
    vng::u64 accepted_{};
    std::optional<ViewportRequest> pending_;
    vng::editor::InteractionTrace trace_;
};

// Worker-local presentation state, never written back into authored data.
// Smooth magnification in log-space and forward travel in position space.
class ZoomMotion {
public:
    void target(CameraPose pose, bool smooth) {
        if (!initialized_ || !smooth || pose.yaw!=target_.yaw || pose.pitch!=target_.pitch)
            current_=pose;
        target_=pose; initialized_=true;
    }
    bool advance(double seconds) {
        if (!initialized_ || settled()) return false;
        const auto alpha=-std::expm1(-std::max(0.0,seconds)/.055);
        current_.distance=static_cast<float>(std::exp(std::log(current_.distance) +
            (std::log(target_.distance)-std::log(current_.distance))*alpha));
        current_.zoom=static_cast<float>(std::exp(std::log(current_.zoom) +
            (std::log(target_.zoom)-std::log(current_.zoom))*alpha));
        bool position_settled=true;
        for (std::size_t i=0;i<3;++i) {
            current_.target[i]=static_cast<float>(current_.target[i]+(target_.target[i]-current_.target[i])*alpha);
            // Float world coordinates become coarser far from the origin.
            // Do not keep redrawing forever below their representable precision.
            const auto tolerance=std::max({.00001, double(target_.distance)*.00005,
                4*std::numeric_limits<float>::epsilon()*std::abs(double(target_.target[i]))});
            position_settled &= std::abs(double(current_.target[i])-target_.target[i]) <= tolerance;
        }
        if (std::abs(std::log(current_.distance/target_.distance)) < .0005F &&
            std::abs(std::log(current_.zoom/target_.zoom)) < .0005F &&
            position_settled) current_=target_;
        return true;
    }
    [[nodiscard]] bool settled() const { return current_==target_; }
    [[nodiscard]] const CameraPose& pose() const { return current_; }
private:
    bool initialized_{};
    CameraPose current_,target_;
};
} // namespace editor_example
