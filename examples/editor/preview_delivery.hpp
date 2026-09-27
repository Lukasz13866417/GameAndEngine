#pragma once
#include "component_dispatch.hpp"
#include "preview_updates.hpp"
#include "viewport_session.hpp"
#include "mesh_visibility.hpp"
#include "preview_viewport.hpp"
#include <vng/editor/preview.hpp>

namespace editor_example {
struct VisibilityDelivery { const MeshVisibility& visibility; vng::u64 revision; };
struct DeliveryContext {
    vng::u64 generation;
    const State& state;
    vng::editor::InteractionOrigin document_origin{}, view_origin{};
    bool document{};
    std::optional<VisibilityDelivery> view{};
};
struct DeliveryReply {
    std::string error{};
    bool submitted{};
    // Present only when a latest-value view packet was actually attempted.
    std::optional<bool> view_submission{};
};

// This boundary owns delivery bookkeeping, not scene state or process lifetime.
// Ordered document/visibility packets and replaceable views remain separate.
class PreviewDelivery final {
public:
    struct LiveLink {};
    struct StartingIndependentPlay {};
    explicit PreviewDelivery(vng::editor::preview::PreviewSession& transport) : transport_(transport) {}
    void add(vng::u64 generation) { updates_.add(generation); }
    void remove(vng::u64 generation) {
        updates_.remove(generation); extents_.erase(generation); views_.erase(generation); masks_.erase(generation);
    }
    void reset_document(vng::u64 generation) { updates_.reset(generation); }
    void reset_view(vng::u64 generation) { views_.erase(generation); masks_.erase(generation); }
    void changed(const DocumentChanges& changes) { updates_.changed(changes); }
    void acknowledge(vng::u64 generation,vng::u64 revision) { updates_.acknowledge(generation,revision); }
    void accepted(vng::u64 generation,vng::u64 revision) { updates_.accepted(generation,revision); }
    void reject(vng::u64 generation) { updates_.reject(generation); }
    [[nodiscard]] bool ready(vng::u64 generation,vng::u64 revision) const { return updates_.ready(generation,revision); }
    void retain_extents(vng::u64 active,const std::set<vng::u64>& candidates) {
        std::erase_if(extents_,[&](const auto& item){return item.first!=active&&!candidates.contains(item.first);});
    }
    void forget_extent(vng::u64 generation) { extents_.erase(generation); }
    std::string resize(vng::u64 generation,vng::Extent2D size) {
        if(!generation || (extents_.contains(generation)&&extents_.at(generation)==size)) return {};
        auto sent=transport_.send(encode_viewport_size(size),generation);
        if(!sent) return sent.error().message;
        extents_[generation]=size;
        return {};
    }
    [[nodiscard]] DebugReport debug_report() const {
        DebugReport report{.name="preview",.role="nonblocking document/view delivery",
            .situation=std::string(situation_),
            .owned={{"view peers",std::to_string(views_.size())},{"visibility peers",std::to_string(masks_.size())},
                {"requested extents",std::to_string(extents_.size())}},.children={updates_.debug_report()}};
        for(const auto& [generation,sequence]:views_)
            report.observations.push_back({"submitted view / "+std::to_string(generation),std::to_string(sequence)});
        return report;
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend struct Dispatcher;
    DeliveryReply handle(const LiveLink&,const DeliveryContext& c) {
        situation_="LiveLink";
        auto reply=c.document?document(c):DeliveryReply{};
        if(c.view) {
            auto view_reply=view(*c.view,c);
            if(!view_reply.error.empty()) reply.error=std::move(view_reply.error);
            reply.view_submission=view_reply.view_submission;
            reply.submitted|=view_reply.submitted;
        }
        return reply;
    }
    DeliveryReply document(const DeliveryContext& c) {
        auto packet=updates_.next(c.generation,c.state);
        if(!packet) return {.error=packet.error().message};
        if(!*packet) return {};
        auto sent=transport_.send(vng::editor::traced_message(**packet,c.document_origin),c.generation);
        if(!sent) { updates_.reset(c.generation); return {.error=sent.error().message}; }
        return {.submitted=true};
    }
    DeliveryReply view(const VisibilityDelivery& s,const DeliveryContext& c) {
        DeliveryReply reply;
        if(c.generation && masks_[c.generation]!=s.revision) {
            const auto bytes=encode_mesh_visibility({c.state.document.revision,s.visibility});
            if(auto sent=transport_.send("mesh_visibility\n"+bytes,c.generation); !sent) reply.error=sent.error().message;
            else masks_[c.generation]=s.revision;
        }
        if(!c.generation || views_[c.generation]==c.state.viewport.sequence) return reply;
        auto bytes=encode_viewport_request({c.state.document.revision,c.state.viewport});
        if(!bytes) { reply.error=bytes.error(); return reply; }
        const auto sent=transport_.send_latest(vng::editor::traced_message("view\n"+*bytes,c.view_origin),c.generation);
        reply.view_submission=sent.has_value();
        if(!sent) reply.error=sent.error().message;
        else { views_[c.generation]=c.state.viewport.sequence; reply.submitted=true; }
        return reply;
    }
    DeliveryReply handle(const StartingIndependentPlay&,const DeliveryContext& c) {
        situation_="StartingIndependentPlay";
        updates_.reset(c.generation);
        auto packet=updates_.next(c.generation,c.state);
        if(!packet || !*packet) return {.error=packet?"Cannot initialize Play":packet.error().message};
        (**packet).replace(0,9,"launch\n");
        auto sent=transport_.send(**packet,c.generation);
        if(!sent) { updates_.reset(c.generation); return {.error=sent.error().message}; }
        return {.submitted=true};
    }
    vng::editor::preview::PreviewSession& transport_;
    PreviewUpdates updates_;
    std::map<vng::u64,vng::u64> views_,masks_;
    std::map<vng::u64,vng::Extent2D> extents_;
    std::string_view situation_{"Not dispatched"};
};
} // namespace editor_example
