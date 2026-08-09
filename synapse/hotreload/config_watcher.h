// ============================================================================
// synapse/hotreload/config_watcher.h
// Project Synapse – Configuration Hot-Reload via File Watching
//
// Monitors a config file for changes and triggers callbacks.
// Validators run before any config is applied.
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace synapse::hotreload {

class ConfigWatcher {
public:
    using ConfigCallback = std::function<void(const std::string&)>;
    using Validator      = std::function<bool(const std::string&)>;

    explicit ConfigWatcher(const std::filesystem::path& config_path)
        : config_path_(config_path)
        , watching_(false)
        , last_modified_(0) {}

    ~ConfigWatcher() { stop(); }

    // Non-copyable, non-movable (owns a thread)
    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    // Start watching for config changes
    void start() {
        if (watching_.load()) return;
        watching_ = true;
        watcher_thread_ = std::thread([this]() { watch_loop(); });
    }

    // Stop watching
    void stop() {
        watching_ = false;
        if (watcher_thread_.joinable()) {
            watcher_thread_.join();
        }
    }

    // Register a callback for config changes
    void on_change(ConfigCallback cb) {
        callbacks_.push_back(std::move(cb));
    }

    // Add a validation callback
    void add_validator(Validator v) {
        validators_.push_back(std::move(v));
    }

    // Manual trigger: read, validate, notify
    void force_reload() {
        auto content = read_file();
        if (validate(content)) {
            notify_all(content);
        }
    }

    // Is the watcher running?
    bool is_watching() const { return watching_.load(); }

private:
    std::filesystem::path config_path_;
    std::atomic<bool> watching_;
    std::thread watcher_thread_;
    uint64_t last_modified_;
    std::vector<ConfigCallback> callbacks_;
    std::vector<Validator> validators_;

    void watch_loop() {
        // Capture initial modification time
        last_modified_ = get_mtime();

        while (watching_.load(std::memory_order_relaxed)) {
            auto current = get_mtime();
            if (current != last_modified_ && current != 0) {
                auto content = read_file();
                if (validate(content)) {
                    notify_all(content);
                    last_modified_ = current;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::string read_file() const {
        std::ifstream ifs(config_path_);
        if (!ifs) return {};
        return std::string((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
    }

    bool validate(const std::string& content) const {
        for (const auto& v : validators_) {
            if (!v(content)) return false;
        }
        return true;
    }

    void notify_all(const std::string& content) {
        for (const auto& cb : callbacks_) {
            cb(content);
        }
    }

    uint64_t get_mtime() const {
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(config_path_, ec);
        if (ec) return 0;
        auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
            ftime - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                sctp.time_since_epoch()).count());
    }
};

}  // namespace synapse::hotreload
