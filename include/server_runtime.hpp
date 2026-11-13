#pragma once

#include "labyrinth.hpp"
#include <atomic>
#include <functional>
#include <ios>
#include <memory>
#include <mutex>

namespace httplib { class Server; }

namespace labyrinth {

struct ServerRunOptions {
    std::filesystem::path root, config_path, decision_log_path;
    // Byte position after the flushed JSONL record, or zero if it was not saved.
    std::function<void(const Json&, std::size_t, std::streamoff)> decision_log;
    std::function<void(std::string)> notice;
};

struct ServerState;

// One control per listener. stop() is safe before the listener starts and is idempotent.
class ServerControl {
public:
    std::atomic_bool enabled{true};

    bool ready() const { return ready_.load(); }
    std::string backend() const;
    void stop();
    // Prepare off-thread, persist only after validation, then publish to new requests.
    void reload(const std::filesystem::path& root, Config config, Catalog catalog,
                const std::function<void()>& persist);

private:
    friend int run_server(const std::vector<std::string>&, const ServerRunOptions&, ServerControl*);
    mutable std::mutex mutex_;
    httplib::Server* server_ = nullptr;
    std::atomic_bool ready_{false};
    bool stopping_ = false;
    std::shared_ptr<const ServerState> snapshot() const;
    std::shared_ptr<const ServerState> state_;
};

std::filesystem::path executable_dir(const char* argv0 = nullptr);
int run_server(const std::vector<std::string>& args, const ServerRunOptions& options = {},
               ServerControl* control = nullptr);
}
