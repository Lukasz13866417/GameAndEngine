#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vng::content {

enum class ErrorCode {
    io_error,
    input_too_large,
    invalid_token,
    unexpected_token,
    unsupported_version,
    invalid_escape,
    invalid_utf8,
    invalid_number,
    number_out_of_range,
    limit_exceeded,
    duplicate_metadata,
    duplicate_field,
    count_mismatch,
    invalid_field_type,
    invalid_document,
    index_out_of_range,
};

struct SourceLocation final {
    std::size_t byte_offset{};
    std::size_t line{1};
    std::size_t column{1};

    friend constexpr bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

struct Diagnostic final {
    ErrorCode code{ErrorCode::invalid_document};
    std::string message;
    std::optional<SourceLocation> location;
    std::filesystem::path path;
    std::vector<std::string> notes;

    [[nodiscard]] Diagnostic& note(std::string text)
    {
        notes.push_back(std::move(text));
        return *this;
    }
};

template<class T>
using Result = std::expected<T, Diagnostic>;

} // namespace vng::content
