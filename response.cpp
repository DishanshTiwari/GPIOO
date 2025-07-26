// alarm_controller.cpp
/// @brief Real-time home alarm controller with lock-free sensor event handling, persistent state, modular plugins, and real-time HTTP metrics.
/// @intuition: Divide the system into modular components: lock-free sensor input, resilient event loop, persistent memory-mapped logging, dynamic alerts, and efficient admin/telemetry.
/// @approach: Use lock-free SPSC event queue, in-place custom memory pools, UNIX mmap for persistence, plugin-based alerts (dlopen), file-descriptor polling, and a lightweight HTTP server for observability. Design for modularity, minimal runtime allocation, pluggability, and robust concurrency.
/// @complexity: Time: O(1) for sensor input/event push, O(N) for log flush/persistence (N = events since last flush)
///              Space: O(M) for memory-mapped logs + O(S) for static event pool

#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <semaphore.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <dlfcn.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/uio.h>

// ===== Constants =====

constexpr int MAX_EVENTS           = 1024;
constexpr int MAX_LOGS             = 4096;
constexpr int POOL_SIZE            = 2048;
constexpr int SENSOR_COUNT         = 3; // motion, door, smoke
constexpr int HTTP_PORT            = 9080;
constexpr int ADMIN_PORT           = 9090;
constexpr int MAX_PLUGINS          = 4;
constexpr char STATE_FILE[]        = "/tmp/alarm_state.mmap";
constexpr char LOG_FILE[]          = "/tmp/alarm_log.mmap";
constexpr int LOG_LINE_MAX         = 256;
constexpr int MAX_USERS            = 4;

// ===== Strong Typing & Enums =====

enum class SensorType : uint8_t { Motion, Door, Smoke };
enum class EventType  : uint8_t { Trigger, Reset, Arm, Disarm, Tamper, Fault };
enum class LogLevel   : uint8_t { Trace, Info, Warn, Error, Critical };
enum class Role       : uint8_t { Admin, User, Viewer };

// ===== Event Model =====

struct alignas(8) Event {
    SensorType sensor;
    EventType  type;
    uint64_t   timestamp;      // monotonic UNIX ns
    char       message[64];
};

// ===== Lock-free SPSC Event Queue =====

class EventQueue final {
    alignas(64) std::array<Event, MAX_EVENTS> buffer_{};
    alignas(64) std::atomic<size_t> head_{0}, tail_{0};
public:
    bool push(const Event& e) noexcept {
        const auto h = head_.load(std::memory_order_relaxed);
        const auto n = (h + 1) % MAX_EVENTS;
        if (n == tail_.load(std::memory_order_acquire)) return false;
        buffer_[h] = e;
        head_.store(n, std::memory_order_release);
        return true;
    }
    std::optional<Event> pop() noexcept {
        auto t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return std::nullopt;
        Event e = buffer_[t];
        tail_.store((t + 1) % MAX_EVENTS, std::memory_order_release);
        return e;
    }
    [[nodiscard]] size_t size() const noexcept {
        size_t h = head_, t = tail_;
        return (h >= t) ? h - t : MAX_EVENTS - t + h;
    }
};

// ===== Memory Pool for Event/Log structs =====

template<typename T, size_t N>
class MemPool final {
    alignas(64) std::array<T, N>        pool_{};
    alignas(64) std::atomic<uint64_t>   bitset_{0};
public:
    T* allocate() noexcept {
        auto b = bitset_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < N; ++i) {
            uint64_t mask = 1ULL << i;
            if (!(b & mask)) {
                if (bitset_.compare_exchange_strong(b, b | mask))
                    return &pool_[i];
                // else restart (b is updated)
                i = 0, b = bitset_.load();
            }
        }
        return nullptr;
    }
    void free(T* ptr) noexcept {
        size_t idx = ptr - pool_.data();
        if (idx < N) bitset_.fetch_and(~(1ULL << idx));
    }
};

// ===== Memory-Mapped Persistence =====

class MMapFile final {
    int fd_{-1};
    void* base_{nullptr};
    size_t size_{0};
public:
    MMapFile(const char* fname, size_t sz, int prot = PROT_READ | PROT_WRITE)
        : size_{sz}
    {
        fd_ = ::open(fname, O_RDWR | O_CREAT, 0666);
        if (fd_ < 0) throw std::runtime_error("open failed");
        if (::ftruncate(fd_, sz) < 0) throw std::runtime_error("truncate failed");
        base_ = ::mmap(nullptr, sz, prot, MAP_SHARED, fd_, 0);
        if (!base_ || base_ == MAP_FAILED) throw std::runtime_error("mmap failed");
    }
    ~MMapFile() { if (base_) ::munmap(base_, size_); if (fd_>=0) ::close(fd_); }
    void* data()      noexcept { return base_; }
    const void* data() const noexcept { return base_; }
    size_t size() const noexcept { return size_; }
};

// ===== Logging: Timestamp, Role, Level, Message =====

struct LogEntry {
    uint64_t   timestamp;
    LogLevel   level;
    Role       role;
    char       text[LOG_LINE_MAX-16];
};

class LogBook final {
    LogEntry*    logEntries_ = nullptr;
    std::atomic<int> pos_{0};
    MMapFile     mmapLog_;
public:
    explicit LogBook(const char* filename, int maxLogs)
        : mmapLog_(filename, maxLogs * sizeof(LogEntry)) {
        logEntries_ = static_cast<LogEntry*>(mmapLog_.data());
    }
    void log(LogLevel lvl, Role role, const std::string_view& msg) noexcept {
        auto idx = pos_.fetch_add(1) % MAX_LOGS;
        auto& log = logEntries_[idx];
        log.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        log.level = lvl;
        log.role  = role;
        strncpy(log.text, msg.data(), sizeof(log.text)-1);
        log.text[sizeof(log.text)-1] = '\0';
    }
    std::vector<LogEntry> recent(size_t n) const {
        std::vector<LogEntry> logs;
        for (int i = 0; i < n && i < MAX_LOGS; ++i) logs.push_back(logEntries_[(pos_-i-1+MAX_LOGS)%MAX_LOGS]);
        return logs;
    }
};

// ===== Sensor (gpio) abstraction and debouncer =====

struct GpioSensor {
    int fd;
    SensorType type;
    int lastInput;
    int stableInput;
    int stableCount;
};

// Helper: open GPIO as input (sysfs e.g., /sys/class/gpio/gpioN/value)
inline int open_gpio(const char* path) {
    int fd = ::open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) throw std::runtime_error("gpio open failed");
    return fd;
}

// ===== Plugin Interface =====

using plugin_handle_t = void*;
struct AlertPlugin {
    plugin_handle_t handle;
    void (*trigger)(const char* msg);
    void (*reset)();
};

AlertPlugin load_plugin(const char* sofile) {
    plugin_handle_t h = ::dlopen(sofile, RTLD_LAZY);
    if (!h) throw std::runtime_error(dlerror());
    auto trg = reinterpret_cast<void(*)(const char*)>(dlsym(h, "plugin_alert_trigger"));
    auto rst = reinterpret_cast<void(*)()>(dlsym(h, "plugin_alert_reset"));
    if (!trg || !rst) throw std::runtime_error("plugin missing api");
    return {h, trg, rst};
}

void unload_plugin(AlertPlugin p) { if (p.handle) ::dlclose(p.handle); }

// ===== HTTP Server for Metrics (plain text) =====

class HttpServer final {
    int sock_{-1};
public:
    HttpServer(int port) {
        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) throw std::runtime_error("HTTP socket failed");
        int flag = 1; ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
        sockaddr_in addr{};
        addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
        if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("HTTP bind failed");
        if (::listen(sock_, 2) < 0) throw std::runtime_error("HTTP listen failed");
    }
    ~HttpServer() { if (sock_>=0) ::close(sock_); }
    void serve(const std::function<std::string()>& getMetrics) {
        for (;;) {
            int fd = ::accept(sock_, nullptr, nullptr);
            if (fd < 0) continue;
            char buf[256]{};
            ::read(fd, buf, sizeof(buf));
            // Always respond to /metrics
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n" + getMetrics();
            ::write(fd, response.data(), response.size());
            ::close(fd);
        }
    }
};

// ===== Admin Console (TCP, role-based) =====

struct User {
    std::string username;
    std::string password;
    Role        role;
};

class AdminConsole final {
    int sock_{-1};
    std::array<User, MAX_USERS> users_;
    LogBook& logs_;
public:
    AdminConsole(int port, LogBook& logs) : logs_(logs) {
        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) throw std::runtime_error("Admin socket failed");
        int flag = 1; ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
        sockaddr_in addr{};
        addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
        if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("Admin bind failed");
        if (::listen(sock_, 2) < 0) throw std::runtime_error("Admin listen failed");
        // Fixed users for demo
        users_[0] = {"admin", "adminpw", Role::Admin};
        users_[1] = {"user", "userpw", Role::User};
        users_[2] = {"viewer", "viewpw", Role::Viewer};
    }
    ~AdminConsole() { if (sock_ >= 0) ::close(sock_); }

    void serve() {
        char buf[256];
        std::array<int,2> last{0,0};
        for (;;) {
            int fd = ::accept(sock_, nullptr, nullptr);
            if (fd < 0) continue;
            ::write(fd, "Username: ", 10);
            int n = ::read(fd, buf, 255); buf[n < 0 ? 0 : n] = 0;
            std::string_view user{buf, size_t(n)};
            ::write(fd, "Password: ", 10);
            n = ::read(fd, buf, 255); buf[n < 0 ? 0 : n] = 0;
            std::string_view pw{buf, size_t(n)};
            auto it = std::ranges::find_if(users_, [&](const User& u) {
                return user.starts_with(u.username) && pw.starts_with(u.password);
            });
            Role role = (it != users_.end()) ? it->role : Role::Viewer;
            logs_.log(LogLevel::Info, role, "Login " + std::string(user));
            ::write(fd, "[Alarm Console Ready]\n", 22);
            if (role == Role::Admin) {
                ::write(fd, "Type logs for recent log entries\n", 33);
                n = ::read(fd, buf,255); buf[n<0?0:n]=0;
                if (std::string_view{buf}.starts_with("logs")) {
                    auto v = logs_.recent(10);
                    for (const auto& le : v)
                        ::dprintf(fd, "[%lu][%d] %s\n", le.timestamp, int(le.level), le.text);
                }
            }
            ::close(fd);
        }
    }
};

// ===== Core (Main) =====

int main()
{
    // Persistent logbook and state
    LogBook logBook{LOG_FILE, MAX_LOGS};
    logBook.log(LogLevel::Info, Role::Admin, "System boot");

    // State: armed/triggered, per-sensor
    struct State {
        bool armed[SENSOR_COUNT]{true,true,true};
        bool triggered[SENSOR_COUNT]{false,false,false};
        char pad[32];
    };
    MMapFile stateMap{STATE_FILE, sizeof(State)};
    State& systemState = *static_cast<State*>(stateMap.data());

    // Lock-free event queue and memory pool
    EventQueue q;
    MemPool<Event, POOL_SIZE> eventPool;

    // Dynamically loaded plugins
    std::array<AlertPlugin, MAX_PLUGINS> plugins{};
    int loaded = 0;
    try { plugins[loaded++] = load_plugin("libalert_plugin.so"); }
    catch (...) { logBook.log(LogLevel::Warn, Role::Admin, "Default plugin not loaded"); }

    // GPIO sensor simulation (replace paths with actual GPIO on device)
    std::array<GpioSensor, SENSOR_COUNT> sensors {{
        {open_gpio("/tmp/gpio_motion"), SensorType::Motion, 0, 0, 0},
        {open_gpio("/tmp/gpio_door"),   SensorType::Door,   0, 0, 0},
        {open_gpio("/tmp/gpio_smoke"),  SensorType::Smoke,  0, 0, 0},
    }};

    // Event loop thread (debounce, poll, process, log, plugin trigger)
    std::atomic<bool> running{true};
    std::thread eventThread([&]{
        struct pollfd fds[SENSOR_COUNT];
        for (int i=0; i<SENSOR_COUNT; ++i) {
            fds[i].fd = sensors[i].fd; fds[i].events = POLLIN;
        }
        uint64_t debounce_us = 20000;
        while (running.load(std::memory_order_relaxed)) {
            ::poll(fds, SENSOR_COUNT, 20);
            for (int i=0; i<SENSOR_COUNT; ++i) {
                char ch;
                if (::pread(fds[i].fd, &ch, 1, 0) == 1) {
                    int val = (ch == '1');
                    if (val == sensors[i].lastInput) {
                        if (++sensors[i].stableCount > 2) {
                            if (sensors[i].stableInput != val) {
                                sensors[i].stableInput = val;
                                Event* e = eventPool.allocate();
                                if (e) {
                                    e->sensor = sensors[i].type;
                                    e->type = val ? EventType::Trigger : EventType::Reset;
                                    e->timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::system_clock::now().time_since_epoch()).count();
                                    std::format_to(e->message, "sensor {} {}",
                                        int(sensors[i].type), val ? "TRIGGER":"RESET");
                                    q.push(*e);
                                }
                            }
                        }
                    } else {
                        sensors[i].stableCount = 0;
                    }
                    sensors[i].lastInput = val;
                }
            }
            // Process all events
            while (auto evt = q.pop()) {
                std::string_view msg(evt->message);
                logBook.log(LogLevel::Info, Role::User, msg);
                auto idx = int(evt->sensor);
                if (evt->type == EventType::Trigger) {
                    systemState.triggered[idx] = true;
                    for (int i=0; i<loaded; ++i) if (plugins[i].trigger)
                        plugins[i].trigger(evt->message);
                } else if (evt->type == EventType::Reset) {
                    systemState.triggered[idx] = false;
                    for (int i=0; i<loaded; ++i) if (plugins[i].reset)
                        plugins[i].reset();
                }
                eventPool.free(&(*evt));
            }
        }
    });

    // HTTP server for telemetry
    HttpServer metricsSrv{HTTP_PORT};
    std::thread httpThread([&] {
        metricsSrv.serve([&]{
            std::string m;
            for (int i=0; i<SENSOR_COUNT; ++i) {
                std::format_to(std::back_inserter(m), "sensor:{} triggered:{} armed:{}\n", i, systemState.triggered[i], systemState.armed[i]);
            }
            m += std::format("logsize:{}\n", logBook.recent(0).size());
            return m;
        });
    });
    // Admin console for role-based actions
    AdminConsole admin{ADMIN_PORT, logBook};
    std::thread adminThread([&]{ admin.serve(); });

    // Main: wait for signal to terminate
    std::signal(SIGINT, [](int){ std::exit(0); });
    eventThread.join();
    httpThread.detach();
    adminThread.detach();
    for (int i=0; i<loaded; ++i) unload_plugin(plugins[i]);
    logBook.log(LogLevel::Info, Role::Admin, "System halt");
}
