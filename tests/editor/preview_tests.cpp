#include <vng/editor/preview.hpp>
#include "../../src/editor/preview_surface.hpp"
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
namespace preview = vng::editor::preview;
using Clock = std::chrono::steady_clock;
std::filesystem::path executable;

int worker_main(int control, int memory, std::uint64_t generation) {
    auto endpoint = preview::WorkerEndpoint::attach(control, memory, generation);
    if (!endpoint) return 40;
    std::uint64_t frame{};
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (!endpoint->closed() && Clock::now() < deadline) {
        auto messages = endpoint->receive();
        if (!messages) return 41;
        for (const auto& message : *messages) {
            if (message == "shutdown") return 0;
            if (message == "crash") _exit(9);
            if (message == "busy-publication") {
                void* shared=::mmap(nullptr,sizeof(preview::detail::SharedHeader),PROT_READ|PROT_WRITE,MAP_SHARED,memory,0);
                if(shared==MAP_FAILED) return 49;
                auto& header=*static_cast<preview::detail::SharedHeader*>(shared);
                const std::array<std::byte,16> rgba{};
                const preview::FrameInfo info{2,2,generation,77,frame+1,0};
                if(pthread_mutex_lock(&header.mutex)) return 50;
                auto busy=endpoint->try_publish(info,rgba);
                const bool unchanged=header.info.frame_id==frame;
                pthread_mutex_unlock(&header.mutex);
                ::munmap(shared,sizeof(preview::detail::SharedHeader));
                if(!busy || *busy || !unchanged) return 51;
                for(;;) {
                    auto retried=endpoint->try_publish(info,rgba);
                    if(!retried) return 52;
                    if(*retried) break;
                    if(Clock::now()>=deadline) return 53;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                ++frame;
                if(!endpoint->send("busy-retry-ok")) return 54;
                continue;
            }
            if (message == "capacity") {
                const auto size = endpoint->capacity();
                if (!endpoint->send(std::to_string(size.width) + "x" +
                                    std::to_string(size.height))) return 47;
                continue;
            }
            if (message == "move-endpoint") {
                const auto capacity = endpoint->capacity();
                auto moved = std::move(*endpoint);
                const auto empty = endpoint->capacity();
                const bool moved_from_safe = empty.width == 0 && empty.height == 0 && endpoint->closed();
                const auto moved_capacity = moved.capacity();
                *endpoint = std::move(moved);
                const auto reassigned_capacity = endpoint->capacity();
                const bool restored = moved_capacity.width == capacity.width &&
                                      moved_capacity.height == capacity.height &&
                                      reassigned_capacity.width == capacity.width &&
                                      reassigned_capacity.height == capacity.height;
                const auto moved_again = moved.capacity();
                const bool reassigned_from_safe = moved_again.width == 0 && moved_again.height == 0 && moved.closed();
                if (!endpoint->send(moved_from_safe && restored && reassigned_from_safe ? "move-ok" : "BUG")) return 48;
                continue;
            }
            if (message == "malformed-packet") {
                const std::array<char, 9> malformed{};
                (void)::send(control, malformed.data(), malformed.size(), MSG_NOSIGNAL);
                continue;
            }
            if (message == "begin" || message == "frame") {
                const std::array<std::byte, 16> rgba{
                    std::byte{7}, std::byte{17}, std::byte{27}, std::byte{255},
                    std::byte{37}, std::byte{47}, std::byte{57}, std::byte{255},
                    std::byte{67}, std::byte{77}, std::byte{87}, std::byte{255},
                    std::byte{97}, std::byte{107}, std::byte{117}, std::byte{255}};
                if (message == "begin") {
                    if (!endpoint->send("schema-before-ready")) return 42;
                    if (!endpoint->ready()) return 43;
                }
                const preview::FrameInfo info{2, 2, generation, 77, ++frame, 1.25};
                for (;;) {
                    const auto published = endpoint->try_publish(info, rgba);
                    if (!published) return 44;
                    if (*published) break;
                    if (Clock::now() >= deadline) return 44;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } else if (message == "bad-frame") {
                const std::array<std::byte, 16> rgba{};
                const bool generation_rejected = !endpoint->try_publish({2, 2, generation + 1, 77, frame + 1, 0}, rgba);
                const bool bytes_rejected = !endpoint->try_publish({2, 2, generation, 77, frame + 1, 0}, {});
                const bool old_id_rejected = !endpoint->try_publish({2, 2, generation, 77, frame, 0}, rgba);
                if (!endpoint->send(generation_rejected && bytes_rejected && old_id_rejected ? "rejected" : "BUG")) return 45;
            } else {
                if (!endpoint->send(message)) return 46;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

preview::PreviewConfig config(std::vector<std::string> build = {"/bin/true"}) {
    return {.build_command = std::move(build), .build_directory = {}, .worker_executable = executable,
            .max_width = 8, .max_height = 8,
            .build_timeout = std::chrono::seconds(2), .startup_timeout = std::chrono::seconds(2)};
}
template<class Predicate>
std::vector<preview::PreviewEvent> until(preview::PreviewSession& session, Predicate done,
                                       bool initialize_candidates = true) {
    std::vector<preview::PreviewEvent> events;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        auto next = session.poll();
        for (auto& event : next) {
            if (event.kind == preview::EventKind::candidate_started && initialize_candidates)
                REQUIRE(session.send("begin", event.generation));
            events.push_back(std::move(event));
        }
        if (done(events)) return events;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    FAIL("Preview condition timed out: " << session.status() << "\n" << session.logs());
    return events;
}
bool contains(const std::vector<preview::PreviewEvent>& events, preview::EventKind kind) {
    for (const auto& e : events) if (e.kind == kind) return true;
    return false;
}
} // namespace

TEST_CASE("Preview workers exchange bounded binary-safe packets and completed RGBA frames", "[editor][preview]") {
    auto session = preview::PreviewSession::create(config());
    REQUIRE(session);
    const auto events = until(*session, [&](const auto&) { return session->active_generation() != 0; });
    CHECK(contains(events, preview::EventKind::activated));
    bool schema_received{};
    for (const auto& event : events) schema_received |= event.message == "schema-before-ready";
    CHECK(schema_received);
    REQUIRE(session->latest_frame());
    const auto& frame = *session->latest_frame();
    CHECK(frame.info.width == 2);
    CHECK(frame.info.height == 2);
    CHECK(frame.info.generation == session->active_generation());
    CHECK(frame.info.scene_revision == 77);
    CHECK(frame.info.frame_id == 1);
    CHECK(frame.info.time == 1.25);
    CHECK(frame.rgba.size() == 16);
    CHECK(frame.rgba.front() == std::byte{7});
    CHECK(frame.rgba.back() == std::byte{255});
    const std::string binary("echo\0bytes", 10);
    REQUIRE(session->send(binary));
    const auto echoed = until(*session, [&](const auto& incoming) {
        for (const auto& e : incoming) if (e.message == binary) return true;
        return false;
    });
    CHECK_FALSE(echoed.empty());
    std::string large(preview::max_message_bytes, 'q');
    large[111] = '\0';
    large.back() = 'z';
    REQUIRE(session->send(large));
    until(*session, [&](const auto& incoming) {
        for (const auto& e : incoming) if (e.message == large) return true;
        return false;
    });
    CHECK_FALSE(session->send(""));
    CHECK_FALSE(session->send(std::string(preview::max_message_bytes + 1, 'x')));
    CHECK_FALSE(session->send("stale event", session->active_generation() + 12));
    REQUIRE(session->send("bad-frame"));
    until(*session, [](const auto& incoming) {
        for (const auto& e : incoming) if (e.message == "rejected") return true;
        return false;
    });
    CHECK(session->latest_frame()->info.frame_id == 1);
}

TEST_CASE("Publication never waits on a busy reader and the final frame can be retried", "[editor][preview][async]") {
    auto session=preview::PreviewSession::create(config()); REQUIRE(session);
    until(*session,[&](const auto&){return session->active_generation()!=0;});
    REQUIRE(session->send("busy-publication"));
    until(*session,[](const auto& incoming) {
        for(const auto& event:incoming) if(event.message=="busy-retry-ok") return true;
        return false;
    });
    until(*session,[&](const auto&){return session->latest_frame() && session->latest_frame()->info.frame_id==2;});
    CHECK(session->latest_frame()->rgba==std::vector<std::byte>(16));
}

TEST_CASE("Preview requests flush in the current tick while preserving coalescing and ordering", "[editor][preview]") {
    auto session = preview::PreviewSession::create(config());
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    REQUIRE(session->send("ordered-before"));
    REQUIRE(session->send_latest("superseded-view"));
    REQUIRE(session->send_latest("latest-view"));
    REQUIRE(session->flush_requests()); // No poll between queuing and delivery.
    REQUIRE(session->send("barrier-after"));
    const auto events = until(*session, [](const auto& incoming) {
        for (const auto& event : incoming) if (event.message == "barrier-after") return true;
        return false;
    });
    std::vector<std::string> messages;
    for (const auto& event : events)
        if (event.kind == preview::EventKind::message) messages.push_back(event.message);
    CHECK(messages == std::vector<std::string>{"ordered-before", "latest-view", "barrier-after"});
    REQUIRE(session->flush_requests()); // Empty flush is harmless.
    auto moved = std::move(*session);
    CHECK_FALSE(session->flush_requests());
    REQUIRE(moved.flush_requests());
}

TEST_CASE("Preview reload keeps active generation until a replacement frame is ready", "[editor][preview]") {
    auto session = preview::PreviewSession::create(config());
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    const auto old = session->active_generation();
    REQUIRE(session->request_reload());
    CHECK_FALSE(session->request_reload());
    const auto events = until(*session, [](const auto& e) { return contains(e, preview::EventKind::candidate_started); }, false);
    CHECK(session->active_generation() == old);
    CHECK(session->latest_frame()->info.generation == old);
    std::uint64_t candidate{};
    for (const auto& e : events) if (e.kind == preview::EventKind::candidate_started) candidate = e.generation;
    REQUIRE(candidate > old);
    REQUIRE(session->send("begin", candidate));
    until(*session, [&](const auto&) { return session->active_generation() == candidate; });
    CHECK(session->latest_frame()->info.generation == candidate);
    CHECK_FALSE(session->send("stale", old));
    CHECK_FALSE(session->busy());
}

TEST_CASE("Worker frame capacity matches its configured surface and survives moves", "[editor][preview]") {
    auto options = config();
    options.max_width = 13;
    options.max_height = 7;
    auto session = preview::PreviewSession::create(std::move(options));
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    REQUIRE(session->send("capacity"));
    until(*session, [](const auto& incoming) {
        for (const auto& event : incoming) if (event.message == "13x7") return true;
        return false;
    });
    REQUIRE(session->send("move-endpoint"));
    until(*session, [](const auto& incoming) {
        for (const auto& event : incoming) if (event.message == "move-ok") return true;
        return false;
    });
    REQUIRE(session->send("frame"));
    until(*session, [&](const auto&) {
        return session->latest_frame() && session->latest_frame()->info.frame_id == 2;
    });
}

TEST_CASE("Taking a completed preview frame transfers its pixels exactly once", "[editor][preview]") {
    auto session = preview::PreviewSession::create(config());
    REQUIRE(session);
    CHECK_FALSE(session->take_latest_frame());
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    REQUIRE(session->latest_frame());
    const auto* pixels = session->latest_frame()->rgba.data();
    auto displayed = session->take_latest_frame();
    REQUIRE(displayed);
    CHECK(displayed->rgba.data() == pixels);
    CHECK(displayed->rgba.size() == 16);
    CHECK(displayed->info.frame_id == 1);
    CHECK(displayed->info.scene_revision == 77);
    CHECK(displayed->info.time == 1.25);
    CHECK_FALSE(session->latest_frame());
    CHECK_FALSE(session->take_latest_frame());

    // An ordinary message round trip must not re-import an already consumed surface.
    REQUIRE(session->send("after-take"));
    until(*session, [](const auto& incoming) {
        for (const auto& event : incoming) if (event.message == "after-take") return true;
        return false;
    });
    CHECK_FALSE(session->latest_frame());
    CHECK_FALSE(session->take_latest_frame());

    REQUIRE(session->send("frame"));
    until(*session, [&](const auto&) {
        return session->latest_frame() && session->latest_frame()->info.frame_id == 2;
    });
    const auto* next_pixels = session->latest_frame()->rgba.data();
    auto next = session->take_latest_frame();
    REQUIRE(next);
    CHECK(next->rgba.data() == next_pixels);
    CHECK(next->info.frame_id == 2);
    CHECK(next->rgba == displayed->rgba);
    CHECK(displayed->rgba.data() == pixels);
    CHECK(displayed->info.frame_id == 1);
    CHECK_FALSE(session->latest_frame());
    CHECK_FALSE(session->take_latest_frame());
}

TEST_CASE("Caller-owned preview images survive worker failure and replacement", "[editor][preview]") {
    std::optional<preview::PreviewFrame> displayed;
    const std::byte* pixels{};
    {
        auto session = preview::PreviewSession::create(config());
        REQUIRE(session);
        until(*session, [&](const auto&) { return session->active_generation() != 0; });
        const auto generation = session->active_generation();
        pixels = session->latest_frame()->rgba.data();
        displayed = session->take_latest_frame();
        REQUIRE(displayed);

        REQUIRE(session->send("crash"));
        until(*session, [](const auto& events) { return contains(events, preview::EventKind::worker_failed); });
        CHECK(session->active_generation() == 0);
        CHECK_FALSE(session->latest_frame());
        CHECK_FALSE(session->take_latest_frame());
        CHECK(displayed->rgba.data() == pixels);
        CHECK(displayed->info.generation == generation);
        CHECK(displayed->rgba.front() == std::byte{7});

        REQUIRE(session->request_reload());
        until(*session, [&](const auto&) { return session->active_generation() > generation; });
        REQUIRE(session->latest_frame());
        CHECK(session->latest_frame()->info.generation > generation);
        CHECK(session->latest_frame()->rgba == displayed->rgba);
        CHECK(displayed->info.generation == generation);
        CHECK(displayed->rgba.data() == pixels);
    }
    // The displayed pixels also outlive the session and its shared-memory mappings.
    REQUIRE(displayed);
    CHECK(displayed->rgba.data() == pixels);
    CHECK(displayed->rgba.size() == 16);
    CHECK(displayed->rgba.front() == std::byte{7});
    CHECK(displayed->rgba.back() == std::byte{255});
}

TEST_CASE("Caller-owned preview images survive a failed rebuild", "[editor][preview]") {
    auto session = preview::PreviewSession::create(config({"/bin/false"}));
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    const auto generation = session->active_generation();
    auto displayed = session->take_latest_frame();
    REQUIRE(displayed);
    const auto* pixels = displayed->rgba.data();
    REQUIRE(session->request_reload());
    until(*session, [](const auto& events) { return contains(events, preview::EventKind::build_failed); });
    CHECK(session->active_generation() == generation);
    CHECK_FALSE(session->latest_frame());
    CHECK_FALSE(session->take_latest_frame());
    CHECK(displayed->rgba.data() == pixels);
    CHECK(displayed->info.generation == generation);
    CHECK(displayed->rgba.front() == std::byte{7});
    REQUIRE(session->send("frame"));
    until(*session, [&](const auto&) {
        return session->latest_frame() && session->latest_frame()->info.frame_id == 2;
    });
}

TEST_CASE("Taking a frame from a moved-from session is harmless", "[editor][preview]") {
    auto session = preview::PreviewSession::create(config());
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    const auto* pixels = session->latest_frame()->rgba.data();
    auto moved = std::move(*session);
    CHECK_FALSE(session->latest_frame());
    CHECK_FALSE(session->take_latest_frame());
    auto displayed = moved.take_latest_frame();
    REQUIRE(displayed);
    CHECK(displayed->rgba.data() == pixels);
    CHECK_FALSE(moved.latest_frame());
    CHECK_FALSE(moved.take_latest_frame());
}

TEST_CASE("Failed builds preserve the running worker and image", "[editor][preview]") {
    auto session = preview::PreviewSession::create(config({"/bin/false"}));
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    const auto generation = session->active_generation();
    const auto image = session->latest_frame()->rgba;
    REQUIRE(session->request_reload());
    until(*session, [](const auto& e) { return contains(e, preview::EventKind::build_failed); });
    CHECK(session->active_generation() == generation);
    CHECK(session->latest_frame()->rgba == image);
    REQUIRE(session->send("frame"));
    until(*session, [&](const auto&) { return session->latest_frame()->info.frame_id == 2; });
}

TEST_CASE("Worker crashes retain the last frame and a reload recovers", "[editor][preview]") {
    auto session = preview::PreviewSession::create(config());
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    const auto image = session->latest_frame()->rgba;
    const auto generation = session->active_generation();
    REQUIRE(session->send("crash"));
    until(*session, [](const auto& e) { return contains(e, preview::EventKind::worker_failed); });
    CHECK(session->active_generation() == 0);
    CHECK(session->latest_frame()->rgba == image);
    REQUIRE(session->request_reload());
    until(*session, [&](const auto&) { return session->active_generation() > generation; });
}

TEST_CASE("A candidate startup timeout cannot take down the active preview", "[editor][preview]") {
    auto options = config();
    options.startup_timeout = std::chrono::milliseconds(300);
    auto session = preview::PreviewSession::create(std::move(options));
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    const auto generation = session->active_generation();
    REQUIRE(session->request_reload());
    until(*session, [](const auto& e) { return contains(e, preview::EventKind::worker_failed); }, false);
    CHECK(session->active_generation() == generation);
    CHECK(session->latest_frame()->info.generation == generation);
    CHECK_FALSE(session->busy());
}

TEST_CASE("Build timeouts and invalid worker packets preserve completed output", "[editor][preview]") {
    auto options = config({"/bin/sleep", "10"});
    options.build_timeout = std::chrono::milliseconds(25);
    auto session = preview::PreviewSession::create(std::move(options));
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    const auto generation = session->active_generation();
    REQUIRE(session->request_reload());
    until(*session, [](const auto& e) { return contains(e, preview::EventKind::build_failed); });
    CHECK(session->active_generation() == generation);
    REQUIRE(session->send("malformed-packet"));
    until(*session, [](const auto& e) { return contains(e, preview::EventKind::worker_failed); });
    CHECK(session->latest_frame()->info.generation == generation);
}

TEST_CASE("Preview build arguments are not interpreted as shell commands", "[editor][preview]") {
    auto session = preview::PreviewSession::create(config({"/usr/bin/printf", "%s", "literal; $(not-a-command)"}));
    REQUIRE(session);
    until(*session, [&](const auto&) { return session->active_generation() != 0; });
    const auto generation = session->active_generation();
    REQUIRE(session->request_reload());
    until(*session, [&](const auto&) { return session->active_generation() > generation; });
    CHECK(session->logs() == "literal; $(not-a-command)");
}

TEST_CASE("Preview configuration errors are reported without launching workers", "[editor][preview]") {
    auto options = config();
    options.max_width = 0;
    CHECK_FALSE(preview::PreviewSession::create(options));
    options = config();
    options.worker_executable = "/definitely/no/vng-preview-executable";
    CHECK_FALSE(preview::PreviewSession::create(options));
    options = config();
    options.startup_timeout = std::chrono::milliseconds(0);
    CHECK_FALSE(preview::PreviewSession::create(options));
    CHECK_FALSE(preview::WorkerEndpoint::attach(-1, -1, 0));
}

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--worker")
        return worker_main(std::atoi(argv[2]), std::atoi(argv[3]), std::strtoull(argv[4], nullptr, 10));
    executable = std::filesystem::canonical("/proc/self/exe");
    return Catch::Session().run(argc, argv);
}
