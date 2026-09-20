#pragma once
#include <vng/editor/preview.hpp>
#include <pthread.h>

// Internal process-shared wire layout. No pointers or C++ containers.
namespace vng::editor::preview::detail {
struct SharedHeader {
    std::uint64_t signature;
    std::uint32_t max_width, max_height;
    std::uint64_t generation;
    pthread_mutex_t mutex;
    std::uint64_t sequence;
    std::uint32_t valid;
    FrameInfo info;
};
} // namespace vng::editor::preview::detail
