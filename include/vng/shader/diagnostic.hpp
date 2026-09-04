#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace vng::shader {

enum class DiagnosticCode : std::uint8_t {
    invalid_expression,
    mixed_builders,
    invalid_stage_result,
    incomplete_record,
    duplicate_semantic,
    interface_mismatch,
    invalid_builtin,
    invalid_ir,
    unsupported_operation,
};

struct SourceOrigin {
    std::string file;
    std::string function;
    std::uint_least32_t line{};
    std::uint_least32_t column{};
};

struct Diagnostic {
    DiagnosticCode code{DiagnosticCode::invalid_ir};
    std::string message;
    std::vector<std::string> notes;
    SourceOrigin origin;
    std::string generated_source;

    [[nodiscard]] Diagnostic& note(std::string text)
    {
        notes.push_back(std::move(text));
        return *this;
    }
};

template<class T>
using Result = std::expected<T, Diagnostic>;

} // namespace vng::shader
