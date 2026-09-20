#pragma once
#include <string>

namespace vng::window {
enum class ErrorCode {
    initialization_failed,
    window_creation_failed,
    invalid_window,
    context_not_current,
    wrong_thread,
    operation_failed,
};
struct Diagnostic final {
    ErrorCode code{ErrorCode::operation_failed};
    std::string message{};
    int native_code{};
};
} // namespace vng::window
