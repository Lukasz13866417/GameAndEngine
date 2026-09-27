#pragma once
#include "component_dispatch.hpp"
#include "preview_delivery_logic.hpp"
#include "viewport_session.hpp"
#include "mesh_visibility.hpp"
#include "preview_viewport.hpp"
#include "preview_mailbox.hpp"
#include <vng/editor/preview.hpp>
#include <charconv>
#include <deque>

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

// One behavior owner for process lifetime and delivery. Transport is its child,
// not a sibling reached through a mutable reference. The editor host supplies
// immutable document/view observations and polls worker outcomes.
class PreviewLogic final {
public:
    struct LiveLink {};
    struct StartingIndependentPlay {};
    struct Observation {
        vng::editor::preview::PreviewEvent event;
        bool schema_updated{};
    };
    [[nodiscard]] static vng::editor::preview::Result<PreviewLogic> create(
        vng::editor::preview::PreviewConfig config) {
        auto transport = vng::editor::preview::PreviewSession::create(std::move(config));
        if (!transport) return std::unexpected(transport.error());
        return PreviewLogic{std::move(*transport)};
    }
    auto request_reload() { return transport_.request_reload(); }
    void poll() {
        auto events=transport_.poll();
        for(auto& event:events) events_.push_back(std::move(event));
    }
    // Process one observation immediately before returning it to the host.
    // The host can react before the next lifecycle event changes these facts.
    [[nodiscard]] std::optional<Observation> take_event() {
        if(events_.empty())return {};
        Observation observed{std::move(events_.front())};events_.pop_front();
        const auto& event=observed.event;
        {
            using Kind=vng::editor::preview::EventKind;
            if(event.kind==Kind::candidate_started) {
                candidates_.insert(event.generation);
                add(event.generation);
            } else if(event.kind==Kind::activated) {
                frames_.clear();
                candidates_.erase(event.generation);
                retain_extents(event.generation,candidates_);
                std::erase_if(schemas_,[&](const auto& entry) {
                    const bool obsolete=entry.first!=event.generation&&!candidates_.contains(entry.first);
                    if(obsolete) remove(entry.first);
                    return obsolete;
                });
            } else if(event.kind==Kind::worker_failed || event.kind==Kind::build_failed) {
                candidates_.erase(event.generation);
                forget_extent(event.generation);
                if(event.kind==Kind::worker_failed) {
                    schemas_.erase(event.generation);
                    remove(event.generation);
                }
            } else if(event.kind==Kind::message) {
                const std::string_view message=event.message;
                if(message.starts_with("revision\n")) {
                    vng::u64 revision{};
                    const auto text=message.substr(9);
                    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),revision);
                    if(error==std::errc{}&&end==text.data()+text.size()) acknowledge(event.generation,revision);
                } else if(message.starts_with("resync\n")) reset_document(event.generation);
                else if(message.starts_with("schema\n")) {
                    if(auto schema=vng::editor::decode_schema(message.substr(7))) {
                        schemas_.insert_or_assign(event.generation,std::move(*schema));
                        observed.schema_updated=true;
                    }
                }
            }
        }
        return observed;
    }
    auto send(std::string_view message, vng::u64 generation = 0) { return transport_.send(message,generation); }
    auto flush_requests() { return transport_.flush_requests(); }
    auto take_frame(vng::u64 minimum_revision,vng::u64 displayed_revision,vng::u64 authored_revision) {
        if(auto arrived=transport_.take_latest_frame()) frames_.offer(std::move(*arrived),active_generation());
        return frames_.take(active_generation(),minimum_revision,displayed_revision,authored_revision);
    }
    void discard_frames() { frames_.clear(); }
    [[nodiscard]] const auto& candidates() const { return candidates_; }
    [[nodiscard]] const auto& schemas() const { return schemas_; }
    [[nodiscard]] vng::u64 active_generation() const { return transport_.active_generation(); }
    [[nodiscard]] bool busy() const { return transport_.busy(); }
    [[nodiscard]] auto logs() const { return transport_.logs(); }
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
        DebugReport report{.name="preview",.role="worker lifecycle, nonblocking delivery and completed-frame adoption",
            .situation=std::string(situation_),
            .owned={{"view peers",std::to_string(views_.size())},{"visibility peers",std::to_string(masks_.size())},
                {"requested extents",std::to_string(extents_.size())},
                {"candidate workers",std::to_string(candidates_.size())},
                {"unobserved worker events",std::to_string(events_.size())},
                {"schema peers",std::to_string(schemas_.size())},
                {"active generation",std::to_string(active_generation())}},.children={updates_.debug_report()}};
        for(const auto& [generation,sequence]:views_)
            report.observations.push_back({"submitted view / "+std::to_string(generation),std::to_string(sequence)});
        return report;
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    explicit PreviewLogic(vng::editor::preview::PreviewSession transport) : transport_(std::move(transport)) {}
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
    vng::editor::preview::PreviewSession transport_;
    std::deque<vng::editor::preview::PreviewEvent> events_;
    std::set<vng::u64> candidates_;
    std::map<vng::u64,vng::editor::Schema> schemas_;
    PreviewMailbox frames_;
    PreviewDeliveryLogic updates_;
    std::map<vng::u64,vng::u64> views_,masks_;
    std::map<vng::u64,vng::Extent2D> extents_;
    std::string_view situation_{"Not dispatched"};
};
} // namespace editor_example
