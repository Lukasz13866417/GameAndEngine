#pragma once
#include <cstddef>

namespace vng::editor {
// A document can retain both published geometry and a procedural working copy.
// All limits remain bounded; the envelope reserves room for protocol headers.
inline constexpr std::size_t max_message_bytes=32U*1024U*1024U;
inline constexpr std::size_t max_document_bytes=max_message_bytes-256U;
}
