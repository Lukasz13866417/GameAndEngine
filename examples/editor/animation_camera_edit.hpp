#pragma once
#include "viewport_session.hpp"
#include "animation.hpp"

namespace editor_example {
using CameraTracks = std::array<std::optional<vng::timeline::Track>, camera_track_properties.size()>;
struct AnimationCameraEdit {
    vng::u64 base_revision{},revision{};
    CameraPose pose;
    // Missing means pose-only. Present replaces exactly these camera
    // tracks; an empty entry removes that camera property, never other tracks.
    std::optional<CameraTracks> tracks{};
    friend bool operator==(const AnimationCameraEdit&, const AnimationCameraEdit&) = default;
};
inline AnimationCameraEdit capture_animation_camera_edit(vng::u64 base, const Document& document) {
    AnimationCameraEdit edit{base, document.revision, document.animation_camera, CameraTracks{}};
    for (std::size_t i=0;i<camera_track_properties.size();++i)
        if (const auto* track=document.timeline.find({camera_animation_object,std::string(camera_track_properties[i])}))
            (*edit.tracks)[i]=*track;
    return edit;
}
inline std::expected<void,std::string> validate_animation_camera_edit(const AnimationCameraEdit& edit,
                                                                     vng::f32 duration=vng::timeline::max_time) {
    ViewportState check; check.editor_camera=edit.pose;
    if (!edit.base_revision || edit.revision<=edit.base_revision || edit.revision>((vng::u64{1}<<53)-1) || !valid_viewport(check))
        return std::unexpected("Invalid authored animation-camera edit");
    if (!std::isfinite(duration) || duration<0 || duration>vng::timeline::max_time)
        return std::unexpected("Invalid camera timeline duration");
    if (!edit.tracks) return {};
    std::vector<vng::timeline::Track> checked;
    for (std::size_t i=0;i<edit.tracks->size();++i) {
        const auto& track=(*edit.tracks)[i];
        if (!track) continue;
        if (track->keys.empty() || track->keys.size()>vng::timeline::max_keys_per_track)
            return std::unexpected("Camera track key count exceeds protocol bounds");
        if (track->target!=vng::timeline::Target{camera_animation_object,std::string(camera_track_properties[i])})
            return std::unexpected("Camera packet contains an unrelated or misplaced property");
        for (const auto& key:track->keys) {
            if (key.time>duration) return std::unexpected("Camera key lies beyond the scene timeline");
            CameraPose value;
            if (i==3) {
                const auto* target=std::get_if<vng::Vec3>(&key.value);
                if (!target) return std::unexpected("Camera target key requires Vec3");
                value.target=*target;
            } else {
                const auto* scalar=std::get_if<vng::f32>(&key.value);
                if (!scalar) return std::unexpected("Camera scalar key requires float");
                if (i==0) value.yaw=*scalar;
                else if (i==1) value.pitch=*scalar;
                else if (i==2) value.distance=*scalar;
                else value.zoom=*scalar;
            }
            check.editor_camera=value;
            if (!valid_viewport(check)) return std::unexpected("Camera key exceeds pose limits");
        }
        checked.push_back(*track);
    }
    vng::timeline::Timeline timeline;
    if (auto valid=timeline.replace(std::move(checked));!valid) return std::unexpected(valid.error().message);
    return {};
}
inline std::expected<std::string,std::string> encode_animation_camera_edit(const AnimationCameraEdit& edit) {
    if (auto valid=validate_animation_camera_edit(edit);!valid) return std::unexpected(valid.error());
    std::string bytes{edit.tracks ? "VNGACAM\4" : "VNGACAM\3",8};
    const auto append=[&](vng::u64 n,unsigned width) {
        for(unsigned i=0;i<width;++i) bytes+=static_cast<char>((n>>(8*i))&255);
    };
    const auto scalar=[&](float n) { append(std::bit_cast<vng::u32>(n),4); };
    append(edit.base_revision,8); append(edit.revision,8);
    for(float n:{edit.pose.yaw,edit.pose.pitch,edit.pose.distance,edit.pose.target.x,edit.pose.target.y,edit.pose.target.z,edit.pose.zoom})
        scalar(n);
    if (edit.tracks) for (std::size_t i=0;i<edit.tracks->size();++i) {
        const auto& track=(*edit.tracks)[i];
        append(track.has_value(),1);
        if (!track) continue;
        append(track->keys.size(),4); append(track->label.size(),2); append(track->layer.size(),2);
        bytes+=track->label; bytes+=track->layer;
        for (const auto& key:track->keys) {
            scalar(key.time); append(static_cast<vng::u32>(key.incoming),1);
            if (i==3) {
                const auto value=std::get<vng::Vec3>(key.value);
                scalar(value.x); scalar(value.y); scalar(value.z);
            }
            else scalar(std::get<vng::f32>(key.value));
        }
    }
    return bytes;
}
inline std::expected<AnimationCameraEdit,std::string> decode_animation_camera_edit(std::string_view bytes) {
    const bool legacy = bytes.substr(0,8)==std::string_view{"VNGACAM\1",8} ||
                        bytes.substr(0,8)==std::string_view{"VNGACAM\2",8};
    const bool tracks=bytes.substr(0,8)==std::string_view{"VNGACAM\2",8} ||
                      bytes.substr(0,8)==std::string_view{"VNGACAM\4",8};
    const auto pose_size = legacy ? 48u : 52u;
    if(bytes.size()<pose_size || bytes.size()>400000 ||
       (!legacy && !tracks && bytes.substr(0,8)!=std::string_view{"VNGACAM\3",8}) ||
       (!tracks && bytes.size()!=pose_size))
        return std::unexpected("Unknown animation-camera packet size or version");
    std::size_t offset=8;
    bool complete=true;
    const auto integer=[&](unsigned width) {
        if(width>bytes.size()-offset) { complete=false; return vng::u64{}; }
        vng::u64 n{};
        for(unsigned i=0;i<width;++i) n|=vng::u64(static_cast<unsigned char>(bytes[offset++]))<<(8*i);
        return n;
    };
    const auto scalar=[&] { return std::bit_cast<float>(static_cast<vng::u32>(integer(4))); };
    AnimationCameraEdit edit;
    edit.base_revision=integer(8); edit.revision=integer(8);
    for(float* n:{&edit.pose.yaw,&edit.pose.pitch,&edit.pose.distance,&edit.pose.target.x,&edit.pose.target.y,&edit.pose.target.z})
        *n=scalar();
    if (!legacy) edit.pose.zoom=scalar();
    if (tracks) {
        edit.tracks.emplace();
        for (std::size_t i=0;i<(legacy ? 4 : edit.tracks->size());++i) {
            const auto present=integer(1);
            if (!complete || present>1) return std::unexpected("Invalid camera track presence flag");
            if (!present) continue;
            const auto count=integer(4), label_size=integer(2), layer_size=integer(2);
            if (!complete || !count || count>vng::timeline::max_keys_per_track ||
                label_size>vng::timeline::max_label_bytes || layer_size>vng::timeline::max_layer_bytes ||
                label_size+layer_size+count*(i==3?17:9)>bytes.size()-offset)
                return std::unexpected("Invalid or truncated camera track payload");
            vng::timeline::Track track{{camera_animation_object,std::string(camera_track_properties[i])},
                std::string(bytes.substr(offset,label_size)),std::string(bytes.substr(offset+label_size,layer_size)),{}};
            offset+=label_size+layer_size;
            track.keys.reserve(count);
            for (vng::u64 n=0;n<count;++n) {
                const auto time=scalar();
                const auto incoming=static_cast<vng::timeline::Interpolation>(integer(1));
                vng::timeline::Value value;
                if (i==3) value=vng::Vec3{scalar(),scalar(),scalar()};
                else value=scalar();
                track.keys.push_back({time,std::move(value),incoming});
            }
            (*edit.tracks)[i]=std::move(track);
        }
    }
    if (!complete || offset!=bytes.size()) return std::unexpected("Truncated or trailing camera packet data");
    if(auto valid=validate_animation_camera_edit(edit);!valid) return std::unexpected(valid.error());
    return edit;
}
inline std::expected<void,std::string> apply_animation_camera_edit(Document& document,const AnimationCameraEdit& edit) {
    if(auto valid=validate_animation_camera_edit(edit,document.timeline_duration);!valid) return std::unexpected(valid.error());
    if(document.revision!=edit.base_revision) return std::unexpected("Stale animation-camera base revision");
    if (edit.tracks) {
        // Stage timeline data only. Meshes, instances, private view state and
        // GPU resources are neither copied nor reconstructed by camera edits.
        auto timeline=document.timeline;
        for (const auto property:camera_track_properties)
            (void)timeline.erase({camera_animation_object,std::string(property)});
        for (const auto& track:*edit.tracks) if(track)
            if (auto replaced=timeline.replace_track(*track);!replaced)
                return std::unexpected(replaced.error().message);
        document.timeline=std::move(timeline);
    }
    document.animation_camera=edit.pose;
    document.revision=edit.revision;
    return {};
}
} // namespace editor_example
