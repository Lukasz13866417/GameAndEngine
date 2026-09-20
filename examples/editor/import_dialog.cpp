#include "import_dialog.hpp"
#include "import_format.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace editor_example {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t scan_limit = 2048;

bool representable(std::string_view text) {
    return text.size() <= 4096 && std::ranges::none_of(text, [](unsigned char c) {
        return c < 32 || c == 127;
    });
}
// Long names stay complete in the path field, where normal text scrolling works.
// Never cut through a UTF-8 continuation byte in the compact browser caption.
std::string caption(std::string_view text) {
    std::size_t characters{}, end{};
    for (; end < text.size(); ++end)
        if ((static_cast<unsigned char>(text[end]) & 0xC0U) != 0x80U && ++characters > 32)
            break;
    if (end == text.size())
        return std::string(text);
    return std::string(text.substr(0, end)) + "...";
}
} // namespace

ImportDialog::ImportDialog(vng::ui::Container panel) : panel_(std::move(panel)) {
    panel_.padding(14).gap(8);
    panel_.label("Import / meshes and effect presets").height(28);
    panel_.label("Choose a file below, or paste its full path. Directories open on click.")
        .height(24);
    filename_ = panel_.text_input().height(42).placeholder("Existing .vmesh / .veffect file or directory");
    auto navigation = panel_.row().height(36).padding(0).gap(8);
    up_ = navigation.button("Up").width(82);
    refresh_ = navigation.button("Refresh").width(108);
    navigation.label("Folders first / .vmesh meshes / .veffect presets").width(450);
    list_ = panel_.column().height(280).padding(2).gap(0);
    status_ = panel_.label("Choose a mesh (.vmesh) or effect preset (.veffect).").height(36);
    auto buttons = panel_.row().height(38).padding(0).gap(10);
    import_ = buttons.button("Import").width(160);
    cancel_ = buttons.button("Cancel").width(160);
    panel_.visible(false);
}

void ImportDialog::clear_entries() {
    entries_.clear();
    if (body_.valid())
        body_.remove();
    body_ = list_.column().padding(0).gap(4);
    list_.scroll(0);
}

void ImportDialog::open(const fs::path& start) {
    visible_ = true;
    panel_.visible(true);
    selected_.reset();
    clear_entries();
    directory_.clear();
    up_.enabled(false);
    std::error_code code;
    auto path = start.empty() ? fs::current_path(code) : fs::absolute(start, code);
    if (code || !representable(path.string())) {
        clear_entries();
        directory_.clear();
        filename_.value("");
        last_filename_.clear();
        error("Cannot open this path. Paste a directory, .vmesh or .veffect path.");
        filename_.focus();
        return;
    }
    path = path.lexically_normal();
    const bool is_directory = fs::is_directory(path, code);
    try {
        if (is_directory) {
            browse(path);
        } else {
            browse(path.parent_path());
            choose(path);
        }
    } catch (const std::invalid_argument&) {
        clear_entries();
        filename_.value("");
        last_filename_.clear();
        error("This path cannot be displayed as UTF-8 text. Choose another file.");
    }
    filename_.focus();
}

void ImportDialog::close() {
    visible_ = false;
    panel_.visible(false);
}

void ImportDialog::error(std::string_view message) {
    status_.text(message);
}

void ImportDialog::update_selection() {
    for (auto& entry : entries_)
        entry.button.text((selected_ == entry.path ? "> " : "") + entry.label);
}

void ImportDialog::choose(const fs::path& path) {
    last_filename_ = path.string();
    filename_.value(last_filename_);
    selected_ = path;
    update_selection();
    filename_.focus();
}

bool ImportDialog::browse(const fs::path& path) {
    std::error_code code;
    if (!fs::is_directory(path, code) || code) {
        error("Cannot open that directory. Check the path and access permissions.");
        return false;
    }
    directory_ = path.lexically_normal();
    if (directory_.filename().empty() && directory_ != directory_.root_path())
        directory_ = directory_.parent_path(); // "folder/" must go Up on the first click.
    selected_.reset();
    clear_entries();
    last_filename_ = directory_.string();
    filename_.value(last_filename_);
    up_.enabled(directory_.has_parent_path() && directory_.parent_path() != directory_);
    fs::directory_iterator next(directory_, code), end;
    if (code) {
        error("Cannot list this directory. Check its access permissions.");
        return false;
    }
    std::vector<Entry> found;
    std::size_t visited{};
    bool skipped{};
    for (; next != end && visited < scan_limit; next.increment(code)) {
        if (code)
            break;
        ++visited;
        const auto path = next->path();
        std::error_code type_error;
        const bool directory = next->is_directory(type_error);
        if (type_error) {
            skipped = true;
            continue;
        }
        if (!directory && (!import_kind(path) || !next->is_regular_file(type_error)))
            continue;
        const auto name = path.filename().string();
        if (!representable(name)) {
            skipped = true;
            continue;
        }
        found.push_back({path, directory, {}, caption((directory ? "[Folder] " : "") + name)});
    }
    const bool truncated = next != end;
    std::ranges::sort(found, [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory)
            return a.directory;
        return a.path.filename().native() < b.path.filename().native();
    });
    for (auto& entry : found) {
        try {
            entry.button = body_.button(entry.label).height(32);
            entries_.push_back(std::move(entry));
        } catch (const std::invalid_argument&) {
            skipped = true; // A Linux filename may contain bytes that are not UTF-8.
        }
    }
    if (code)
        error("Directory listing stopped early: some entries could not be read.");
    else if (truncated)
        error("Large directory: showing the first 2048 entries. Paste a full path if missing.");
    else if (skipped)
        error("Some unreadable names were skipped. Choose a displayed file or paste a path.");
    else if (entries_.empty())
        error("No .vmesh / .veffect files or directories here. Use Up or paste a path.");
    else
        error("Select a mesh or effect preset, then Import or Enter. A folder click opens it.");
    filename_.focus();
    return !code;
}

std::optional<fs::path> ImportDialog::submit() {
    const auto text = filename_.getText();
    if (text.empty() || !representable(text)) {
        error("Enter an existing .vmesh or .veffect file path.");
        return {};
    }
    auto path = fs::path(text);
    std::error_code code;
    if (!path.is_absolute())
        path = directory_.empty() ? fs::absolute(path, code) : directory_ / path;
    if (code) {
        error("Cannot resolve this path.");
        return {};
    }
    path = path.lexically_normal();
    const auto status = fs::status(path, code);
    if (code || !fs::exists(status)) {
        error("This path does not exist or cannot be accessed.");
        return {};
    }
    if (fs::is_directory(status)) {
        browse(path);
        return {};
    }
    if (!fs::is_regular_file(status) || !import_kind(path)) {
        error("Choose an existing regular .vmesh mesh or .veffect preset.");
        return {};
    }
    choose(path);
    return path;
}

std::optional<fs::path> ImportDialog::poll(std::span<const vng::input::Event> raw) {
    using namespace vng;
    if (!visible_)
        return {};
    if (cancel_.clicked() || std::ranges::any_of(raw, [](const auto& event) {
            return event.kind == input::EventKind::key_down && event.key == input::Key::escape;
        })) {
        close();
        return {};
    }
    if (filename_.changedText() || filename_.getText() != last_filename_) {
        last_filename_ = filename_.getText();
        selected_.reset();
        update_selection();
        error("Press Enter to open this directory or import the selected asset.");
    }
    if (up_.clicked()) {
        browse(directory_.parent_path());
        return {};
    }
    if (refresh_.clicked()) {
        const auto selected = selected_;
        browse(directory_);
        if (selected)
            choose(*selected);
        return {};
    }
    for (const auto& entry : entries_)
        if (entry.button.clicked()) {
            const auto path = entry.path; // Navigation replaces the list.
            if (entry.directory)
                browse(path);
            else {
                choose(path);
                error("Asset selected. Click Import or press Enter.");
            }
            return {};
        }
    const bool enter = filename_.submittedText() &&
                       std::ranges::any_of(raw, [](const auto& event) {
                           return event.kind == input::EventKind::key_down &&
                                  event.key == input::Key::enter && !event.repeat;
                       });
    if (!import_.clicked() && !enter)
        return {};
    return submit();
}
} // namespace editor_example
