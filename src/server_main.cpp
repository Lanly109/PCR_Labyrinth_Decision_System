#include "server_runtime.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
std::atomic_flag interrupted;
void signal_stop(int) { interrupted.test_and_set(std::memory_order_relaxed); }

class SignalWatcher {
public:
    explicit SignalWatcher(labyrinth::ServerControl& control) : worker_([this, &control] {
        while (!done_.load()) {
            if (interrupted.test(std::memory_order_relaxed)) {
                control.stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }) {}
    ~SignalWatcher() {
        done_.store(true);
        worker_.join();
    }

private:
    std::atomic_bool done_{false};
    std::thread worker_;
};
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!n) return 1;
        std::string value(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), n, nullptr, nullptr);
        value.pop_back();
        args.push_back(std::move(value));
    }
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    try {
        labyrinth::ServerControl control;
        std::signal(SIGINT, signal_stop);
        std::signal(SIGTERM, signal_stop);
        const SignalWatcher watcher(control);
        return labyrinth::run_server(args, {}, &control);
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 1;
    }
}
