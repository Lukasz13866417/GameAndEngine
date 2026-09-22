#pragma once

#include <filesystem>
#include <utility>

#include <vng/text/font.hpp>

namespace vng::providers {

class FontFile final {
public:
    explicit FontFile(std::filesystem::path path) : path_(std::move(path)) {}

    [[nodiscard]] auto provide() const { return text::Font::load(path_); }

private:
    std::filesystem::path path_;
};

[[nodiscard]] inline FontFile font_file(std::filesystem::path path)
{
    return FontFile{std::move(path)};
}

} // namespace vng::providers
