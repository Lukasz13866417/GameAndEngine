#pragma once

#include <vng/core/monotonic_clock.hpp>
#include <vng/input/input.hpp>
#include <algorithm>
#include <deque>
#include <expected>
#include <iomanip>
#include <locale>
#include <sstream>
#include <span>
#include <array>
#include <vector>
#include <string>

namespace vng::editor {
// Only CLOCK_MONOTONIC CPU timestamps. These do not claim mouse-hardware,
// GPU execution, compositor, or physical scanout timing.
struct InteractionOrigin {
    u64 id{}, first_input_ns{}, last_input_ns{}, input_count{}, ui_applied_ns{}, sent_ns{};
    bool estimated_input{};
    friend bool operator==(const InteractionOrigin&, const InteractionOrigin&) = default;
};
struct InteractionTrace {
    InteractionOrigin origin;
    u64 worker_received_ns{}, worker_applied_ns{}, render_started_ns{}, readback_ready_ns{};
    u64 published_ns{}, ui_received_ns{}, presentation_submit_ns{}, presentation_return_ns{};
};
// Fixed-size metadata envelope: payload codecs remain responsible for their own
// contracts. Generation/lifetime identity belongs to the preview connection.
[[nodiscard]] inline std::string traced_message(std::string_view payload, InteractionOrigin source) {
    if (!source.id) return std::string(payload);
    source.sent_ns = monotonic_ns();
    std::string result{"VNGTRACE\1", 9};
    for (u64 value : {source.id, source.first_input_ns, source.last_input_ns, source.input_count,
                     source.ui_applied_ns, source.sent_ns, u64(source.estimated_input)})
        for (unsigned i = 0; i < 8; ++i) result += static_cast<char>((value >> (8*i)) & 255);
    result += payload;
    return result;
}
struct ReceivedInteraction { std::string_view payload; InteractionOrigin origin; };
[[nodiscard]] inline std::expected<ReceivedInteraction, std::string> receive_interaction(std::string_view bytes) {
    if (!bytes.starts_with("VNGTRACE")) return ReceivedInteraction{bytes, {}};
    if (bytes.size() <= 65 || bytes.substr(0, 9) != std::string_view{"VNGTRACE\1", 9})
        return std::unexpected("Invalid interaction trace envelope");
    std::array<u64, 7> values{};
    for (std::size_t n = 0; n < values.size(); ++n)
        for (unsigned i = 0; i < 8; ++i)
            values[n] |= u64(static_cast<unsigned char>(bytes[9+n*8+i])) << (8*i);
    if (!values[0] || !values[1] || values[1] > values[2] || !values[3] ||
        values[2] > values[4] || values[4] > values[5] || values[6] > 1)
        return std::unexpected("Invalid interaction timestamp order");
    return ReceivedInteraction{bytes.substr(65), {values[0], values[1], values[2], values[3],
        values[4], values[5], bool(values[6])}};
}

struct InteractionSample {
    u64 generation{}, document_revision{}, view_sequence{}, frame_id{};
    InteractionTrace trace;
    u64 settled_submit_ns{};
};
class InteractionTimings {
public:
    static constexpr std::size_t capacity = 256;
    void begin_tick(std::span<const input::Event> events) {
        tick_ = {}; tick_events_ = events;
    }
    // Called by the common mutation/router paths, not individual widgets.
    [[nodiscard]] InteractionOrigin interaction() {
        if (!enabled_ || tick_.id || tick_events_.empty()) return tick_;
        const auto now = monotonic_ns();
        if (!now) return {};
        tick_.id = ++next_id_;
        tick_.ui_applied_ns = now;
        for (const auto& event : tick_events_) {
            auto stamp = event.received_ns;
            if (!stamp || stamp > now) { stamp = now; tick_.estimated_input = true; }
            if (!tick_.first_input_ns || stamp < tick_.first_input_ns) tick_.first_input_ns = stamp;
            tick_.last_input_ns = std::max(tick_.last_input_ns, stamp);
            ++tick_.input_count;
        }
        return tick_;
    }
    void enabled(bool value) { enabled_ = value; }
    [[nodiscard]] bool enabled() const { return enabled_; }
    void clear() {
        samples_.clear(); intervals_.clear();
        discard_through_id_ = std::max(next_id_,highest_observed_id_);
        last_frame_ = last_generation_ = last_present_ = 0;
    }
    void presented(u64 generation, u64 document, u64 view, u64 frame,
                   InteractionTrace trace, bool settled, u64 submit, u64 returned) {
        if (!enabled_ || !submit || returned < submit) return;
        if (generation == last_generation_ && frame <= last_frame_) return;
        if (generation == last_generation_ && last_present_ && submit >= last_present_) {
            if (intervals_.size() == capacity) intervals_.pop_front();
            intervals_.push_back(double(submit - last_present_) / 1e6);
        }
        last_generation_ = generation; last_frame_ = frame; last_present_ = submit;
        if (!trace.origin.id || trace.origin.id <= discard_through_id_ || !trace.origin.last_input_ns || submit < trace.origin.last_input_ns ||
            trace.origin.ui_applied_ns < trace.origin.last_input_ns ||
            trace.origin.sent_ns < trace.origin.ui_applied_ns ||
            trace.worker_received_ns < trace.origin.sent_ns ||
            trace.worker_applied_ns < trace.worker_received_ns ||
            trace.render_started_ns < trace.worker_applied_ns ||
            trace.readback_ready_ns < trace.render_started_ns ||
            trace.published_ns < trace.readback_ready_ns ||
            trace.ui_received_ns < trace.published_ns || submit < trace.ui_received_ns) return;
        highest_observed_id_ = std::max(highest_observed_id_,trace.origin.id);
        auto existing = std::ranges::find_if(samples_, [&](const auto& sample) {
            return sample.generation == generation && sample.trace.origin.id == trace.origin.id;
        });
        if (existing != samples_.end()) {
            if (settled && !existing->settled_submit_ns) existing->settled_submit_ns = submit;
            return;
        }
        trace.presentation_submit_ns = submit;
        trace.presentation_return_ns = returned;
        if (samples_.size() == capacity) samples_.pop_front();
        samples_.push_back({generation, document, view, frame, trace, settled ? submit : 0});
    }
    [[nodiscard]] const std::deque<InteractionSample>& samples() const { return samples_; }
    [[nodiscard]] std::string summary() const {
        std::ostringstream out; out.imbue(std::locale::classic()); out << std::fixed << std::setprecision(2);
        out << samples_.size() << " / " << capacity << " interaction samples\n";
        if (samples_.empty()) return out.str() + "Navigate or edit the scene to collect timings.";
        std::vector<double> latency;
        for (const auto& s : samples_) latency.push_back(ms(s.trace.presentation_submit_ns, s.trace.origin.last_input_ns));
        std::ranges::sort(latency);
        const auto median=(latency[(latency.size()-1)/2]+latency[latency.size()/2])*.5;
        out << "Input -> first presentation submit\nMedian: " << median
            << " ms / p95: " << latency[(latency.size()*95+99)/100-1] << " ms\n";
        const auto& s = samples_.back(); const auto& t = s.trace;
        out << "Latest interaction #" << t.origin.id << " / view " << s.view_sequence
            << " / " << t.origin.input_count << " input event(s)\n"
            << "UI input handling: " << ms(t.origin.ui_applied_ns,t.origin.last_input_ns) << " ms\n"
            << "Send + worker delivery: " << ms(t.worker_received_ns,t.origin.ui_applied_ns) << " ms\n"
            << "Worker apply + scheduling: " << ms(t.render_started_ns,t.worker_received_ns) << " ms\n"
            << "Render + readback: " << ms(t.readback_ready_ns,t.render_started_ns) << " ms\n"
            << "Transfer + UI presentation: " << ms(t.presentation_submit_ns,t.readback_ready_ns) << " ms\n";
        if (s.settled_submit_ns) out << "Target settled: " << ms(s.settled_submit_ns,t.origin.last_input_ns) << " ms\n";
        else out << "Target not settled (or superseded)\n";
        if (!intervals_.empty()) {
            std::vector<double> values(intervals_.begin(), intervals_.end()); std::ranges::sort(values);
            out << "New-preview gap p95 (includes idle): " << values[(values.size()*95+99)/100-1] << " ms\n";
        }
        out << "CPU monotonic timing; not GPU time or monitor scanout.";
        return out.str();
    }
    [[nodiscard]] std::string json() const {
        std::ostringstream out; out.imbue(std::locale::classic());
        out << "{\"clock\":\"Linux CLOCK_MONOTONIC nanoseconds\",\"scope\":\"callback reception to presentation call, not hardware or scanout latency\",\"capacity\":" << capacity << ",\"samples\":[";
        bool first = true;
        for (const auto& s : samples_) {
            const auto& t=s.trace; const auto& o=t.origin;
            if (!first) out << ',';
            first=false;
            out << "{\"id\":" << o.id << ",\"generation\":" << s.generation << ",\"document_revision\":" << s.document_revision
                << ",\"view_sequence\":" << s.view_sequence << ",\"frame_id\":" << s.frame_id
                << ",\"first_input_ns\":" << o.first_input_ns << ",\"last_input_ns\":" << o.last_input_ns
                << ",\"input_count\":" << o.input_count << ",\"estimated_input\":" << (o.estimated_input ? "true":"false")
                << ",\"ui_applied_ns\":" << o.ui_applied_ns << ",\"sent_ns\":" << o.sent_ns
                << ",\"worker_received_ns\":" << t.worker_received_ns << ",\"worker_applied_ns\":" << t.worker_applied_ns
                << ",\"render_started_ns\":" << t.render_started_ns << ",\"readback_ready_ns\":" << t.readback_ready_ns
                << ",\"published_ns\":" << t.published_ns << ",\"ui_received_ns\":" << t.ui_received_ns
                << ",\"presentation_submit_ns\":" << t.presentation_submit_ns << ",\"presentation_return_ns\":" << t.presentation_return_ns
                << ",\"settled_submit_ns\":" << s.settled_submit_ns << '}';
        }
        return out.str() + "]}\n";
    }
private:
    static double ms(u64 end, u64 begin) { return end >= begin ? double(end-begin)/1e6 : 0; }
    bool enabled_{true};
    u64 next_id_{}, last_generation_{}, last_frame_{}, last_present_{};
    u64 highest_observed_id_{}, discard_through_id_{};
    InteractionOrigin tick_;
    std::span<const input::Event> tick_events_;
    std::deque<InteractionSample> samples_;
    std::deque<double> intervals_;
};
} // namespace vng::editor
