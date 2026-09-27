#pragma once
#include "surface_move_tool.hpp"
#include <cmath>

namespace editor_example {
// Passive, blueprint-declared socket markers. The host routes a click only
// while attachment mode is active; this tool never edits or owns a document.
class SocketPickTool {
public:
    void update(std::span<const SurfaceMove> slots,const vng::gfx::CameraSnapshot& camera,vng::ui::Rect viewport) {
        markers_.resize(slots.size());
        for(std::size_t i=0;i<slots.size();++i)(void)markers_[i].update(slots[i],camera,viewport,{},{},true,true);
    }
    [[nodiscard]] std::optional<std::size_t> hit(vng::Vec2 p) const {
        std::optional<std::size_t> result;float nearest=13;
        for(std::size_t i=0;i<markers_.size();++i)if(const auto h=markers_[i].handle()) {
            const auto distance=std::hypot(p.x-h->x,p.y-h->y);if(distance<nearest){nearest=distance;result=i;}
        }
        return result;
    }
    [[nodiscard]] std::vector<std::optional<vng::Vec2>> handles() const {
        std::vector<std::optional<vng::Vec2>> result;for(const auto& marker:markers_)result.push_back(marker.handle());return result;
    }
    void clear(){markers_.clear();}
    void append(vng::ui::DrawList& list,const vng::text::Font& font) const {
        for(const auto& marker:markers_)marker.append_marker(list,font);
    }
private:
    std::vector<SurfaceMoveTool> markers_;
};
}
