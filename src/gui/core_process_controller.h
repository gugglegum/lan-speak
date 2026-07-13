#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "gui/telemetry_protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace lanspeak::gui {

struct CoreOutputChunk {
    std::uint64_t generation = 0;
    std::string bytes;
};

struct CoreExitNotice {
    std::uint64_t generation = 0;
    DWORD exit_code = 0;
};

class CoreProcessController {
public:
    using CommandBuilder = std::function<std::wstring(HANDLE telemetry_write, HANDLE control_read)>;

    CoreProcessController() = default;
    ~CoreProcessController();
    CoreProcessController(const CoreProcessController&) = delete;
    CoreProcessController& operator=(const CoreProcessController&) = delete;

    bool start(
        const std::wstring& working_directory,
        const CommandBuilder& command_builder,
        HWND notify_window,
        UINT output_message,
        UINT telemetry_message,
        UINT exit_message,
        DWORD& error);

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint64_t generation() const;
    bool write_control(std::string_view line);
    void request_stop(DWORD graceful_timeout_ms = 750);
    void shutdown_and_wait(DWORD graceful_timeout_ms = 1000);
    bool finalize(std::uint64_t generation);
    bool take_latest_telemetry(std::uint64_t generation, TelemetrySnapshot& snapshot);

private:
    void read_output(HANDLE pipe, std::uint64_t generation);
    void read_telemetry(HANDLE pipe, std::uint64_t generation);
    void wait_for_exit(HANDLE process, std::uint64_t generation);
    void close_handles();
    void join_workers();

    mutable std::mutex state_mutex_;
    PROCESS_INFORMATION process_{};
    HANDLE stdout_read_ = nullptr;
    HANDLE telemetry_read_ = nullptr;
    HANDLE control_write_ = nullptr;
    HWND notify_window_ = nullptr;
    UINT output_message_ = 0;
    UINT telemetry_message_ = 0;
    UINT exit_message_ = 0;
    std::uint64_t generation_ = 0;
    std::atomic_bool running_{false};
    std::atomic_bool stop_requested_{false};

    std::thread output_thread_;
    std::thread telemetry_thread_;
    std::thread wait_thread_;
    std::thread fallback_thread_;

    std::mutex telemetry_mutex_;
    TelemetryParser telemetry_parser_;
    bool telemetry_notification_pending_ = false;
};

} // namespace lanspeak::gui
