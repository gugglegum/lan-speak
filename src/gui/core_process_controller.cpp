#include "gui/core_process_controller.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace lanspeak::gui {
namespace {

void close_if_valid(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

} // namespace

CoreProcessController::~CoreProcessController() {
    shutdown_and_wait();
}

bool CoreProcessController::start(
    const std::wstring& working_directory,
    const CommandBuilder& command_builder,
    HWND notify_window,
    UINT output_message,
    UINT telemetry_message,
    UINT exit_message,
    DWORD& error) {
    error = ERROR_SUCCESS;
    if (running()) {
        error = ERROR_ALREADY_EXISTS;
        return false;
    }

    join_workers();
    close_handles();

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE telemetry_read = nullptr;
    HANDLE telemetry_write = nullptr;
    HANDLE control_read = nullptr;
    HANDLE control_write = nullptr;
    HANDLE null_input = INVALID_HANDLE_VALUE;

    auto cleanup_locals = [&]() {
        close_if_valid(stdout_read);
        close_if_valid(stdout_write);
        close_if_valid(telemetry_read);
        close_if_valid(telemetry_write);
        close_if_valid(control_read);
        close_if_valid(control_write);
        close_if_valid(null_input);
    };

    if (!CreatePipe(&stdout_read, &stdout_write, &attributes, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&telemetry_read, &telemetry_write, &attributes, 0) ||
        !SetHandleInformation(telemetry_read, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&control_read, &control_write, &attributes, 0) ||
        !SetHandleInformation(control_write, HANDLE_FLAG_INHERIT, 0)) {
        error = GetLastError();
        cleanup_locals();
        return false;
    }

    null_input = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;
    startup.hStdInput = null_input != INVALID_HANDLE_VALUE
        ? null_input
        : GetStdHandle(STD_INPUT_HANDLE);

    std::wstring command_line = command_builder(telemetry_write, control_read);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr,
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.c_str(),
        &startup,
        &process);
    if (!created) {
        error = GetLastError();
        cleanup_locals();
        return false;
    }

    close_if_valid(stdout_write);
    close_if_valid(telemetry_write);
    close_if_valid(control_read);
    close_if_valid(null_input);

    {
        std::lock_guard lock(state_mutex_);
        process_ = process;
        stdout_read_ = std::exchange(stdout_read, nullptr);
        telemetry_read_ = std::exchange(telemetry_read, nullptr);
        control_write_ = std::exchange(control_write, nullptr);
        notify_window_ = notify_window;
        output_message_ = output_message;
        telemetry_message_ = telemetry_message;
        exit_message_ = exit_message;
        ++generation_;
        stop_requested_.store(false, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
    }
    {
        std::lock_guard lock(telemetry_mutex_);
        telemetry_parser_.clear();
        telemetry_notification_pending_ = false;
    }

    const std::uint64_t active_generation = generation();
    try {
        output_thread_ = std::thread(
            &CoreProcessController::read_output, this, stdout_read_, active_generation);
        telemetry_thread_ = std::thread(
            &CoreProcessController::read_telemetry, this, telemetry_read_, active_generation);
        wait_thread_ = std::thread(
            &CoreProcessController::wait_for_exit, this, process_.hProcess, active_generation);
    } catch (...) {
        TerminateProcess(process_.hProcess, 1);
        WaitForSingleObject(process_.hProcess, INFINITE);
        join_workers();
        close_handles();
        running_.store(false, std::memory_order_release);
        error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }

    return true;
}

bool CoreProcessController::running() const {
    return running_.load(std::memory_order_acquire);
}

std::uint64_t CoreProcessController::generation() const {
    std::lock_guard lock(state_mutex_);
    return generation_;
}

bool CoreProcessController::write_control(std::string_view line) {
    if (line.empty()) {
        return false;
    }
    std::lock_guard lock(state_mutex_);
    if (!running_.load(std::memory_order_relaxed) || control_write_ == nullptr) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(
        control_write_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    return ok && written == static_cast<DWORD>(line.size());
}

void CoreProcessController::request_stop(DWORD graceful_timeout_ms) {
    if (!running() || stop_requested_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    write_control("shutdown\n");

    if (fallback_thread_.joinable()) {
        fallback_thread_.join();
    }
    HANDLE process = nullptr;
    std::uint64_t active_generation = 0;
    {
        std::lock_guard lock(state_mutex_);
        process = process_.hProcess;
        active_generation = generation_;
    }
    fallback_thread_ = std::thread([this, process, active_generation, graceful_timeout_ms]() {
        if (process == nullptr || WaitForSingleObject(process, graceful_timeout_ms) != WAIT_TIMEOUT) {
            return;
        }
        if (generation() == active_generation && running()) {
            TerminateProcess(process, 0);
        }
    });
}

void CoreProcessController::shutdown_and_wait(DWORD graceful_timeout_ms) {
    if (!running()) {
        join_workers();
        close_handles();
        return;
    }
    request_stop(graceful_timeout_ms);

    HANDLE process = nullptr;
    const std::uint64_t active_generation = generation();
    {
        std::lock_guard lock(state_mutex_);
        process = process_.hProcess;
    }
    if (process != nullptr) {
        const DWORD wait_result = WaitForSingleObject(process, graceful_timeout_ms + 250);
        if (wait_result == WAIT_TIMEOUT) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, INFINITE);
        }
    }
    finalize(active_generation);
}

bool CoreProcessController::finalize(std::uint64_t generation) {
    if (generation != this->generation()) {
        return false;
    }
    join_workers();
    close_handles();
    running_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    return true;
}

bool CoreProcessController::take_latest_telemetry(
    std::uint64_t generation,
    TelemetrySnapshot& snapshot) {
    if (generation != this->generation()) {
        return false;
    }
    std::lock_guard lock(telemetry_mutex_);
    snapshot = telemetry_parser_.snapshot();
    telemetry_notification_pending_ = false;
    return snapshot.revision != 0;
}

void CoreProcessController::read_output(HANDLE pipe, std::uint64_t generation) {
    char buffer[4096];
    for (;;) {
        DWORD bytes_read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, nullptr) || bytes_read == 0) {
            return;
        }
        auto message = std::make_unique<CoreOutputChunk>();
        message->generation = generation;
        message->bytes.assign(buffer, buffer + bytes_read);
        if (!PostMessageW(
                notify_window_, output_message_, 0,
                reinterpret_cast<LPARAM>(message.get()))) {
            return;
        }
        message.release();
    }
}

void CoreProcessController::read_telemetry(HANDLE pipe, std::uint64_t generation) {
    char buffer[4096];
    for (;;) {
        DWORD bytes_read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, nullptr) || bytes_read == 0) {
            return;
        }

        bool notify = false;
        {
            std::lock_guard lock(telemetry_mutex_);
            if (telemetry_parser_.append(std::string_view(buffer, bytes_read)) &&
                !telemetry_notification_pending_) {
                telemetry_notification_pending_ = true;
                notify = true;
            }
        }
        if (notify && !PostMessageW(
                notify_window_, telemetry_message_, static_cast<WPARAM>(generation), 0)) {
            std::lock_guard lock(telemetry_mutex_);
            telemetry_notification_pending_ = false;
            return;
        }
    }
}

void CoreProcessController::wait_for_exit(HANDLE process, std::uint64_t generation) {
    WaitForSingleObject(process, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process, &exit_code);
    running_.store(false, std::memory_order_release);

    auto notice = std::make_unique<CoreExitNotice>();
    notice->generation = generation;
    notice->exit_code = exit_code;
    if (PostMessageW(
            notify_window_, exit_message_, 0,
            reinterpret_cast<LPARAM>(notice.get()))) {
        notice.release();
    }
}

void CoreProcessController::close_handles() {
    std::lock_guard lock(state_mutex_);
    close_if_valid(stdout_read_);
    close_if_valid(telemetry_read_);
    close_if_valid(control_write_);
    close_if_valid(process_.hThread);
    close_if_valid(process_.hProcess);
    process_.dwProcessId = 0;
    process_.dwThreadId = 0;
}

void CoreProcessController::join_workers() {
    if (fallback_thread_.joinable()) fallback_thread_.join();
    if (wait_thread_.joinable()) wait_thread_.join();
    if (output_thread_.joinable()) output_thread_.join();
    if (telemetry_thread_.joinable()) telemetry_thread_.join();
}

} // namespace lanspeak::gui
