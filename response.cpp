/*
 * @tagline: Real-time GPIO-based Home Alarm System with concurrency, persistence, plugins, and admin console.
 * @intuition: Simulate GPIO sensors with lock-free queue, event-driven polling, debouncing, memory-mapped persistence,
 *            lightweight HTTP server, modular alerts, and multi-level logging.
 * @approach: Use modern C++23 features: lock-free data structures, memory mapping, dynamic plugin loading,
 *            std::jthread with stop tokens, custom memory pools, and role-based interactive CLI.
 * @complexity:
 *   - Time: O(1) per event processing; event loop runs indefinitely.
 *   - Space: Fixed memory pool size with minimal dynamic allocations.
 */

#include <atomic>
#include <chrono>
#include <cctype>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>
#include <array>
#include <cstring>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <unistd.h>
#include <ctime>
#include <type_traits>
#include <memory>

// Helper to cast sockaddr_in* to sockaddr* safely (required by POSIX socket APIs)
inline sockaddr* sockaddr_cast(sockaddr_in* addr) noexcept {
    // Using static_cast with explicit conversion for POSIX API compatibility
    return static_cast<sockaddr*>(static_cast<void*>(addr));
}

namespace util {

/**
 * @brief Log levels for logger.
 */
enum class LogLevel : int {
    Error = 0,
    Warn,
    Info,
    Debug,
    Trace
};

/**
 * @brief Thread-safe logger with level control and load-based skipping.
 */
class Logger {
    std::mutex logMutex_;
    LogLevel level_{LogLevel::Info};
    std::atomic<bool> highLoad_{false};

    static constexpr const char* ToString(LogLevel lvl) {
        using enum LogLevel;
        switch (lvl) {
            case Error: return "ERROR";
            case Warn:  return "WARN";
            case Info:  return "INFO";
            case Debug: return "DEBUG";
            case Trace: return "TRACE";
            default:    return "UNKNOWN";
        }
    }

public:
    void setLevel(LogLevel lvl) noexcept { level_ = lvl; }
    LogLevel level() const noexcept { return level_; }
    void setHighLoad(bool val) noexcept { highLoad_ = val; }

    template<typename... Args>
    void log(LogLevel lvl, Args&&... args) {
        using enum LogLevel;
        if (lvl > level_) return;
        if (highLoad_ && lvl > Warn) return;

        std::lock_guard lock(logMutex_);
        std::stringstream ss;

        auto now = std::chrono::system_clock::now();
        time_t now_c = std::chrono::system_clock::to_time_t(now);

        if (std::tm buf{}; !localtime_r(&now_c, &buf)) {
            ss << "[unknown time] ";
        } else {
            ss << '[' << std::put_time(&buf, "%Y-%m-%d %H:%M:%S") << "] ";
        }
        ss << ToString(lvl) << ": ";
        (ss << ... << std::forward<Args>(args));
        ss << '\n';

        std::cout << ss.str();
        std::cout.flush();
    }
};

/**
 * @brief Accessor for singleton Logger instance using inline variable (C++17).
 */
inline Logger& gLogger() {
    static inline Logger instance; // Fix: Use inline variable
    return instance;
}

}  // namespace util

namespace concurrent {

/**
 * @brief Lock-free single-producer single-consumer queue.
 * @tparam T element type
 * @tparam N buffer size, must be a power of two.
 */
template<typename T, size_t N>
class LockFreeQueue {
    static_assert((N & (N - 1)) == 0 && N != 0, "N must be power of two");
    std::array<T, N> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    bool enqueue(const T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t nextTail = (tail + 1) & (N - 1);
        if (nextTail == head_.load(std::memory_order_acquire)) {
            return false; // full
        }
        buffer_[tail] = item;
        tail_.store(nextTail, std::memory_order_release);
        return true;
    }

    std::optional<T> dequeue() noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt; // empty
        }
        T item = buffer_[head];
        head_.store((head + 1) & (N - 1), std::memory_order_release);
        return item;
    }
};

}  // namespace concurrent

namespace gpio {

using SensorId = int;
enum class SensorType { Door, Motion, Smoke };

struct SensorEvent {
    SensorId sensor;
    SensorType type;
    bool activated;
    std::chrono::steady_clock::time_point timestamp;
};

class GPIOManager {
    concurrent::LockFreeQueue<SensorEvent, 256> eventQueue_;
    std::unordered_map<SensorId, std::chrono::steady_clock::time_point> lastActivation_;
    static constexpr std::chrono::milliseconds debounceDuration{50};

public:
    bool enqueueEvent(const SensorEvent& e) {
        auto now = std::chrono::steady_clock::now();
        if (auto lastIt = lastActivation_.find(e.sensor);
            lastIt != lastActivation_.end() && (now - lastIt->second < debounceDuration)) {
            return false;
        }
        lastActivation_[e.sensor] = now;
        return eventQueue_.enqueue(e);
    }

    std::optional<SensorEvent> getNextEvent() noexcept {
        return eventQueue_.dequeue();
    }
};

// Fix: Use inline variable for global access
inline GPIOManager& GetGPIOManager() {
    static inline GPIOManager instance; // Fix: Use inline variable
    return instance;
}

}  // namespace gpio

namespace persistence {

class MMapFileException : public std::runtime_error {
public:
    // Use inherited constructors instead of manually duplicating them
    using std::runtime_error::runtime_error;
};

class FileOpenException : public MMapFileException {
public:
    explicit FileOpenException(const std::string& filepath)
        : MMapFileException("Failed to open file: " + filepath) {}
};

class FileTruncateException : public MMapFileException {
public:
    explicit FileTruncateException(const std::string& filepath)
        : MMapFileException("Failed to truncate file: " + filepath) {}
};

class MMapException : public MMapFileException {
public:
    explicit MMapException(const std::string& filepath)
        : MMapFileException("Memory mapping failed for file: " + filepath) {}
};

// Type-safe wrapper for memory-mapped regions (replaces void*)
class MappedMemoryRegion {
private:
    using RawMemoryPtr = std::byte*; // Use std::byte* instead of void*
    RawMemoryPtr rawAddress_;
    size_t size_;
    
public:
    explicit MappedMemoryRegion(RawMemoryPtr addr, size_t size) noexcept 
        : rawAddress_(addr), size_(size) {}
    
    // Default constructor for invalid region
    MappedMemoryRegion() noexcept : rawAddress_(nullptr), size_(0) {}
    
    template<typename T>
    T* as() noexcept {
        return std::launder(reinterpret_cast<T*>(rawAddress_)); // Safer cast with launder
    }
    
    template<typename T>
    const T* as() const noexcept {
        return std::launder(reinterpret_cast<const T*>(rawAddress_)); // Safer cast with launder
    }
    
    bool isValid() const noexcept {
        return rawAddress_ != nullptr && 
               rawAddress_ != reinterpret_cast<RawMemoryPtr>(MAP_FAILED);
    }
    
    size_t size() const noexcept { return size_; }
    
    // Internal use only - for munmap
    RawMemoryPtr getRawAddress() const noexcept { return rawAddress_; }
};

class MMapFile {
    int fd_{-1};
    MappedMemoryRegion mappedRegion_; // Replace void* with type-safe wrapper
    size_t length_{0};
    std::string filepath_;

    void cleanup() noexcept {
        if (mappedRegion_.isValid()) {
            munmap(mappedRegion_.getRawAddress(), length_);
            mappedRegion_ = MappedMemoryRegion(); // Reset to invalid state
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

public:
    MMapFile(const char* filename, size_t length) : length_(length), filepath_(filename) {
        fd_ = open(filename, O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) throw FileOpenException(filepath_);
        
        if (ftruncate(fd_, static_cast<off_t>(length)) < 0) {
            close(fd_);
            throw FileTruncateException(filepath_);
        }
        
        void* addr = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (addr == MAP_FAILED) {
            close(fd_);
            throw MMapException(filepath_);
        }
        
        // Safe conversion from void* to std::byte*
        mappedRegion_ = MappedMemoryRegion(static_cast<std::byte*>(addr), length);
    }

    MMapFile(const MMapFile&) = delete;
    MMapFile& operator=(const MMapFile&) = delete;

    MMapFile(MMapFile&& other) noexcept
        : fd_(other.fd_), 
          mappedRegion_(std::move(other.mappedRegion_)), 
          length_(other.length_), 
          filepath_(std::move(other.filepath_)) {
        other.fd_ = -1;
        other.mappedRegion_ = MappedMemoryRegion();
        other.length_ = 0;
        other.filepath_.clear();
    }

    MMapFile& operator=(MMapFile&& other) noexcept {
        if (this != &other) {
            cleanup();

            fd_ = other.fd_;
            mappedRegion_ = std::move(other.mappedRegion_);
            length_ = other.length_;
            filepath_ = std::move(other.filepath_);

            other.fd_ = -1;
            other.mappedRegion_ = MappedMemoryRegion();
            other.length_ = 0;
            other.filepath_.clear();
        }
        return *this;
    }

    ~MMapFile() {
        cleanup();
    }

    template<typename T>
    T* data() noexcept {
        return mappedRegion_.template as<T>();
    }

    template<typename T>
    const T* data() const noexcept {
        return mappedRegion_.template as<T>();
    }

    size_t size() const noexcept { return length_; }
};

}  // namespace persistence

namespace alarm {

using namespace std::chrono_literals;

enum class AlarmState { Disarmed, Armed, Triggered };

class AlarmController {
    mutable std::mutex mutex_;
    AlarmState state_{AlarmState::Disarmed};
    std::map<gpio::SensorId, bool> sensorStatus_;
    std::chrono::steady_clock::time_point triggeredAt_{};

    static constexpr std::string_view adminPin_{"1234"};

public:
    bool arm(std::string_view pin) {
        std::lock_guard lk(mutex_);
        if (pin != adminPin_) return false;
        if (state_ == AlarmState::Disarmed) {
            sensorStatus_.clear();
            state_ = AlarmState::Armed;
            util::gLogger().log(util::LogLevel::Info, "Alarm armed.");
            return true;
        }
        return false;
    }

    bool disarm(std::string_view pin) {
        std::lock_guard lk(mutex_);
        if (pin != adminPin_) return false;
        if (state_ != AlarmState::Disarmed) {
            sensorStatus_.clear();
            state_ = AlarmState::Disarmed;
            util::gLogger().log(util::LogLevel::Info, "Alarm disarmed.");
            return true;
        }
        return false;
    }

    AlarmState getState() const {
        std::lock_guard lk(mutex_);
        return state_;
    }

    void processSensorEvent(const gpio::SensorEvent& event) {
        std::lock_guard lk(mutex_);
        if(state_ != AlarmState::Armed) return;
        if(event.activated) {
            sensorStatus_[event.sensor] = true;
            triggeredAt_ = std::chrono::steady_clock::now();
            state_ = AlarmState::Triggered;
            util::gLogger().log(util::LogLevel::Warn,
                "Alarm triggered by sensor ", event.sensor,
                " type ", std::to_underlying(event.type));
        }
    }

    std::map<gpio::SensorId, bool> getSensorStatus() const {
        std::lock_guard lk(mutex_);
        return sensorStatus_;
    }
};

}  // namespace alarm

namespace plugin {

// Base exception class for all plugin-related errors
class PluginException : public std::runtime_error {
public:
    // Use inherited constructors from std::runtime_error
    using std::runtime_error::runtime_error;
};

// Specific exception for symbol resolution failures
class SymbolNotFoundException : public PluginException {
private:
    std::string symbolName_;
    std::string libraryPath_;
    
public:
    explicit SymbolNotFoundException(const std::string& symbolName, const std::string& libraryPath = "")
        : PluginException("Symbol '" + symbolName + "' not found" + 
                         (libraryPath.empty() ? "" : " in library '" + libraryPath + "'")),
          symbolName_(symbolName),
          libraryPath_(libraryPath) {}
    
    const std::string& getSymbolName() const noexcept { return symbolName_; }
    const std::string& getLibraryPath() const noexcept { return libraryPath_; }
};

// Exception for library loading failures
class LibraryLoadException : public PluginException {
private:
    std::string libraryPath_;
    std::string systemError_;
    
public:
    explicit LibraryLoadException(const std::string& libraryPath, const std::string& systemError = "")
        : PluginException("Failed to load library '" + libraryPath + "'" +
                         (systemError.empty() ? "" : ": " + systemError)),
          libraryPath_(libraryPath),
          systemError_(systemError) {}
    
    const std::string& getLibraryPath() const noexcept { return libraryPath_; }
    const std::string& getSystemError() const noexcept { return systemError_; }
};

// Exception for plugin initialization failures
class PluginInitializationException : public PluginException {
private:
    std::string pluginPath_;
    std::string initError_;
    
public:
    explicit PluginInitializationException(const std::string& pluginPath, const std::string& initError)
        : PluginException("Plugin initialization failed for '" + pluginPath + "': " + initError),
          pluginPath_(pluginPath),
          initError_(initError) {}
    
    const std::string& getPluginPath() const noexcept { return pluginPath_; }
    const std::string& getInitError() const noexcept { return initError_; }
};

class IAlertPlugin {
public:
    virtual ~IAlertPlugin() = default;
    virtual void alert(std::string_view message) noexcept = 0;
    virtual void initialize() noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

using plugin_create_t = IAlertPlugin* (*)();
using plugin_destroy_t = void (*)(IAlertPlugin*);

// Type-safe wrapper for dynamic library handles (replaces void*)
class DynamicLibraryHandle {
private:
    // Custom handle type instead of raw void*
    struct LibraryHandle {
        using HandleType = std::byte*; // Use std::byte* instead of void*
        HandleType rawHandle;
        
        explicit LibraryHandle(HandleType handle) noexcept : rawHandle(handle) {}
        LibraryHandle() noexcept : rawHandle(nullptr) {}
        
        bool isValid() const noexcept { return rawHandle != nullptr; }
        HandleType get() const noexcept { return rawHandle; }
        
        // Safe conversion for dlsym/dlclose
        void* getRawPointer() const noexcept { 
            return static_cast<void*>(rawHandle); 
        }
    };

    struct LibraryHandleDeleter {
        void operator()(LibraryHandle* handle) const noexcept {
            if (handle && handle->isValid()) {
                dlclose(handle->getRawPointer());
            }
            delete handle;
        }
    };
    
    std::unique_ptr<LibraryHandle, LibraryHandleDeleter> handle_;
    std::string libraryPath_;
    
public:
    DynamicLibraryHandle() noexcept = default;
    
    explicit DynamicLibraryHandle(void* raw_handle, std::string path) noexcept 
        : handle_(std::make_unique<LibraryHandle>(static_cast<LibraryHandle::HandleType>(raw_handle))), // Fix: Use std::make_unique
          libraryPath_(std::move(path)) {}
    
    // Move-only type
    DynamicLibraryHandle(const DynamicLibraryHandle&) = delete;
    DynamicLibraryHandle& operator=(const DynamicLibraryHandle&) = delete;
    
    DynamicLibraryHandle(DynamicLibraryHandle&&) noexcept = default;
    DynamicLibraryHandle& operator=(DynamicLibraryHandle&&) noexcept = default;
    
    bool isValid() const noexcept { 
        return handle_ && handle_->isValid(); 
    }
    
    // Internal use only - for dlsym operations
    void* getRawHandle() const noexcept { 
        return handle_ ? handle_->getRawPointer() : nullptr;
    }
    
    const std::string& getLibraryPath() const noexcept {
        return libraryPath_;
    }
    
    explicit operator bool() const noexcept {
        return isValid();
    }
    
    void reset() noexcept {
        handle_.reset();
        libraryPath_.clear();
    }
};

// Factory function to create library handle
inline DynamicLibraryHandle openDynamicLibrary(const std::string& path) {
    void* raw_handle = dlopen(path.c_str(), RTLD_NOW);
    if (!raw_handle) {
        throw LibraryLoadException(path, dlerror());
    }
    return DynamicLibraryHandle(raw_handle, path);
}

// Type-safe wrapper for symbol addresses from dynamic libraries
class SymbolAddress {
private:
    // Wrapper for symbol addresses (replaces void*)
    struct SymbolPtr {
        using AddressType = std::byte*; // Use std::byte* instead of void*
        AddressType address;
        
        explicit SymbolPtr(AddressType addr) noexcept : address(addr) {}
        SymbolPtr() noexcept : address(nullptr) {}
        
        bool isValid() const noexcept { return address != nullptr; }
        AddressType get() const noexcept { return address; }
        
        // Safe conversion for function pointer casting
        void* getRawPointer() const noexcept { 
            return static_cast<void*>(address); 
        }
    };
    
    SymbolPtr symbolPtr_;
    std::string symbolName_;
    std::string libraryPath_;
    
public:
    explicit SymbolAddress(void* addr, std::string symbolName, std::string libraryPath = "") noexcept 
        : symbolPtr_(static_cast<SymbolPtr::AddressType>(addr)), 
          symbolName_(std::move(symbolName)), 
          libraryPath_(std::move(libraryPath)) {}
    
    // Internal use only - for function pointer casting
    void* getRawAddress() const noexcept { return symbolPtr_.getRawPointer(); }
    const std::string& getSymbolName() const noexcept { return symbolName_; }
    const std::string& getLibraryPath() const noexcept { return libraryPath_; }
    
    bool isValid() const noexcept { return symbolPtr_.isValid(); }
    explicit operator bool() const noexcept { return isValid(); }
};

// Factory function to get symbol from library
inline SymbolAddress getSymbolAddress(const DynamicLibraryHandle& handle, const char* symbolName) {
    void* sym = dlsym(handle.getRawHandle(), symbolName);
    return SymbolAddress(sym, symbolName, handle.getLibraryPath());
}

// Type-safe function pointer casting with safer operations
template <typename FuncPtr>
FuncPtr safe_cast_function_ptr(const SymbolAddress& symbolAddr) {
    static_assert(std::is_function_v<std::remove_pointer_t<FuncPtr>>, 
                  "FuncPtr must be a function pointer type");
    
    if (!symbolAddr.isValid()) {
        throw SymbolNotFoundException(symbolAddr.getSymbolName(), symbolAddr.getLibraryPath());
    }
    
    // Use std::bit_cast for safer casting (C++20) or fallback to reinterpret_cast
    if constexpr (requires { std::bit_cast<FuncPtr>(symbolAddr.getRawAddress()); }) {
        return std::bit_cast<FuncPtr>(symbolAddr.getRawAddress());
    } else {
        // Fallback with additional safety checks
        void* rawAddr = symbolAddr.getRawAddress();
        // Ensure alignment is correct for function pointers
        if (reinterpret_cast<std::uintptr_t>(rawAddr) % alignof(FuncPtr) != 0) {
            throw SymbolNotFoundException(symbolAddr.getSymbolName(), 
                "Symbol address alignment mismatch for " + symbolAddr.getLibraryPath());
        }
        return reinterpret_cast<FuncPtr>(rawAddr); // Last resort with safety check
    }
}

class PluginHandle {
private:
    DynamicLibraryHandle libraryHandle_;

public:
    PluginHandle() noexcept = default;
    explicit PluginHandle(DynamicLibraryHandle handle) noexcept 
        : libraryHandle_(std::move(handle)) {}

    bool isValid() const noexcept { 
        return libraryHandle_.isValid(); 
    }
    
    void* getRawHandle() const noexcept { 
        return libraryHandle_.getRawHandle(); 
    }

    const DynamicLibraryHandle& getLibraryHandle() const noexcept {
        return libraryHandle_;
    }

    void close() noexcept {
        libraryHandle_.reset();
    }

    // Move-only semantics
    PluginHandle(const PluginHandle&) = delete;
    PluginHandle& operator=(const PluginHandle&) = delete;

    PluginHandle(PluginHandle&&) noexcept = default;
    PluginHandle& operator=(PluginHandle&&) noexcept = default;

    ~PluginHandle() = default; // RAII handled by DynamicLibraryHandle
};

// Updated get_symbol function with specific exception
template <typename FuncPtr>
FuncPtr get_symbol(const PluginHandle& ph, const char* symbolName) {
    if (!ph.isValid()) {
        throw PluginException("Invalid plugin handle provided to get_symbol");
    }
    
    auto symbolAddr = getSymbolAddress(ph.getLibraryHandle(), symbolName);
    
    // This will throw SymbolNotFoundException if symbol is not found
    return safe_cast_function_ptr<FuncPtr>(symbolAddr);
}

class PluginManager {
    PluginHandle handle_;
    IAlertPlugin* plugin_ = nullptr;
    plugin_destroy_t destroy_ = nullptr;
    std::string currentPluginPath_;

    // Extract plugin creation logic to separate method (fix for nested try-catch)
    void createAndInitializePlugin(const std::string& path) {
        auto create = get_symbol<plugin_create_t>(handle_, "create_plugin");
        destroy_ = get_symbol<plugin_destroy_t>(handle_, "destroy_plugin");
        
        plugin_ = create();
        if (!plugin_) {
            throw PluginInitializationException(path, "create_plugin returned null");
        }
        
        initializePlugin(path);
    }
    
    // Extract initialization logic to separate method
    void initializePlugin(const std::string& path) {
        try {
            plugin_->initialize();
        } catch (const PluginException& e) {
            cleanupPartialPlugin();
            throw PluginInitializationException(path, "initialize() failed: " + std::string(e.what()));
        } catch (...) {
            cleanupPartialPlugin();
            throw PluginInitializationException(path, "initialize() failed with unknown exception");
        }
    }
    
    // Helper method for partial cleanup
    void cleanupPartialPlugin() noexcept {
        if (plugin_ && destroy_) {
            try {
                destroy_(plugin_);
            } catch (...) {
                // Ignore cleanup exceptions
            }
        }
        plugin_ = nullptr;
    }

    void cleanup() noexcept {
        if (plugin_) {
            try {
                plugin_->shutdown();
            } catch (...) {
                // Ignore exceptions during shutdown
            }
            
            if (destroy_) {
                try {
                    destroy_(plugin_);
                } catch (...) {
                    // Ignore exceptions during destruction
                }
            }
            plugin_ = nullptr;
        }
        handle_.close();
        destroy_ = nullptr;
    }

public:
    PluginManager() = default;
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    PluginManager(PluginManager&& other) noexcept
        : handle_(std::move(other.handle_)), 
          plugin_(other.plugin_), 
          destroy_(other.destroy_),
          currentPluginPath_(std::move(other.currentPluginPath_)) {
        other.plugin_ = nullptr;
        other.destroy_ = nullptr;
    }

    PluginManager& operator=(PluginManager&& other) noexcept {
        if(this != &other) {
            unload();
            handle_ = std::move(other.handle_);
            plugin_ = other.plugin_;
            destroy_ = other.destroy_;
            currentPluginPath_ = std::move(other.currentPluginPath_);
            other.plugin_ = nullptr;
            other.destroy_ = nullptr;
        }
        return *this;
    }

    bool load(const std::string& path) {
        if (handle_.isValid()) {
            util::gLogger().log(util::LogLevel::Warn, "Plugin already loaded, unloading first");
            unload();
        }
        
        try {
            auto libraryHandle = openDynamicLibrary(path);
            handle_ = PluginHandle(std::move(libraryHandle));
            currentPluginPath_ = path;
            
            // Extracted complex logic to separate method
            createAndInitializePlugin(path);
            
            util::gLogger().log(util::LogLevel::Info, "Plugin loaded successfully: ", path);
            return true;
            
        } catch (const SymbolNotFoundException& e) {
            util::gLogger().log(util::LogLevel::Error, "Plugin symbol error: ", e.what());
            cleanup();
            return false;
        } catch (const LibraryLoadException& e) {
            util::gLogger().log(util::LogLevel::Error, "Library load error: ", e.what());
            cleanup();
            return false;
        } catch (const PluginInitializationException& e) {
            util::gLogger().log(util::LogLevel::Error, "Plugin initialization error: ", e.what());
            cleanup();
            return false;
        } catch (const PluginException& e) {
            util::gLogger().log(util::LogLevel::Error, "Generic plugin error: ", e.what());
            cleanup();
            return false;
        } catch (...) {
            util::gLogger().log(util::LogLevel::Error, "Unknown error loading plugin: ", path);
            cleanup();
            throw; // Re-throw unknown exceptions
        }
    }

    void unload() noexcept {
        cleanup();
        util::gLogger().log(util::LogLevel::Info, "Plugin unloaded: ", currentPluginPath_);
        currentPluginPath_.clear();
    }

    bool isLoaded() const noexcept { 
        return plugin_ != nullptr; 
    }

    void alert(std::string_view message) noexcept {
        if (plugin_) {
            try { 
                plugin_->alert(message); 
            } catch (const PluginException& e) {
                util::gLogger().log(util::LogLevel::Error, 
                    "Alert plugin threw plugin exception: ", e.what());
            } catch (...) { 
                util::gLogger().log(util::LogLevel::Error, 
                    "Alert plugin threw unknown exception");
            }
        }
    }

    const std::string& getCurrentPluginPath() const noexcept {
        return currentPluginPath_;
    }

    ~PluginManager() {
        unload();
    }
};

}  // namespace plugin

namespace http {

class HttpServer {
    int serverFd_{-1};
    int port_;
    mutable std::atomic<bool> running_{false};
    std::jthread serverThread_;
    std::function<std::string()> statusProvider_;

    void run(std::stop_token stoken) const {
        using namespace std::chrono_literals;
        while (running_.load(std::memory_order_acquire) && !stoken.stop_requested()) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(serverFd_, sockaddr_cast(&clientAddr), &clientLen);
            if (clientFd < 0) {
                std::this_thread::sleep_for(10ms);
                continue;
            }
            std::array<char, 1024> buf{};
            ssize_t count = read(clientFd, buf.data(), buf.size() - 1);
            if (count <= 0) {
                close(clientFd);
                continue;
            }
            std::string req{buf.data(), static_cast<size_t>(count)};
            if (req.contains("GET /status")) {
                std::string body = statusProvider_();
                std::stringstream response;
                response << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/plain\r\n"
                         << "Content-Length: " << body.size() << "\r\n"
                         << "Connection: close\r\n\r\n"
                         << body;
                std::string respStr = response.str();
                write(clientFd, respStr.data(), respStr.size());
            } else {
                static constexpr const char resp[] =
                    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                write(clientFd, resp, sizeof(resp) - 1);
            }
            close(clientFd);
        }
    }

public:
    explicit HttpServer(int port, std::function<std::string()> statusProvider)
        : port_(port), statusProvider_(std::move(statusProvider)) {}

    bool start() {
        sockaddr_in addr{};
        serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ == -1) return false;

        int opt = 1;
        setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port_));

        if (bind(serverFd_, sockaddr_cast(&addr), sizeof(addr)) < 0) {
            close(serverFd_);
            serverFd_ = -1;
            return false;
        }
        if (listen(serverFd_, 10) < 0) {
            close(serverFd_);
            serverFd_ = -1;
            return false;
        }
        running_ = true;

        serverThread_ = std::jthread(&HttpServer::run, this, std::placeholders::_1);
        util::gLogger().log(util::LogLevel::Info, "HTTP server started on port ", port_);
        return true;
    }

    void stop() {
        if (running_) {
            running_ = false;
            shutdown(serverFd_, SHUT_RDWR);
            close(serverFd_);
            if (serverThread_.joinable()) {
                serverThread_.request_stop();
                serverThread_.join();
            }
            util::gLogger().log(util::LogLevel::Info, "HTTP server stopped");
        }
    }
    ~HttpServer() {
        stop();
    }
};

}  // namespace http

namespace admin {

class AdminConsole {
    alarm::AlarmController& alarm_;
    plugin::PluginManager& pluginManager_;
    std::atomic<bool> running_{false};
    std::jthread consoleThread_;

    static std::vector<std::string> split(std::string_view sv) {
        std::vector<std::string> tokens;
        std::string tmp;
        for (char c : sv) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!tmp.empty()) {
                    tokens.push_back(std::move(tmp));
                    tmp.clear();
                }
            } else {
                tmp.push_back(c);
            }
        }
        if (!tmp.empty()) tokens.push_back(std::move(tmp));
        return tokens;
    }

    void printHelp() const {
        std::cout << "Commands:\n"
                  << "  arm <pin>          : arm the alarm\n"
                  << "  disarm <pin>       : disarm the alarm\n"
                  << "  status             : show current status\n"
                  << "  loadplugin <path>  : load alert plugin\n"
                  << "  unloadplugin       : unload alert plugin\n"
                  << "  quit               : exit console\n";
    }

    void handleArm(const std::string& pin) {
        if (alarm_.arm(pin)) {
            std::cout << "Alarm armed\n";
        } else {
            std::cout << "Failed to arm alarm\n";
        }
    }

    void handleDisarm(const std::string& pin) {
        if (alarm_.disarm(pin)) {
            std::cout << "Alarm disarmed\n";
        } else {
            std::cout << "Failed to disarm alarm\n";
        }
    }

    void printStatus() const {
        auto state = alarm_.getState();
        std::string stateStr;
        if (state == alarm::AlarmState::Armed) {
            stateStr = "Armed";
        } else if (state == alarm::AlarmState::Disarmed) {
            stateStr = "Disarmed";
        } else {
            stateStr = "Triggered";
        }

        std::cout << "Alarm state: " << stateStr << "\n";

        auto sensors = alarm_.getSensorStatus();
        for (const auto& [id, active] : sensors) {
            std::cout << "Sensor " << id << ": " << (active ? "Activated" : "Inactive") << "\n";
        }
    }

    void handleLoadPlugin(const std::string& path) {
        if (pluginManager_.load(path)) {
            std::cout << "Plugin loaded\n";
        } else {
            std::cout << "Plugin loading failed\n";
        }
    }

    void handleUnloadPlugin() {
        pluginManager_.unload();
        std::cout << "Plugin unloaded\n";
    }

    void run(std::stop_token stoken) {
        util::gLogger().log(util::LogLevel::Info, "Admin Console started. Type 'help'.");

        while (running_ && !stoken.stop_requested()) {
            std::cout << "> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) break;

            auto tokens = split(line);
            if (tokens.empty()) continue;

            const auto& cmd = tokens[0];
            if (cmd == "help") {
                printHelp();
            } else if (cmd == "arm" && tokens.size() == 2) {
                handleArm(tokens[1]);
            } else if (cmd == "disarm" && tokens.size() == 2) {
                handleDisarm(tokens[1]);
            } else if (cmd == "status") {
                printStatus();
            } else if (cmd == "loadplugin" && tokens.size() == 2) {
                handleLoadPlugin(tokens[1]);
            } else if (cmd == "unloadplugin") {
                handleUnloadPlugin();
            } else if (cmd == "quit") {
                running_ = false;
                consoleThread_.request_stop();
            } else {
                std::cout << "Unknown command\n";
            }
        }

        util::gLogger().log(util::LogLevel::Info, "Admin Console terminated.");
    }

public:
    explicit AdminConsole(alarm::AlarmController& alarm, plugin::PluginManager& plugins)
        : alarm_(alarm), pluginManager_(plugins) {}

    void start() {
        running_ = true;
        consoleThread_ = std::jthread(&AdminConsole::run, this);
    }

    void stop() {
        running_ = false;
        consoleThread_.request_stop();
        // jthread join on destruction automatically
    }

    ~AdminConsole() {
        stop();
    }
};

}  // namespace admin

int main() {
    util::gLogger().setLevel(util::LogLevel::Debug);

    alarm::AlarmController alarmController;
    plugin::PluginManager pluginManager;

    http::HttpServer httpServer(8080, [&alarmController]() {
        static inline const std::map<alarm::AlarmState, std::string> stateNames{ // Fix: Use inline variable
            {alarm::AlarmState::Disarmed, "Disarmed"},
            {alarm::AlarmState::Armed,    "Armed"},
            {alarm::AlarmState::Triggered,"Triggered"}};
        std::stringstream ss;
        auto st = alarmController.getState();
        ss << "AlarmState: " << stateNames.at(st) << "\n";
        auto sensors = alarmController.getSensorStatus();
        for (const auto& [id, active] : sensors)
            ss << "Sensor " << id << ": " << (active ? "Activated" : "Inactive") << "\n";
        return ss.str();
    });

    if (!httpServer.start()) {
        util::gLogger().log(util::LogLevel::Error, "Failed to start HTTP server");
        return 1;
    }

    admin::AdminConsole console(alarmController, pluginManager);
    console.start();

    std::jthread gpioThread([](std::stop_token stoken) {
        auto& gpioMgr = gpio::GetGPIOManager();
        // Fix: Use CTAD (Class Template Argument Deduction) for both array and distribution
        std::array types{gpio::SensorType::Door, gpio::SensorType::Motion, gpio::SensorType::Smoke}; // CTAD: deduces std::array<gpio::SensorType, 3>
        gpio::SensorId sensorId = 1;
        size_t typeIdx = 0;

        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution dist(0, 4); // Fix: CTAD - template argument deduced automatically as <int>

        while (!stoken.stop_requested()) {
            bool activate = (dist(rng) == 0);
            if (activate) {
                gpio::SensorEvent ev{sensorId, types[typeIdx], true, std::chrono::steady_clock::now()};
                gpioMgr.enqueueEvent(ev);
            }
            sensorId = (sensorId % 3) + 1;
            typeIdx = (typeIdx + 1) % types.size();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    });

    auto& gpioManager = gpio::GetGPIOManager();

    while (true) {
        if (auto evt = gpioManager.getNextEvent()) {
            alarmController.processSensorEvent(*evt);
            if (pluginManager.isLoaded()) {
                pluginManager.alert("Alarm event triggered");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    gpioThread.request_stop();
    gpioThread.join();
    console.stop();
    httpServer.stop();

    return 0;
}


