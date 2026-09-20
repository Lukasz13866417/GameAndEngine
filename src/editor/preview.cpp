#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <vng/editor/preview.hpp>
#include "preview_surface.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <unistd.h>

extern char** environ;

namespace vng::editor::preview {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t magic = 0x564e475052455634ULL;
constexpr std::string_view ready_message = "@vng/preview/ready/4";
constexpr std::size_t max_log_bytes = 128 * 1024;
Diagnostic error(std::string message) { return {std::move(message)}; }
Diagnostic system_error(std::string_view operation, int code = errno) {
    return error(std::string(operation) + ": " + std::strerror(code));
}
struct Fd {
    int value{-1};
    Fd() = default;
    explicit Fd(int v) : value(v) {}
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { reset(); value = std::exchange(other.value, -1); }
        return *this;
    }
    ~Fd() { reset(); }
    void reset() noexcept { if (value >= 0) ::close(std::exchange(value, -1)); }
};

// No C++ containers or pointers live in this mapping. The mutex is robust so
// a worker dying halfway through memcpy cannot wedge the editor or expose a
// partially written frame. Both processes try the lock; neither waits on it.
using detail::SharedHeader;
struct Surface {
    Fd fd;
    void* mapping{MAP_FAILED};
    std::size_t mapping_size{};
    Surface() = default;
    Surface(Surface&& other) noexcept
        : fd(std::move(other.fd)), mapping(std::exchange(other.mapping, MAP_FAILED)),
          mapping_size(std::exchange(other.mapping_size, 0)) {}
    Surface& operator=(Surface&& other) noexcept {
        if (this != &other) {
            if (mapping != MAP_FAILED) munmap(mapping, mapping_size);
            fd = std::move(other.fd);
            mapping = std::exchange(other.mapping, MAP_FAILED);
            mapping_size = std::exchange(other.mapping_size, 0);
        }
        return *this;
    }
    ~Surface() { if (mapping != MAP_FAILED) munmap(mapping, mapping_size); }
    SharedHeader& header() const { return *static_cast<SharedHeader*>(mapping); }
    std::byte* pixels() const { return static_cast<std::byte*>(mapping) + sizeof(SharedHeader); }
    static Result<Surface> create(std::uint32_t w, std::uint32_t h, std::uint64_t generation) {
        if (!w || !h || w > 8192 || h > 8192 || std::uint64_t(w) * h > 16 * 1024 * 1024)
            return std::unexpected(error("Preview capacity must be positive and at most 16 megapixels / 8192 per axis"));
        Surface result;
        result.fd = Fd(memfd_create("vng-preview-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        if (result.fd.value < 0) return std::unexpected(system_error("memfd_create"));
        result.mapping_size = sizeof(SharedHeader) + std::size_t(w) * h * 4;
        if (ftruncate(result.fd.value, static_cast<off_t>(result.mapping_size)) < 0)
            return std::unexpected(system_error("ftruncate preview"));
        // A worker may write frames but may not truncate the backing storage.
        if (fcntl(result.fd.value, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
            return std::unexpected(system_error("seal preview storage"));
        result.mapping = mmap(nullptr, result.mapping_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, result.fd.value, 0);
        if (result.mapping == MAP_FAILED) return std::unexpected(system_error("mmap preview"));
        std::construct_at(static_cast<SharedHeader*>(result.mapping));
        auto& header = result.header();
        header.signature = magic;
        header.max_width = w;
        header.max_height = h;
        header.generation = generation;
        pthread_mutexattr_t attributes;
        int rc = pthread_mutexattr_init(&attributes);
        if (rc) return std::unexpected(system_error("mutex attributes", rc));
        rc = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
        if (!rc) rc = pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
        if (!rc) rc = pthread_mutex_init(&header.mutex, &attributes);
        pthread_mutexattr_destroy(&attributes);
        if (rc) return std::unexpected(system_error("shared robust mutex", rc));
        return result;
    }
    static Result<Surface> attach(Fd fd, std::uint64_t generation) {
        Surface result;
        result.fd = std::move(fd);
        struct stat stat{};
        if (fstat(result.fd.value, &stat) < 0) return std::unexpected(system_error("fstat preview"));
        if (stat.st_size < static_cast<off_t>(sizeof(SharedHeader)) ||
            stat.st_size > static_cast<off_t>(sizeof(SharedHeader) + 64 * 1024 * 1024))
            return std::unexpected(error("Invalid shared preview storage size"));
        result.mapping_size = static_cast<std::size_t>(stat.st_size);
        result.mapping = mmap(nullptr, result.mapping_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, result.fd.value, 0);
        if (result.mapping == MAP_FAILED) return std::unexpected(system_error("mmap worker preview"));
        auto& h = result.header();
        if (h.signature != magic || h.generation != generation || !h.max_width || !h.max_height ||
            h.max_width > 8192 || h.max_height > 8192 ||
            std::uint64_t(h.max_width) * h.max_height * 4 != result.mapping_size - sizeof(SharedHeader))
            return std::unexpected(error("Incompatible shared preview header or generation"));
        return result;
    }
    Result<bool> copy_latest(std::uint64_t& sequence, std::optional<PreviewFrame>& output) {
        auto& h = header();
        const int rc = pthread_mutex_trylock(&h.mutex);
        if (rc == EBUSY) return false;
        if (rc == EOWNERDEAD) {
            h.valid = 0;
            pthread_mutex_consistent(&h.mutex);
            pthread_mutex_unlock(&h.mutex);
            return false;
        }
        if (rc) return std::unexpected(system_error("lock preview image", rc));
        struct Unlock { pthread_mutex_t* p; ~Unlock() { pthread_mutex_unlock(p); } } unlock{&h.mutex};
        if (!h.valid || h.sequence == sequence) return false;
        const auto info = h.info;
        if (!info.width || !info.height || info.width > h.max_width || info.height > h.max_height ||
            info.generation != h.generation || !std::isfinite(info.time))
            return std::unexpected(error("Worker published invalid frame metadata"));
        const auto size = std::size_t(info.width) * info.height * 4;
        if (size > mapping_size - sizeof(SharedHeader))
            return std::unexpected(error("Worker frame exceeds its shared image capacity"));
        PreviewFrame next{info, std::vector<std::byte>(size)};
        std::memcpy(next.rgba.data(), pixels(), size);
        next.info.interaction.ui_received_ns = monotonic_ns();
        sequence = h.sequence;
        output = std::move(next);
        return true;
    }
};

struct Channel {
    Fd fd;
    std::deque<std::string> outgoing;
    std::size_t queued_bytes{}, sent{};
    std::string incoming;
    std::uint32_t incoming_size{};
    bool closed{};
};
constexpr std::size_t packet_bytes = 60 * 1024;
void write_u32(char* p, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<char>((value >> (i * 8)) & 255U);
}
std::uint32_t read_u32(const char* p) {
    std::uint32_t result{};
    for (int i = 0; i < 4; ++i) result |= std::uint32_t(static_cast<unsigned char>(p[i])) << (i * 8);
    return result;
}
Result<void> flush(Channel& channel) {
    std::array<char, packet_bytes> packet{};
    for (int i = 0; i < 16 && !channel.outgoing.empty(); ++i) {
        const auto& message = channel.outgoing.front();
        const auto count = std::min(packet.size() - 8, message.size() - channel.sent);
        write_u32(packet.data(), static_cast<std::uint32_t>(message.size()));
        write_u32(packet.data() + 4, static_cast<std::uint32_t>(channel.sent));
        std::memcpy(packet.data() + 8, message.data() + channel.sent, count);
        const auto n = ::send(channel.fd.value, packet.data(), count + 8, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return {};
            if (errno == EINTR) continue;
            return std::unexpected(system_error("send preview packet"));
        }
        if (static_cast<std::size_t>(n) != count + 8)
            return std::unexpected(error("Incomplete preview packet"));
        channel.sent += count;
        if (channel.sent == message.size()) {
            channel.queued_bytes -= message.size();
            channel.outgoing.pop_front();
            channel.sent = 0;
        }
    }
    return {};
}
Result<void> send_message(Channel& channel, std::string_view message) {
    if (message.empty() || message.size() > max_message_bytes)
        return std::unexpected(error("Preview message must contain 1.." +
                                     std::to_string(max_message_bytes) + " bytes"));
    if (channel.closed) return std::unexpected(error("Preview channel is closed"));
    if (channel.queued_bytes + message.size() > 4 * max_message_bytes)
        return std::unexpected(error("Preview outgoing queue is full (64 MiB limit)"));
    channel.outgoing.emplace_back(message);
    channel.queued_bytes += message.size();
    return flush(channel);
}
Result<std::vector<std::string>> receive_messages(Channel& channel) {
    std::vector<std::string> result;
    if (auto sent = flush(channel); !sent) return std::unexpected(sent.error());
    std::array<char, packet_bytes> buffer{};
    // Bounded per poll; a worker cannot monopolize the UI event loop.
    for (std::size_t i = 0; i < 64; ++i) {
        const auto n = recv(channel.fd.value, buffer.data(), buffer.size(), MSG_DONTWAIT | MSG_TRUNC);
        if (n == 0) { channel.closed = true; break; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return std::unexpected(system_error("receive preview message"));
        }
        if (n < 9 || static_cast<std::size_t>(n) > buffer.size())
            return std::unexpected(error("Oversized preview packet rejected"));
        const auto total = read_u32(buffer.data());
        const auto offset = read_u32(buffer.data() + 4);
        const auto count = static_cast<std::size_t>(n) - 8;
        if (!total || total > max_message_bytes || offset != channel.incoming.size() ||
            std::uint64_t(offset) + count > total || (offset && total != channel.incoming_size))
            return std::unexpected(error("Malformed preview message fragments"));
        if (!offset) { channel.incoming_size = total; channel.incoming.reserve(total); }
        channel.incoming.append(buffer.data() + 8, count);
        if (channel.incoming.size() == total) {
            result.push_back(std::move(channel.incoming));
            channel.incoming = {};
            channel.incoming_size = 0;
        }
    }
    return result;
}

struct Child {
    pid_t pid{-1};
    Fd output;
    Clock::time_point started{};
    Child() = default;
    Child(Child&& other) noexcept
        : pid(std::exchange(other.pid, -1)), output(std::move(other.output)), started(other.started) {}
    Child& operator=(Child&& other) noexcept {
        if (this != &other) { terminate(); pid = std::exchange(other.pid, -1);
            output = std::move(other.output); started = other.started; }
        return *this;
    }
    ~Child() { terminate(); }
    void signal(int sig) const {
        if (pid <= 0) return;
        siginfo_t info{};
        // If some external SIGCHLD handler reaped our child, its numeric PID
        // must no longer be used as authority to signal a possibly reused group.
        if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) < 0 && errno == ECHILD)
            return;
        ::kill(-pid, sig);
    }
    std::optional<int> exit_status() {
        if (pid <= 0) return std::nullopt;
        siginfo_t info{};
        if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
            if (errno == ECHILD) { pid = -1; return -1; }
            return std::nullopt;
        }
        if (!info.si_pid) return std::nullopt;
        // Terminate descendants before reaping the group leader, preventing its
        // PID/group ID from being recycled before this last group operation.
        signal(SIGKILL);
        int status{};
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    void terminate() noexcept {
        if (pid <= 0) return;
        signal(SIGKILL);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        pid = -1;
    }
};
Result<Child> spawn(const std::vector<std::string>& command, const std::filesystem::path& cwd,
                    int control_fd = -1, int memory_fd = -1) {
    if (command.empty() || command.front().empty()) return std::unexpected(error("Empty process command"));
    for (const auto& arg : command) if (arg.find('\0') != std::string::npos)
        return std::unexpected(error("Process argument contains a NUL byte"));
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0) return std::unexpected(system_error("process log pipe"));
    Fd read{pipe_fds[0]}, write{pipe_fds[1]};
    // Child stdout is blocking, while the parent only performs nonblocking reads.
    fcntl(write.value, F_SETFL, fcntl(write.value, F_GETFL) & ~O_NONBLOCK);
    Fd control, memory;
    if (control_fd >= 0) {
        control = Fd(fcntl(control_fd, F_DUPFD_CLOEXEC, 200));
        memory = Fd(fcntl(memory_fd, F_DUPFD_CLOEXEC, 200));
        if (control.value < 0 || memory.value < 0)
            return std::unexpected(system_error("duplicate worker descriptors"));
    }
    std::vector<char*> argv;
    for (const auto& arg : command) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) return std::unexpected(system_error("spawn actions", rc));
    rc = posix_spawnattr_init(&attributes);
    if (rc) { posix_spawn_file_actions_destroy(&actions); return std::unexpected(system_error("spawn attributes", rc)); }
    auto cleanup = [&] { posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes); };
    rc = posix_spawn_file_actions_adddup2(&actions, write.value, STDOUT_FILENO);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, write.value, STDERR_FILENO);
    if (!rc && control.value >= 0) rc = posix_spawn_file_actions_adddup2(&actions, control.value, 3);
    if (!rc && control.value >= 0) rc = posix_spawn_file_actions_adddup2(&actions, memory.value, 4);
    if (!rc) rc = posix_spawn_file_actions_addclosefrom_np(&actions, control.value >= 0 ? 5 : 3);
    if (!rc && !cwd.empty()) rc = posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    if (!rc) rc = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (!rc) rc = posix_spawnattr_setpgroup(&attributes, 0);
    Child result;
    if (!rc) rc = posix_spawnp(&result.pid, argv[0], &actions, &attributes, argv.data(), environ);
    cleanup();
    if (rc) return std::unexpected(system_error("launch " + command.front(), rc));
    result.output = std::move(read);
    result.started = Clock::now();
    return result;
}
void read_logs(Child& child, std::string& logs) {
    std::array<char, 8192> bytes{};
    for (int i = 0; i < 16; ++i) {
        const auto n = read(child.output.value, bytes.data(), bytes.size());
        if (n <= 0) break;
        logs.append(bytes.data(), static_cast<std::size_t>(n));
        if (logs.size() > max_log_bytes) logs.erase(0, logs.size() - max_log_bytes);
    }
}
} // namespace

struct WorkerEndpoint::Impl {
    Channel control;
    Surface surface;
    std::uint64_t generation{}, last_frame{};
};
WorkerEndpoint::WorkerEndpoint(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WorkerEndpoint::WorkerEndpoint(WorkerEndpoint&&) noexcept = default;
WorkerEndpoint& WorkerEndpoint::operator=(WorkerEndpoint&&) noexcept = default;
WorkerEndpoint::~WorkerEndpoint() = default;
Result<WorkerEndpoint> WorkerEndpoint::attach(int control_fd, int memory_fd, std::uint64_t generation) {
    Fd control{control_fd}, memory{memory_fd};
    if (control_fd < 0 || memory_fd < 0 || control_fd == memory_fd || !generation)
        return std::unexpected(error("Invalid worker descriptors or generation"));
    int socket_type{};
    socklen_t size = sizeof(socket_type);
    if (getsockopt(control.value, SOL_SOCKET, SO_TYPE, &socket_type, &size) < 0 || socket_type != SOCK_SEQPACKET)
        return std::unexpected(error("Worker control descriptor is not a packet socket"));
    if (fcntl(control.value, F_SETFD, FD_CLOEXEC) < 0 || fcntl(memory.value, F_SETFD, FD_CLOEXEC) < 0)
        return std::unexpected(system_error("protect inherited preview descriptors"));
    auto surface = Surface::attach(std::move(memory), generation);
    if (!surface) return std::unexpected(surface.error());
    auto impl = std::make_unique<Impl>();
    impl->control.fd = std::move(control);
    impl->surface = std::move(*surface);
    impl->generation = generation;
    return WorkerEndpoint(std::move(impl));
}
Result<void> WorkerEndpoint::send(std::string_view message) {
    if (!impl_) return std::unexpected(error("Moved-from worker endpoint"));
    return send_message(impl_->control, message);
}
Result<std::vector<std::string>> WorkerEndpoint::receive() {
    if (!impl_) return std::unexpected(error("Moved-from worker endpoint"));
    return receive_messages(impl_->control);
}
Result<void> WorkerEndpoint::ready() { return send(ready_message); }
bool WorkerEndpoint::closed() const noexcept { return !impl_ || impl_->control.closed; }
Extent2D WorkerEndpoint::capacity() const noexcept {
    if (!impl_) return {};
    const auto& header = impl_->surface.header();
    return {header.max_width, header.max_height};
}
Result<bool> WorkerEndpoint::try_publish(FrameInfo info, std::span<const std::byte> rgba) {
    if (!impl_) return std::unexpected(error("Moved-from worker endpoint"));
    auto& h = impl_->surface.header();
    if (!info.width || !info.height || info.width > h.max_width || info.height > h.max_height ||
        info.generation != impl_->generation || info.frame_id <= impl_->last_frame || !std::isfinite(info.time) ||
        rgba.size() != std::size_t(info.width) * info.height * 4)
        return std::unexpected(error("Invalid frame size, generation, time, or non-increasing frame ID"));
    int rc = pthread_mutex_trylock(&h.mutex);
    if (rc == EBUSY) return false;
    if (rc == EOWNERDEAD) { h.valid = 0; rc = pthread_mutex_consistent(&h.mutex); }
    if (rc) return std::unexpected(system_error("lock worker preview", rc));
    h.valid = 0;
    std::memcpy(impl_->surface.pixels(), rgba.data(), rgba.size());
    info.interaction.published_ns = monotonic_ns();
    h.info = info;
    ++h.sequence;
    h.valid = 1;
    impl_->last_frame = info.frame_id;
    pthread_mutex_unlock(&h.mutex);
    return true;
}

struct PreviewSession::Impl {
    struct Worker {
        Child process;
        std::filesystem::path executable;
        Channel control;
        std::optional<std::string> latest_request;
        Surface surface;
        std::uint64_t generation{}, sequence{};
        bool ready{};
        Clock::time_point retired{};
        std::optional<PreviewFrame> frame;
    };
    PreviewConfig config;
    std::filesystem::path directory;
    std::vector<std::filesystem::path> executables;
    std::unique_ptr<Worker> active, candidate;
    std::vector<std::unique_ptr<Worker>> retired;
    std::optional<Child> build;
    bool build_timed_out{};
    std::optional<PreviewFrame> frame;
    std::vector<PreviewEvent> pending;
    std::uint64_t next_generation{1};
    std::string state{"Starting preview"}, log;

    ~Impl() {
        // Request orderly exit first. No wait is performed in the UI polling
        // path; final teardown grants a short bounded grace interval.
        auto request = [](auto& worker) { if (worker) (void)send_message(worker->control, "shutdown"); };
        request(active); request(candidate);
        for (auto& worker : retired) request(worker);
        if (build) build->signal(SIGTERM);
        const auto deadline = Clock::now() + std::chrono::milliseconds(150);
        while (Clock::now() < deadline) {
            bool running{};
            auto reap = [&](auto& worker) { if (worker && worker->process.pid > 0) {
                worker->process.exit_status(); running |= worker->process.pid > 0;
            }};
            reap(active); reap(candidate);
            for (auto& worker : retired) reap(worker);
            if (build && build->pid > 0) { build->exit_status(); running |= build->pid > 0; }
            if (!running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        active.reset(); candidate.reset(); retired.clear(); build.reset();
        for (const auto& path : executables) { std::error_code ec; std::filesystem::remove(path, ec); }
        if (!directory.empty()) { std::error_code ec; std::filesystem::remove(directory, ec); }
    }
    Result<void> launch() {
        const auto generation = next_generation++;
        auto surface = Surface::create(config.max_width, config.max_height, generation);
        if (!surface) return std::unexpected(surface.error());
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) < 0)
            return std::unexpected(system_error("preview socketpair"));
        Fd parent{sockets[0]}, child{sockets[1]};
        const auto executable = directory / ("worker-" + std::to_string(generation));
        std::error_code ec;
        std::filesystem::copy_file(config.worker_executable, executable, ec);
        if (ec) return std::unexpected(error("Copy worker executable: " + ec.message()));
        executables.push_back(executable);
        auto process = spawn({executable.string(), "--worker", "3", "4", std::to_string(generation)},
                             config.build_directory, child.value, surface->fd.value);
        if (!process) return std::unexpected(process.error());
        auto worker = std::make_unique<Worker>();
        worker->process = std::move(*process);
        worker->executable = executable;
        worker->control.fd = std::move(parent);
        worker->surface = std::move(*surface);
        worker->generation = generation;
        candidate = std::move(worker);
        state = "Starting preview generation " + std::to_string(generation);
        pending.push_back({EventKind::candidate_started, generation, {}});
        return {};
    }
    void retire(std::unique_ptr<Worker> worker) {
        if (!worker) return;
        (void)send_message(worker->control, "shutdown");
        worker->retired = Clock::now();
        retired.push_back(std::move(worker));
    }
    static Result<void> flush_requests(Worker& worker) {
        if (auto sent = flush(worker.control); !sent) return sent;
        if (worker.latest_request && worker.control.outgoing.empty()) {
            auto sent = send_message(worker.control, *worker.latest_request);
            if (!sent) return sent;
            worker.latest_request.reset();
        }
        return {};
    }
    bool inspect(Worker& worker, std::vector<PreviewEvent>& events) {
        read_logs(worker.process, log);
        auto messages = receive_messages(worker.control);
        if (!messages) {
            events.push_back({EventKind::worker_failed, worker.generation, messages.error().message});
            return false;
        }
        for (auto& message : *messages) {
            if (message == ready_message) worker.ready = true;
            else events.push_back({EventKind::message, worker.generation, std::move(message)});
        }
        if (auto sent = flush_requests(worker); !sent) {
            events.push_back({EventKind::worker_failed, worker.generation, sent.error().message});
            return false;
        }
        auto copied = worker.surface.copy_latest(worker.sequence, worker.frame);
        if (!copied) {
            events.push_back({EventKind::worker_failed, worker.generation, copied.error().message});
            return false;
        }
        if (auto exit = worker.process.exit_status()) {
            read_logs(worker.process, log);
            events.push_back({EventKind::worker_failed, worker.generation,
                              "Preview worker exited with status " + std::to_string(*exit)});
            return false;
        }
        if (worker.control.closed) {
            events.push_back({EventKind::worker_failed, worker.generation, "Preview worker disconnected"});
            return false;
        }
        return true;
    }
};

PreviewSession::PreviewSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PreviewSession::PreviewSession(PreviewSession&&) noexcept = default;
PreviewSession& PreviewSession::operator=(PreviewSession&&) noexcept = default;
PreviewSession::~PreviewSession() = default;
Result<PreviewSession> PreviewSession::create(PreviewConfig config) {
    if (config.worker_executable.empty() || config.build_timeout.count() <= 0 || config.startup_timeout.count() <= 0)
        return std::unexpected(error("Preview requires an executable and positive timeouts"));
    std::error_code ec;
    config.worker_executable = std::filesystem::absolute(config.worker_executable, ec);
    if (ec) return std::unexpected(error("Invalid worker executable path: " + ec.message()));
    if (!config.build_directory.empty()) {
        config.build_directory = std::filesystem::absolute(config.build_directory, ec);
        if (ec) return std::unexpected(error("Invalid build directory: " + ec.message()));
    }
    auto impl = std::make_unique<Impl>();
    impl->config = std::move(config);
    std::array<char, 32> path{};
    std::strcpy(path.data(), "/tmp/vng-preview-XXXXXX");
    if (!mkdtemp(path.data())) return std::unexpected(system_error("create preview generation directory"));
    impl->directory = path.data();
    if (auto started = impl->launch(); !started) return std::unexpected(started.error());
    return PreviewSession(std::move(impl));
}
Result<void> PreviewSession::request_reload() {
    if (!impl_) return std::unexpected(error("Moved-from preview session"));
    if (busy()) return std::unexpected(error("A preview build or startup is already in progress"));
    if (impl_->retired.size() >= 4)
        return std::unexpected(error("Waiting for retired preview workers to stop before another reload"));
    auto build = spawn(impl_->config.build_command, impl_->config.build_directory);
    if (!build) return std::unexpected(build.error());
    impl_->log.clear();
    impl_->build = std::move(*build);
    impl_->build_timed_out = false;
    impl_->state = "Building project preview";
    return {};
}
std::vector<PreviewEvent> PreviewSession::poll() {
    if (!impl_) return {};
    auto& s = *impl_;
    auto events = std::exchange(s.pending, {});
    if (s.build) {
        read_logs(*s.build, s.log);
        if (auto exit = s.build->exit_status()) {
            read_logs(*s.build, s.log);
            s.build.reset();
            if (*exit != 0) {
                s.state = "Build failed; keeping previous preview";
                events.push_back({EventKind::build_failed, 0, s.build_timed_out
                    ? "Build exceeded its configured timeout"
                    : "Build exited with status " + std::to_string(*exit)});
            } else {
                events.push_back({EventKind::build_finished, 0, {}});
                if (auto launched = s.launch(); !launched) {
                    s.state = "Could not start replacement; keeping previous preview";
                    events.push_back({EventKind::worker_failed, 0, launched.error().message});
                }
                for (auto& event : s.pending) events.push_back(std::move(event));
                s.pending.clear();
            }
        } else if (!s.build_timed_out && Clock::now() - s.build->started > s.config.build_timeout) {
            s.build->signal(SIGKILL);
            s.build_timed_out = true;
            // Reaped on a subsequent poll, so even timeout handling never waits.
            s.state = "Build timed out; keeping previous preview";
        }
    }
    if (s.active) {
        if (!s.inspect(*s.active, events)) {
            s.state = "Preview stopped; last completed image retained";
            s.retire(std::move(s.active));
        } else if (s.active->frame) {
            s.frame = std::move(s.active->frame);
            s.active->frame.reset();
        }
    }
    if (s.candidate) {
        if (!s.inspect(*s.candidate, events)) {
            s.state = "Replacement failed; keeping previous preview";
            s.retire(std::move(s.candidate));
        } else if (s.candidate->ready && s.candidate->frame) {
            s.retire(std::move(s.active));
            s.active = std::move(s.candidate);
            s.frame = std::move(s.active->frame);
            s.active->frame.reset();
            s.state = "Preview running";
            events.push_back({EventKind::activated, s.active->generation, {}});
        } else if (Clock::now() - s.candidate->process.started > s.config.startup_timeout) {
            events.push_back({EventKind::worker_failed, s.candidate->generation,
                              "Replacement worker startup timed out; previous preview retained"});
            s.state = "Replacement startup timed out";
            s.retire(std::move(s.candidate));
        }
    }
    for (auto& worker : s.retired) {
        (void)flush(worker->control);
        read_logs(worker->process, s.log);
        worker->process.exit_status();
        const auto elapsed = Clock::now() - worker->retired;
        if (elapsed > std::chrono::seconds(2)) worker->process.signal(SIGKILL);
        else if (elapsed > std::chrono::milliseconds(500)) worker->process.signal(SIGTERM);
    }
    std::erase_if(s.retired, [&](const auto& worker) {
        if (worker->process.pid > 0) return false;
        std::error_code ec;
        std::filesystem::remove(worker->executable, ec);
        if (!ec) std::erase(s.executables, worker->executable);
        return true;
    });
    return events;
}
Result<void> PreviewSession::send(std::string_view message, std::uint64_t generation) {
    if (!impl_) return std::unexpected(error("Moved-from preview session"));
    auto* worker = impl_->active ? impl_->active.get() : impl_->candidate.get();
    if (generation) {
        worker = nullptr;
        if (impl_->active && impl_->active->generation == generation) worker = impl_->active.get();
        if (impl_->candidate && impl_->candidate->generation == generation) worker = impl_->candidate.get();
    }
    if (!worker) return std::unexpected(error("No worker for the requested preview generation"));
    return send_message(worker->control, message);
}
Result<void> PreviewSession::send_latest(std::string_view message, std::uint64_t generation) {
    if (!impl_) return std::unexpected(error("Moved-from preview session"));
    if (message.empty() || message.size() > max_message_bytes)
        return std::unexpected(error("Invalid latest-value packet size"));
    auto* worker = impl_->active ? impl_->active.get() : impl_->candidate.get();
    if (generation) {
        worker = nullptr;
        if (impl_->active && impl_->active->generation == generation) worker = impl_->active.get();
        if (impl_->candidate && impl_->candidate->generation == generation) worker = impl_->candidate.get();
    }
    if (!worker) return std::unexpected(error("No worker for the requested preview generation"));
    worker->latest_request = std::string(message);
    return {};
}
Result<void> PreviewSession::flush_requests() {
    if (!impl_) return std::unexpected(error("Moved-from preview session"));
    for (auto* worker : {impl_->active.get(), impl_->candidate.get()}) {
        if (worker)
            if (auto sent = Impl::flush_requests(*worker); !sent) return sent;
    }
    return {};
}
const std::optional<PreviewFrame>& PreviewSession::latest_frame() const noexcept {
    static const std::optional<PreviewFrame> empty;
    return impl_ ? impl_->frame : empty;
}
std::optional<PreviewFrame> PreviewSession::take_latest_frame() noexcept {
    return impl_ ? std::exchange(impl_->frame, std::nullopt) : std::nullopt;
}
std::uint64_t PreviewSession::active_generation() const noexcept {
    return impl_ && impl_->active ? impl_->active->generation : 0;
}
bool PreviewSession::busy() const noexcept { return impl_ && (impl_->build || impl_->candidate); }
std::string_view PreviewSession::status() const noexcept { return impl_ ? std::string_view{impl_->state} : std::string_view{"No preview session"}; }
std::string_view PreviewSession::logs() const noexcept { return impl_ ? impl_->log : std::string_view{}; }

} // namespace vng::editor::preview
