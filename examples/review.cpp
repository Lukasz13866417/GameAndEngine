// vng_review: play saved scenes side by side on one clock, click anything you
// can see to pin a note to it, and write down what you think of each one.
// Everything lives in a .vreview file (examples/review/review_file.hpp) that
// an agent can read and answer. The app saves it as you type, takes in what
// is written to it elsewhere while it is open, and reloads a scene whose
// file changes.
#include "review/candidate_view.hpp"
#include "review/review_file.hpp"
#include "support/glfw_opengl_session.hpp"
#include "support/presentation.hpp"
#include "support/window_loop.hpp"
#include <vng/ui_opengl/ui_renderer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <tuple>
#include <utility>

namespace {
using namespace vng;
namespace project = editor_example;
using Clock = std::chrono::steady_clock;

constexpr std::string_view help = R"(vng_review: review saved scenes, alone or side by side, and leave notes.

  vng_review REVIEW.vreview
      Open a review. Click anything in a view to pin a note to it; the notes,
      your thoughts on each candidate and your overall summary save as you type.

  vng_review --new REVIEW.vreview --candidate LABEL SCENE.vscene [--about TEXT] [--offset S] ...
             [--title TEXT] [--range FROM TO] [--replace] [--no-open]
      Create a review (1 candidate: a single review; 2 to 4: a comparison), then open it.
      --about and --offset apply to the candidate before them. --range limits the clock.

  --at SECONDS       start the clock here            --play        start playing
  --note ID X Y TEXT pin a note on candidate ID at the normalized view point X,Y
                     (0..1, top-left) at the start time, as a click there would
  --frames N         quit after N frames and print the frame rate
  --hidden           keep the window off screen (for scripted runs; with --hidden or
                     --screenshot, exit code 77 means no OpenGL window could be made)
  --screenshot NEW.png
      Render the review in a hidden window once every view shows the start time
      (after any --note pins), save the picture and exit.

Keys (outside text fields): Space play/pause, Left/Right step 1/30 s, Home start, Escape deselect.

While it is open, the app takes in edits made to the file elsewhere (an agent's
replies, say) and reopens the review when its candidates change or a scene
file is rewritten.
)";

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}
std::string describe(const content::Diagnostic& e) {
    auto where = e.path.string();
    if (!where.empty() && e.location) where += std::format(":{}:{}", e.location->line, e.location->column);
    return (where.empty() ? "" : where + ": ") + (e.property_path.empty() ? "" : e.property_path + ": ") + e.message;
}
f32 number(std::string_view text, bool& ok) {
    try {
        std::size_t used{};
        const auto value = std::stof(std::string(text), &used);
        ok = ok && used == text.size() && std::isfinite(value);
        return value;
    } catch (...) {
        ok = false;
        return 0;
    }
}

struct Options {
    std::filesystem::path file;
    bool create{}, replace{}, open{true}, help{};
    std::string title;
    std::optional<std::array<f32, 2>> range;
    std::vector<review::Candidate> candidates;
    std::optional<std::filesystem::path> screenshot;
    std::optional<f32> at;
    struct Pin { std::string candidate; Vec2 view; std::string text; };
    std::vector<Pin> pins;
    bool play{}, hidden{};
    std::optional<u64> frames;
};
std::expected<Options, std::string> parse(int argc, char** argv) {
    Options o;
    bool ok = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto more = [&](int n) { return i + n < argc; };
        if (arg == "--help") o.help = true;
        else if (arg == "--new" && more(1)) { o.create = true; o.file = argv[++i]; }
        else if (arg == "--replace") o.replace = true;
        else if (arg == "--no-open") o.open = false;
        else if (arg == "--title" && more(1)) o.title = argv[++i];
        else if (arg == "--range" && more(2)) {
            const auto from = number(argv[i + 1], ok), to = number(argv[i + 2], ok);
            o.range = std::array{from, to};
            i += 2;
        } else if (arg == "--candidate" && more(2)) {
            review::Candidate c;
            c.id = std::string(1, static_cast<char>('A' + o.candidates.size()));
            c.label = argv[i + 1];
            c.scene = std::filesystem::absolute(argv[i + 2]).lexically_normal();
            o.candidates.push_back(std::move(c));
            i += 2;
        } else if ((arg == "--about" || arg == "--offset") && more(1)) {
            if (o.candidates.empty()) return std::unexpected(std::string(arg) + " must follow a --candidate");
            if (arg == "--about") o.candidates.back().about = argv[++i];
            else o.candidates.back().offset = number(argv[++i], ok);
        } else if (arg == "--note" && more(4)) {
            o.pins.push_back({argv[i + 1], {number(argv[i + 2], ok), number(argv[i + 3], ok)}, argv[i + 4]});
            i += 4;
        } else if (arg == "--play") o.play = true;
        else if (arg == "--hidden") o.hidden = true;
        else if (arg == "--frames" && more(1)) {
            const auto count = number(argv[++i], ok);
            ok = ok && count >= 1;
            o.frames = static_cast<u64>(count);
        } else if (arg == "--screenshot" && more(1)) o.screenshot = argv[++i];
        else if (arg == "--at" && more(1)) o.at = number(argv[++i], ok);
        else if (!arg.starts_with("--") && o.file.empty()) o.file = arg;
        else return std::unexpected("Unknown or incomplete argument \"" + std::string(arg) + "\"; see --help");
        if (!ok) return std::unexpected("A number argument is not a finite number; see --help");
    }
    if (o.help) return o;
    if (o.file.empty()) return std::unexpected("Name a .vreview file; see --help");
    if (o.create && o.candidates.empty()) return std::unexpected("--new needs at least one --candidate");
    if (!o.create && !o.candidates.empty()) return std::unexpected("--candidate needs --new");
    if (o.candidates.size() > review::max_candidates) return std::unexpected("At most 4 candidates fit side by side");
    return o;
}

// A candidate's colour on the timeline and in its header.
Vec4 tint(std::size_t i) {
    constexpr std::array<Vec4, 4> colors{Vec4{1, .62F, .2F, 1}, Vec4{.3F, .78F, 1, 1}, Vec4{.9F, .45F, .95F, 1}, Vec4{.55F, .9F, .35F, 1}};
    return colors[i % colors.size()];
}
std::string clock_text(f32 t) { return std::format("{:.2f} s", t); }
std::string first_line(std::string_view text, std::size_t limit) {
    auto line = text.substr(0, text.find('\n'));
    if (line.size() > limit) return std::string(line.substr(0, limit)) + "...";
    return std::string(line);
}
// Greedy word wrap by measured width, for read-only text. A word too long
// for a line (a path, say) breaks between characters.
std::vector<std::string> wrap(const text::Font& font, std::string_view text, f32 width, u32 size) {
    std::vector<std::string> lines;
    const auto fits = [&](const std::string& s) {
        const auto metrics = font.measure(s, size);
        return metrics && metrics->width <= width;
    };
    for (std::size_t begin = 0; begin <= text.size();) {
        const auto end = std::min(text.find('\n', begin), text.size());
        std::string line;
        for (std::size_t word = begin; word < end;) {
            const auto space = std::min(text.find(' ', word), end);
            auto piece = std::string(text.substr(word, space - word));
            if (!line.empty() && !fits(line + ' ' + piece)) lines.push_back(std::exchange(line, {}));
            if (!line.empty()) line += ' ';
            while (!fits(line + piece)) {
                // The longest start of the word that fits, cut at a UTF-8 character boundary.
                std::size_t cut = piece.size();
                while (cut > 1 && (!fits(line + piece.substr(0, cut)) || (static_cast<unsigned char>(piece[cut]) & 0xC0) == 0x80)) --cut;
                lines.push_back(line + piece.substr(0, cut));
                line.clear();
                piece.erase(0, cut);
            }
            line += piece;
            word = space + 1;
        }
        lines.push_back(std::move(line));
        begin = end + 1;
    }
    return lines;
}

struct Layout {
    ui::Rect sidebar, bar;
    std::vector<ui::Rect> headers, images;
};
// Views share the space left of the sidebar and above the time bar; each
// keeps the 16:10 frame the shots were composed for.
Layout layout(Vec2 size, std::size_t count) {
    constexpr f32 side = 420, bar = 112, gap = 8, header = 40;
    Layout l;
    l.sidebar = {size.x - side, 0, side, size.y};
    l.bar = {0, size.y - bar, std::max(size.x - side, 1.F), bar};
    const f32 width = std::max(size.x - side, 1.F), height = std::max(size.y - bar, 1.F);
    // The grid that makes the views largest: side by side, stacked or 2x2.
    std::size_t columns = 1, rows = count;
    f32 best{};
    for (std::size_t c = 1; c <= count; ++c) {
        const auto r = (count + c - 1) / c;
        const f32 w = (width - gap * static_cast<f32>(c + 1)) / static_cast<f32>(c);
        const f32 h = (height - gap * static_cast<f32>(r + 1)) / static_cast<f32>(r) - header - 4;
        const f32 image_w = std::min(w, h * 1.6F);
        if (image_w > best) { best = image_w; columns = c; rows = r; }
    }
    const f32 cell_w = (width - gap * static_cast<f32>(columns + 1)) / static_cast<f32>(columns);
    const f32 cell_h = (height - gap * static_cast<f32>(rows + 1)) / static_cast<f32>(rows);
    for (std::size_t i = 0; i < count; ++i) {
        const f32 x = gap + static_cast<f32>(i % columns) * (cell_w + gap);
        const f32 y = gap + static_cast<f32>(i / columns) * (cell_h + gap);
        l.headers.push_back({x, y, std::max(cell_w, 1.F), header});
        const f32 room_h = std::max(cell_h - header - 4, 1.F);
        const f32 w = std::min(cell_w, room_h * 1.6F), h = w / 1.6F;
        l.images.push_back({x + (cell_w - w) * .5F, y + header + 4 + (room_h - h) * .5F, std::max(w, 1.F), std::max(h, 1.F)});
    }
    return l;
}
void place(auto&& widget, ui::Rect r) { widget.position({r.x, r.y}).width(r.width).height(r.height); }
std::string row_text(const review::Note& n) {
    const auto what = n.text.empty() ? (n.object ? n.object_name : std::string("background")) : n.text;
    const auto state = n.status == "resolved" ? " (resolved)" : n.reply.empty() ? "" : " (replied)";
    return std::format("#{} {} {}{}  {}", n.id, n.candidate, clock_text(n.time), state, first_line(what, 26));
}

// A candidate's view, or why it has none, and the version of the scene file it shows.
struct Slot {
    std::filesystem::path scene;
    std::optional<review::FileStamp> stamp;
    std::optional<review::CandidateView> view;
    std::string error;
};
// Loads the candidates' scenes in parallel (the heavy ones take seconds),
// carrying over the views of scene files that have not changed. A scene that
// does not load leaves its slot without a view, saying why; the rest open.
std::vector<Slot> load_slots(opengl::Device& device, const review::Review& r, std::vector<Slot> reusable,
                             const std::function<void(std::size_t)>& loading_count) {
    std::vector<Slot> slots(r.candidates.size());
    std::vector<std::future<content::Result<project::State>>> loading(slots.size());
    std::size_t count{};
    for (std::size_t i = 0; i < slots.size(); ++i) {
        auto& slot = slots[i];
        slot.scene = r.candidates[i].scene;
        slot.stamp = review::stamp_of(slot.scene);
        const auto same = std::ranges::find_if(reusable, [&](const Slot& old) {
            return old.view && old.stamp && old.scene == slot.scene && old.stamp == slot.stamp;
        });
        if (same != reusable.end()) {
            slot.view.emplace(std::move(*same->view));
            same->view.reset();
            continue;
        }
        loading[i] = std::async(std::launch::async, [scene = slot.scene] { return review::CandidateView::load_scene(scene); });
        ++count;
    }
    if (count) loading_count(count);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!loading[i].valid()) continue;
        auto state = loading[i].get();
        if (!state) {
            slots[i].error = describe(state.error());
            continue;
        }
        auto view = review::CandidateView::create(device, std::move(*state));
        if (view) slots[i].view.emplace(std::move(*view));
        else slots[i].error = view.error().message;
    }
    return slots;
}
// What an app's views and clock are built from; everything else it takes in while open.
auto structure(const review::Review& r) {
    std::vector<std::tuple<std::string, std::filesystem::path, f32>> candidates;
    for (const auto& c : r.candidates) candidates.emplace_back(c.id, c.scene, c.offset);
    return std::pair{std::move(candidates), r.range};
}

// The review as an app holds it: its edits, and the file as it last read or
// wrote it (with that version's stamp), which the next merge starts from.
struct Edits {
    review::Review review, disk;
    std::optional<review::FileStamp> stamp;
};
// What a reopened review keeps from the app before it.
struct Carry {
    Edits edits;
    std::optional<f32> time;
    f32 speed{1};
    bool playing{}, loop{true};
    std::string candidate, status;
    u32 note{};
};

class App {
public:
    App(std::filesystem::path file, Carry carry, std::vector<Slot> slots, text::Font font)
        : file_(std::move(file)), review_(std::move(carry.edits.review)), disk_(std::move(carry.edits.disk)),
          disk_stamp_(carry.edits.stamp), slots_(std::move(slots)), font_(std::move(font)),
          screen_(std::make_shared<ui::BasicTheme>([&] {
              auto values = ui::dark_theme_values(font_);
              values.panel.w = 1;
              return values;
          }())) {
        dirty_ = review_ != disk_;
        edited_ = Clock::now();
        f32 end{};
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].view) end = std::max(end, slots_[i].view->duration() - review_.candidates[i].offset);
        range_ = review_.range.value_or(std::array{0.F, std::max(end, .1F)});
        speed_ = carry.speed;
        loop_ = carry.loop;
        build();
        at(carry.time.value_or(range_[0]));
        playing_ = carry.playing;
        select_candidate(candidate_index(carry.candidate));
        show_note(carry.note);
        say(carry.status);
    }
    void at(f32 t) { time_ = std::clamp(t, range_[0], range_[1]); playing_ = false; }
    void play() { playing_ = true; }
    // Pin a note as a click at a normalized point of a candidate's view would.
    bool pin(std::string_view candidate, Vec2 view, std::string_view text) {
        if (!review_.find(candidate) || !(view.x >= 0 && view.x <= 1 && view.y >= 0 && view.y <= 1)) return false;
        const auto b = images_[candidate_index(candidate)].bounds();
        click({b.x + view.x * b.width, b.y + view.y * b.height}, true);
        if (auto* note = selected_note()) {
            note->text = std::string(text);
            note_text_.value(text);
            touched();
            show_note(note->id);
        }
        return true;
    }
    [[nodiscard]] bool frame_ready() const {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].view) continue;
            const auto& view = *slots_[i].view;
            const auto shown = view.shown_time();
            if (!shown || std::abs(*shown - std::clamp(time_ + review_.candidates[i].offset, 0.F, view.duration())) > 1e-4F)
                return false;
        }
        return true;
    }
    [[nodiscard]] std::string status() const { return status_; }
    // How many frames each view has shown, for --frames reports.
    [[nodiscard]] std::string shown_frames() const {
        std::string out;
        for (std::size_t i = 0; i < slots_.size(); ++i)
            out += std::format("{}{} {}", i ? ", " : "", review_.candidates[i].id,
                               slots_[i].view ? std::to_string(slots_[i].view->revision()) : "(no view)");
        return out;
    }
    // Set when the file or a scene changed in a way these views cannot show;
    // the caller then opens a new app from carry() and take_slots().
    [[nodiscard]] bool wants_reopen() const { return reopen_.has_value(); }
    [[nodiscard]] Carry carry() const {
        Carry c;
        c.edits = reopen_ ? *reopen_ : Edits{review_, disk_, disk_stamp_};
        c.time = time_;
        c.speed = speed_;
        c.playing = playing_;
        c.loop = loop_;
        c.candidate = review_.candidates[selected_].id;
        c.status = status_;
        c.note = note_;
        return c;
    }
    std::vector<Slot> take_slots() { return std::move(slots_); }

    // One frame: input, interaction, scene renders, then the UI's draw list.
    std::expected<std::optional<ui::DrawList>, std::string> step(input::Frame raw, input::Clipboard* clipboard, f32 dt, opengl::Device& device) {
        poll();
        if (reopen_) return std::optional<ui::DrawList>{};
        const Vec2 size = raw.logical_size;
        if (size.x < 1 || size.y < 1) return std::optional<ui::DrawList>{};
        if (size.x != size_.x || size.y != size_.y) arrange(size);
        auto input = screen_.update(raw, dt, clipboard);
        if (!input) return std::unexpected(input.error().message);
        interact(*input, raw);
        if (playing_) {
            time_ += dt * speed_;
            if (time_ > range_[1]) {
                if (loop_) time_ = range_[0] + std::fmod(time_ - range_[0], range_[1] - range_[0]);
                else { time_ = range_[1]; playing_ = false; }
            }
        }
        sync_controls();
        const f32 scale_x = static_cast<f32>(raw.framebuffer.width) / size.x, scale_y = static_cast<f32>(raw.framebuffer.height) / size.y;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].view) continue;
            auto& view = *slots_[i].view;
            const auto b = images_[i].bounds();
            const Extent2D extent{static_cast<u32>(std::lround(b.width * scale_x)), static_cast<u32>(std::lround(b.height * scale_y))};
            if (auto rendered = view.update(device, time_ + review_.candidates[i].offset, extent); !rendered)
                return std::unexpected(rendered.error().message);
            if (view.revision() != shown_revision_[i] && view.image()) {
                images_[i].image(view.image(), view.revision());
                shown_revision_[i] = view.revision();
            }
        }
        autosave(false);
        if (reopen_) return std::optional<ui::DrawList>{};
        auto list = screen_.draw_list();
        if (!list) return std::unexpected(list.error().message);
        markers(*list);
        return std::optional{std::move(*list)};
    }
    // Saves 0.7 s after the last edit, or now, first merging in whatever was
    // written to the file elsewhere, so that nothing written there is lost.
    // Leaving (now), it also writes over a file that no longer reads as a
    // review, keeping that file beside it.
    bool autosave(bool now) {
        if (!now && (!dirty_ || Clock::now() - edited_ < std::chrono::milliseconds(700))) return true;
        const bool readable = pull();
        if (reopen_ && !now) return true; // the reopened app saves the merge
        if (readable && !dirty_) return true;
        std::optional<std::filesystem::path> backup;
        if (!readable) {
            if (!now) return false; // until whoever is writing it is done
            backup = file_;
            *backup += ".unreadable";
        }
        auto& edits = reopen_ ? reopen_->review : review_;
        auto saved = review::save_review(file_, edits, backup);
        if (!saved) { say("Not saved: " + saved.error().message); return false; }
        if (backup) std::cerr << "The review file was not readable; kept it as " << backup->string() << '\n';
        if (reopen_) {
            reopen_->disk = reopen_->review;
            reopen_->stamp = *saved;
        } else {
            disk_ = review_;
            disk_stamp_ = *saved;
        }
        dirty_ = false;
        say("Saved " + review::timestamp().substr(11, 8) + " UTC");
        return true;
    }

private:
    void say(std::string text) {
        status_ = std::move(text);
        status_label_.text(status_);
    }
    // Twice a second: scene files rewritten on disk, then edits to the review file.
    void poll() {
        if (Clock::now() - polled_ < std::chrono::milliseconds(500)) return;
        polled_ = Clock::now();
        for (const auto& slot : slots_)
            if (review::stamp_of(slot.scene) != slot.stamp) {
                reopen_ = Edits{review_, disk_, disk_stamp_};
                return;
            }
        pull();
    }
    // Takes in what was written to the file elsewhere since this app last read
    // or wrote it. False while the file holds something that is not a review.
    bool pull() {
        const auto stamp = review::stamp_of(file_);
        if (!stamp || stamp == disk_stamp_) return true; // a deleted file is written again on the next save
        if (stamp == unreadable_) return false;
        auto theirs = review::read_review(file_);
        if (!theirs) {
            unreadable_ = stamp;
            std::cerr << describe(theirs.error()) << '\n';
            say("The file changed and does not read as a review; your edits wait for it.");
            return false;
        }
        unreadable_.reset();
        if (*theirs == disk_) {
            disk_stamp_ = stamp;
            return true;
        }
        auto merged = review::merge(disk_, review_, *theirs);
        if (structure(merged) != structure(review_)) {
            // Other candidates, scenes or times need other views.
            reopen_ = Edits{std::move(merged), std::move(*theirs), stamp};
            return true;
        }
        disk_ = std::move(*theirs);
        disk_stamp_ = stamp;
        adopt(std::move(merged));
        dirty_ = review_ != disk_;
        say("Took in changes to the file " + review::timestamp().substr(11, 8) + " UTC");
        return true;
    }
    // Shows a merged review with the same candidates, keeping the selection
    // (a note the merge renumbered is followed to its new id).
    void adopt(review::Review merged) {
        std::optional<std::tuple<std::string, std::string, Vec2>> selected;
        if (const auto* n = selected_note()) selected = std::tuple{n->candidate, n->created, n->view};
        review_ = std::move(merged);
        if (selected) {
            const auto same = std::ranges::find_if(review_.notes, [&](const review::Note& n) {
                return std::tuple{n.candidate, n.created, n.view} == *selected;
            });
            note_ = same == review_.notes.end() ? 0 : same->id;
        }
        title_.text(first_line(review_.title, 40));
        for (std::size_t i = 0; i < header_buttons_.size(); ++i)
            header_buttons_[i].text(review_.candidates[i].id + "  " + review_.candidates[i].label);
        if (summary_.getText() != review_.summary) summary_.value(review_.summary);
        select_candidate(selected_);
        show_note(note_);
    }

    void build() {
        auto root = screen_.root();
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const auto& c = review_.candidates[i];
            auto header = root.row().padding(4).gap(8);
            headers_.push_back(header);
            header_buttons_.push_back(header.button(c.id + "  " + c.label));
            header_counts_.push_back(header.label(""));
            images_.push_back(root.image());
            shown_revision_.push_back(0);
        }
        bar_ = screen_.column().padding(8).gap(6);
        auto controls = bar_.row().padding(0).gap(10);
        play_ = controls.button("Play").width(96);
        speed_box_ = controls.dropdown<f32>("Speed", {{1.F, "1x"}, {.5F, "1/2x"}, {1.F / 3, "1/3x"}, {.25F, "1/4x"}}).width(120);
        speed_box_.value(speed_);
        loop_box_ = controls.checkbox("Loop").value(loop_).width(100);
        clock_ = controls.label("").width(150);
        controls.label("Click a view to pin a note.  Space: play").width(420);
        strip_ = bar_.image().height(12);
        auto pixel = std::make_shared<gfx::ImageData>();
        pixel->extent = {1, 1};
        pixel->pixels.assign(4, std::byte{0});
        strip_.image(pixel);
        playhead_ = bar_.slider("Time", range_[0], range_[1]).step(0);

        side_ = screen_.column().padding(12).gap(8).scrollbar(ui::ScrollBar::automatic);
        title_ = side_.label(first_line(review_.title, 40));
        side_.label(review_.comparison() ? std::format("Comparing {} candidates", review_.candidates.size()) : "Single review");
        candidate_label_ = side_.label("");
        about_ = side_.column().padding(0).gap(0);
        side_.label("Your thoughts on this one");
        thoughts_ = side_.text_area().height(120).placeholder("What works, what doesn't...");
        if (review_.comparison()) pick_ = side_.button("Pick this one");
        else verdict_ = side_.dropdown<std::string>("Verdict", {{"", "No verdict yet"}, {"approved", "Approved"}, {"needs work", "Needs work"}});
        note_label_ = side_.label("");
        note_text_ = side_.text_area().height(100).placeholder("Your note (Enter for a new line)");
        auto actions = side_.row().padding(0).gap(8);
        go_to_ = actions.button("Go to").width(110);
        resolve_ = actions.button("Resolve").width(120);
        delete_ = actions.button("Delete").width(110);
        reply_ = side_.column().padding(0).gap(0);
        notes_title_ = side_.label("");
        notes_ = side_.column().padding(4).gap(4).height(200).scrollbar(ui::ScrollBar::automatic);
        side_.label("Overall");
        summary_ = side_.text_area().height(100).placeholder("Your overall thoughts").value(review_.summary);
        status_label_ = side_.label("");
        rebuild_note_rows();
    }
    void arrange(Vec2 size) {
        size_ = size;
        const auto l = layout(size, slots_.size());
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            place(headers_[i], l.headers[i]);
            place(images_[i], l.images[i]);
        }
        place(bar_, l.bar);
        place(side_, l.sidebar);
        rewrap();
    }
    // The sidebar's inner width, less padding, a scrollbar and the labels' own insets.
    static constexpr f32 text_width = 340;
    void rewrap() {
        for (auto& label : about_lines_) label.remove();
        about_lines_.clear();
        const auto& c = review_.candidates[selected_];
        for (auto& line : wrap(font_, c.about.empty() ? "(no description)" : c.about, text_width, 18))
            about_lines_.push_back(about_.label(line).height(26));
        for (auto& label : reply_lines_) label.remove();
        reply_lines_.clear();
        if (const auto* note = selected_note())
            if (!note->reply.empty())
                for (auto& line : wrap(font_, "Reply: " + note->reply, text_width, 18))
                    reply_lines_.push_back(reply_.label(line).height(26));
    }

    review::Note* selected_note() {
        const auto found = std::ranges::find(review_.notes, note_, &review::Note::id);
        return found == review_.notes.end() ? nullptr : &*found;
    }
    std::size_t candidate_index(std::string_view id) const {
        for (std::size_t i = 0; i < review_.candidates.size(); ++i)
            if (review_.candidates[i].id == id) return i;
        return 0;
    }
    void touched() {
        dirty_ = true;
        edited_ = Clock::now();
    }
    void select_candidate(std::size_t i) {
        selected_ = i;
        const auto& c = review_.candidates[i];
        candidate_label_.text(c.id + "  " + first_line(c.label, 36));
        if (thoughts_.getText() != c.thoughts) thoughts_.value(c.thoughts);
        for (std::size_t k = 0; k < header_buttons_.size(); ++k) header_buttons_[k].selected(k == i);
        if (pick_.valid()) pick_.selected(review_.pick == c.id).text(review_.pick == c.id ? "Picked" : "Pick this one");
        if (verdict_.valid()) verdict_.value(review_.verdict);
        rewrap();
    }
    // Selecting away from a note that was never written drops it.
    void show_note(u32 id) {
        if (note_ && note_ != id)
            if (auto* old = selected_note(); old && old->text.empty() && old->reply.empty()) {
                std::erase_if(review_.notes, [&](const review::Note& n) { return n.id == old->id; });
                touched();
            }
        note_ = id;
        const auto* note = selected_note();
        if (!note) {
            note_ = 0;
            note_label_.text("Click anything in a view to pin a note.");
            note_text_.value("").enabled(false);
            go_to_.enabled(false);
            resolve_.enabled(false);
            delete_.enabled(false);
        } else {
            const auto where = note->object ? note->object_name : std::string("the background");
            note_label_.text(std::format("#{}  {}  {}  {}", note->id, note->candidate, clock_text(note->time), first_line(where, 22)));
            note_text_.enabled(true);
            if (note_text_.getText() != note->text) note_text_.value(note->text);
            go_to_.enabled(true);
            resolve_.enabled(true).text(note->status == "resolved" ? "Reopen" : "Resolve");
            delete_.enabled(true);
        }
        rebuild_note_rows();
        rewrap();
    }
    void rebuild_note_rows() {
        for (auto& row : note_rows_) row.remove();
        note_rows_.clear();
        note_row_ids_.clear();
        auto sorted = review_.notes;
        std::ranges::sort(sorted, {}, [](const review::Note& n) { return std::pair{n.time, n.id}; });
        for (const auto& n : sorted) {
            auto row = notes_.button(row_text(n));
            row.selected(n.id == note_);
            note_rows_.push_back(row);
            note_row_ids_.push_back(n.id);
        }
        notes_title_.text(std::format("Notes ({})", review_.notes.size()));
        for (std::size_t i = 0; i < header_counts_.size(); ++i) {
            const auto count = std::ranges::count(review_.notes, review_.candidates[i].id, &review::Note::candidate);
            header_counts_[i].text(count ? std::format("{} note{}", count, count == 1 ? "" : "s") : "");
        }
    }

    void interact(const ui::UpdateResult& input, const input::Frame&) {
        if (play_.clicked()) playing_ = !playing_;
        if (const auto s = speed_box_.changedValue()) speed_ = *s;
        if (const auto l = loop_box_.changedValue()) loop_ = *l;
        if (const auto t = playhead_.changedValue()) at(*t);
        for (std::size_t i = 0; i < header_buttons_.size(); ++i)
            if (header_buttons_[i].clicked()) select_candidate(i);
        if (const auto text = thoughts_.changedText()) {
            review_.candidates[selected_].thoughts = std::string(*text);
            touched();
        }
        if (pick_.valid() && pick_.clicked()) {
            const auto& id = review_.candidates[selected_].id;
            review_.pick = review_.pick == id ? "" : id;
            select_candidate(selected_);
            touched();
        }
        if (verdict_.valid())
            if (const auto v = verdict_.changedValue()) {
                review_.verdict = *v;
                touched();
            }
        if (const auto text = note_text_.changedText())
            if (auto* note = selected_note()) {
                note->text = std::string(*text);
                touched();
                for (std::size_t i = 0; i < note_rows_.size(); ++i)
                    if (note_row_ids_[i] == note->id) note_rows_[i].text(row_text(*note));
            }
        if (auto* note = selected_note()) {
            if (go_to_.clicked()) {
                at(note->time);
                select_candidate(candidate_index(note->candidate));
            }
            if (resolve_.clicked()) {
                note->status = note->status == "resolved" ? "open" : "resolved";
                touched();
                show_note(note->id);
            }
            if (delete_.clicked()) {
                const auto id = note->id;
                std::erase_if(review_.notes, [&](const review::Note& n) { return n.id == id; });
                note_ = 0;
                touched();
                show_note(0);
            }
        }
        for (std::size_t i = 0; i < note_rows_.size(); ++i)
            if (note_rows_[i].clicked()) {
                const auto id = note_row_ids_[i];
                show_note(id);
                if (const auto* note = selected_note()) {
                    at(note->time);
                    select_candidate(candidate_index(note->candidate));
                }
                break;
            }
        if (const auto text = summary_.changedText()) {
            review_.summary = std::string(*text);
            touched();
        }
        for (const auto& event : input.unhandled()) {
            if (event.kind == input::EventKind::pointer_down && event.button == 0) click(event.position);
            if (event.kind == input::EventKind::key_down && !input.capturesKeyboard) {
                if (event.key == input::Key::space && !event.repeat) playing_ = !playing_;
                if (event.key == input::Key::left) at(time_ - 1.F / 30);
                if (event.key == input::Key::right) at(time_ + 1.F / 30);
                if (event.key == input::Key::home) at(range_[0]);
                if (event.key == input::Key::escape) show_note(0);
            }
        }
    }
    // Where a note's marker sits in its view now, in window coordinates.
    std::optional<Vec2> marker_at(const review::Note& n) const {
        const auto i = candidate_index(n.candidate);
        const auto b = images_[i].bounds();
        if (!slots_[i].view) return {};
        auto spot = n.point ? slots_[i].view->project(*n.point) : std::optional{n.view};
        if (!spot) return {};
        const Vec2 p{b.x + spot->x * b.width, b.y + spot->y * b.height};
        return b.contains(p) ? std::optional{p} : std::nullopt;
    }
    bool marker_visible(const review::Note& n) const { return n.id == note_ || std::abs(n.time - time_) <= 1.F; }
    void click(Vec2 position, bool pin_only = false) {
        for (std::size_t i = 0; i < images_.size(); ++i) {
            const auto b = images_[i].bounds();
            if (!b.contains(position)) continue;
            playing_ = false;
            select_candidate(i);
            // A click on a visible marker selects its note rather than pinning a new one.
            if (!pin_only)
            for (const auto& n : review_.notes)
                if (n.candidate == review_.candidates[i].id && marker_visible(n))
                    if (const auto p = marker_at(n); p && std::hypot(p->x - position.x, p->y - position.y) < 12) {
                        show_note(n.id);
                        return;
                    }
            const Vec2 normalized{(position.x - b.x) / b.width, (position.y - b.y) / b.height};
            review::Note note;
            note.id = review_.next_note_id();
            note.candidate = review_.candidates[i].id;
            note.time = time_;
            // Pick and timestamp the image the reviewer actually clicked,
            // rather than a playhead whose frame may still be rendering.
            if (slots_[i].view)
                if (const auto shown = slots_[i].view->shown_time())
                    note.time = std::clamp(*shown - review_.candidates[i].offset, range_[0], range_[1]);
            at(note.time);
            note.view = normalized;
            note.created = review::timestamp();
            if (const auto hit = slots_[i].view ? slots_[i].view->pick(normalized) : std::nullopt) {
                note.object = hit->object;
                note.object_name = slots_[i].view->name_of(hit->object);
                note.point = hit->point;
            }
            review_.notes.push_back(std::move(note));
            show_note(review_.notes.back().id);
            note_text_.focus();
            return;
        }
    }
    void sync_controls() {
        play_.text(playing_ ? "Pause" : "Play");
        clock_.text(clock_text(time_));
        if (!playhead_.isPressed()) playhead_.value(time_);
    }
    void markers(ui::DrawList& list) const {
        const auto dark = Vec4{.02F, .02F, .03F, 1};
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].view) continue;
            const auto b = images_[i].bounds();
            list.commands.emplace_back(ui::BoxDraw{b, b, {.06F, .06F, .08F, 1}, {.55F, .25F, .22F, 1}, 4, 1});
            f32 y = b.y + 16;
            for (const auto& line : wrap(font_, "This scene did not load. " + slots_[i].error, b.width - 32, 16)) {
                list.commands.emplace_back(ui::TextDraw{line, {b.x + 16, y}, b, font_, 16, {.95F, .78F, .72F, 1}});
                y += 22;
            }
        }
        for (const auto& n : review_.notes) {
            if (!marker_visible(n)) continue;
            const auto p = marker_at(n);
            if (!p) continue;
            const auto clip = images_[candidate_index(n.candidate)].bounds();
            const f32 fade = n.id == note_ ? 1.F : std::clamp(1.2F - std::abs(n.time - time_), .35F, 1.F);
            auto color = n.status == "resolved" ? Vec4{.35F, .85F, .45F, 1} : Vec4{1, .72F, .2F, 1};
            color.w = fade;
            const f32 r = n.id == note_ ? 11.F : 9.F;
            list.commands.emplace_back(ui::BoxDraw{{p->x - r, p->y - r, 2 * r, 2 * r}, clip, color,
                                                   n.id == note_ ? Vec4{1, 1, 1, 1} : dark, r, n.id == note_ ? 2.5F : 1.5F});
            const auto label = std::to_string(n.id);
            list.commands.emplace_back(ui::TextDraw{label, {p->x - 4.F * static_cast<f32>(label.size()), p->y - 8}, clip, font_, 13, dark});
        }
        // The shared clock: every note as a tick in its candidate's colour, and the playhead.
        const auto s = strip_.bounds();
        const auto span = range_[1] - range_[0];
        list.commands.emplace_back(ui::BoxDraw{s, s, {.08F, .09F, .11F, 1}, {}, 3, 0});
        for (const auto& n : review_.notes) {
            const f32 x = s.x + (n.time - range_[0]) / span * s.width;
            list.commands.emplace_back(ui::BoxDraw{{x - 3, s.y, 6, s.height}, s, tint(candidate_index(n.candidate)),
                                                   n.id == note_ ? Vec4{1, 1, 1, 1} : Vec4{}, 2, n.id == note_ ? 1.5F : 0});
        }
        const f32 x = s.x + (time_ - range_[0]) / span * s.width;
        list.commands.emplace_back(ui::BoxDraw{{x - 1, s.y - 2, 2, s.height + 4}, s, {1, 1, 1, .9F}, {}, 0, 0});
        // A coloured underline ties each header to its ticks.
        for (std::size_t i = 0; i < headers_.size(); ++i) {
            const auto h = headers_[i].bounds();
            list.commands.emplace_back(ui::BoxDraw{{h.x, h.y + h.height - 3, h.width, 3}, h, tint(i), {}, 0, 0});
        }
    }

    std::filesystem::path file_;
    review::Review review_, disk_;
    std::optional<review::FileStamp> disk_stamp_, unreadable_;
    std::optional<Edits> reopen_;
    std::vector<Slot> slots_;
    text::Font font_;
    ui::Screen screen_;
    std::array<f32, 2> range_{};
    f32 time_{}, speed_{1};
    bool playing_{}, loop_{true}, dirty_{};
    Clock::time_point edited_{}, polled_{};
    std::size_t selected_{};
    u32 note_{};
    Vec2 size_{};
    std::string status_;
    std::vector<ui::Container> headers_;
    std::vector<ui::Button> header_buttons_;
    std::vector<ui::Label> header_counts_;
    std::vector<ui::ImageView> images_;
    std::vector<u64> shown_revision_;
    ui::Container bar_, side_, about_, reply_, notes_;
    ui::Button play_, pick_, go_to_, resolve_, delete_;
    ui::Dropdown<f32> speed_box_{{}, {}};
    ui::Dropdown<std::string> verdict_{{}, {}};
    ui::Checkbox loop_box_;
    ui::Label title_, clock_, candidate_label_, note_label_, notes_title_, status_label_;
    ui::ImageView strip_;
    ui::Slider playhead_;
    ui::TextField thoughts_, note_text_, summary_;
    std::vector<ui::Label> about_lines_, reply_lines_;
    std::vector<ui::Button> note_rows_;
    std::vector<u32> note_row_ids_;
};

struct WindowClipboard final : input::Clipboard {
    explicit WindowClipboard(glfw_opengl::Window& w) : window(w) {}
    std::string read() override { return window.clipboard_text(); }
    void write(std::string_view text) override { window.set_clipboard_text(text); }
    glfw_opengl::Window& window;
};
} // namespace

int main(int argc, char** argv) {
    auto options = parse(argc, argv);
    if (!options) return fail(options.error());
    if (options->help) {
        std::cout << help;
        return 0;
    }
    Carry carry;
    carry.time = options->at;
    if (options->create) {
        if (std::filesystem::exists(options->file) && !options->replace)
            return fail(options->file.string() + " exists; pass --replace to overwrite it");
        review::Review r;
        r.title = options->title.empty() ? options->candidates.front().label : options->title;
        r.created = review::timestamp();
        r.range = options->range;
        r.candidates = std::move(options->candidates);
        auto saved = review::save_review(options->file, r);
        if (!saved) return fail(describe(saved.error()));
        std::cout << "Wrote " << options->file.string() << '\n';
        if (!options->open) return 0;
        carry.edits = {r, r, *saved};
    } else {
        // Stamped before reading: a write in between shows up as a change later.
        const auto stamp = review::stamp_of(options->file);
        auto read = review::read_review(options->file);
        if (!read) return fail(describe(read.error()));
        carry.edits = {*read, *read, stamp};
    }
    auto font = text::Font::load(VNG_EXAMPLE_FONT_PATH);
    if (!font) return fail(font.error().message);

    const bool hidden = options->hidden || options->screenshot.has_value();
    auto session = example::GlfwOpenGLSession::create(
        {.width = 1600, .height = 940, .title = "Review: " + first_line(carry.edits.review.title, 60), .visible = !hidden},
        {.debug = false, .samples = 0, .default_framebuffer_encoding = render::ColorEncoding::linear});
    if (!session) {
        (void)example::fail(session.error());
        return hidden ? 77 : 1; // scripted runs: 77 lets a test harness skip without OpenGL
    }
    auto& window = session->window();
    auto& device = session->device();
    auto renderer = render::make_ui_renderer(device);
    if (!renderer) return fail(renderer.error().message);
    auto display = example::DisplaySurface::create(device, window.framebuffer_extent());
    if (!display) return fail(display.error().message);
    example::WindowLoop loop{window, options->frames};
    WindowClipboard clipboard{window};

    const auto draw = [&](const ui::DrawList& list) -> std::expected<void, std::string> {
        const auto extent = list.framebuffer;
        if (auto resized = display->resize(device, extent); !resized) return std::unexpected(resized.error().message);
        auto frame = render::begin_frame(device, display->target(), {.extent = extent, .color_encoding = render::ColorEncoding::srgb,
            .clear_color = std::array<float, 4>{.012F, .014F, .02F, 1}, .clear_depth = {}});
        if (!frame) return std::unexpected(frame.error().message);
        if (auto drawn = renderer->render(*frame, list); !drawn) return std::unexpected(drawn.error().message);
        if (auto ended = frame->end(); !ended) return std::unexpected(ended.error().message);
        if (auto copied = display->copy_to_window(device); !copied) return std::unexpected(copied.error().message);
        return {};
    };
    // A line of text in an otherwise empty window while scenes load.
    const auto loading = [&](std::size_t count) {
        std::cout << "Loading " << count << " scene" << (count == 1 ? "" : "s") << "...\n";
        if (hidden) return;
        const auto input = window.take_input();
        if (!input.framebuffer.width || !input.framebuffer.height) return;
        ui::DrawList list{input.logical_size, input.framebuffer, {}};
        list.commands.emplace_back(ui::TextDraw{std::format("Loading {} scene{}...", count, count == 1 ? "" : "s"), {24, 20},
            {0, 0, input.logical_size.x, input.logical_size.y}, *font, 22, {.8F, .82F, .86F, 1}});
        if (draw(list)) (void)window.present();
    };
    const auto open = [&](Carry next, std::vector<Slot> reusable) {
        auto slots = load_slots(device, next.edits.review, std::move(reusable), loading);
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (!slots[i].view) std::cerr << "Candidate " << next.edits.review.candidates[i].id << ": " << slots[i].error << '\n';
        return std::make_unique<App>(options->file, std::move(next), std::move(slots), *font);
    };

    auto app = open(std::move(carry), {});
    auto previous = Clock::now();
    const auto started = previous;
    u64 frames{};
    bool pinned = options->pins.empty(), started_playing = !options->play;
    for (unsigned waited = 0; auto extent = loop.next_extent(); ++frames) {
        // Scripted pins land once every view shows the start time.
        if (!pinned && app->frame_ready()) {
            for (const auto& p : options->pins)
                if (!app->pin(p.candidate, p.view, p.text)) return fail("--note names no candidate or has a point outside 0..1");
            pinned = true;
        }
        if (!started_playing && app->frame_ready()) {
            app->play();
            started_playing = true;
        }
        const auto now = Clock::now();
        const auto dt = std::min(std::chrono::duration<f32>(now - previous).count(), .1F);
        previous = now;
        auto list = app->step(window.take_input(), &clipboard, dt, device);
        if (!list) return fail(list.error());
        if (app->wants_reopen()) {
            // New candidates or rewritten scenes: a new app, keeping the views that still fit.
            auto next = app->carry();
            auto slots = app->take_slots();
            app.reset();
            app = open(std::move(next), std::move(slots));
            previous = Clock::now();
            continue;
        }
        if (!*list || (*list)->framebuffer.width != extent->width || (*list)->framebuffer.height != extent->height) continue;
        if (auto drawn = draw(**list); !drawn) return fail(drawn.error());
        if (options->screenshot && pinned && app->frame_ready() && ++waited > 2) {
            auto pixels = example::capture_screenshot(device, *extent);
            if (!pixels) return fail(pixels.error().message);
            if (auto written = example::write_rgba8_png(*options->screenshot, *extent,
                    {reinterpret_cast<const u8*>(pixels->pixels.data()), pixels->pixels.size()}); !written)
                return fail(written.error().message);
            std::cout << "Saved " << options->screenshot->string() << '\n';
            const bool saved = app->autosave(true);
            if (!app->status().empty()) std::cout << app->status() << '\n';
            return saved ? 0 : 1;
        }
        if (auto presented = loop.present(); !presented) return fail(presented.error().message);
    }
    const bool saved = app->autosave(true);
    if (!app->status().empty()) std::cout << app->status() << '\n';
    if (options->frames) {
        const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
        std::cout << std::format("{} frames in {:.1f} s ({:.1f} fps); views showed {}\n", frames, seconds,
            static_cast<double>(frames) / seconds, app->shown_frames());
    }
    return saved ? 0 : 1;
}
