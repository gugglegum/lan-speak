#include "core/diagnostics.h"

#include <audioclient.h>

#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

namespace lanspeak::core {
namespace {

std::wstring trim_message(std::wstring text) {
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

std::wstring hresult_hex_impl(HRESULT result) {
    std::wstringstream stream;
    stream.imbue(std::locale::classic());
    stream << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
           << static_cast<std::uint32_t>(result);
    return stream.str();
}

std::wstring system_message(HRESULT result) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    if (length == 0 || buffer == nullptr) return {};
    std::wstring message(buffer, length);
    LocalFree(buffer);
    return trim_message(std::move(message));
}

std::wstring audclnt_error_name(HRESULT result) {
    switch (result) {
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return L"AUDCLNT_E_UNSUPPORTED_FORMAT";
    case AUDCLNT_E_DEVICE_IN_USE: return L"AUDCLNT_E_DEVICE_IN_USE";
    case AUDCLNT_E_DEVICE_INVALIDATED: return L"AUDCLNT_E_DEVICE_INVALIDATED";
    case AUDCLNT_E_SERVICE_NOT_RUNNING: return L"AUDCLNT_E_SERVICE_NOT_RUNNING";
    case AUDCLNT_E_BUFFER_SIZE_ERROR: return L"AUDCLNT_E_BUFFER_SIZE_ERROR";
    case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: return L"AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
    case AUDCLNT_E_INVALID_DEVICE_PERIOD: return L"AUDCLNT_E_INVALID_DEVICE_PERIOD";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return L"AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return L"AUDCLNT_E_ENDPOINT_CREATE_FAILED";
    case AUDCLNT_E_RESOURCES_INVALIDATED: return L"AUDCLNT_E_RESOURCES_INVALIDATED";
    default: return {};
    }
}

} // namespace

std::wstring hresult_hex(HRESULT result) {
    return hresult_hex_impl(result);
}

std::wstring hresult_text(HRESULT result) {
    std::wstring text = hresult_hex_impl(result);
    const std::wstring audio_name = audclnt_error_name(result);
    if (!audio_name.empty()) text += L" " + audio_name;
    const std::wstring message = system_message(result);
    if (!message.empty()) text += L" (" + message + L")";
    return text;
}

std::wstring win32_message(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    if (length == 0 || buffer == nullptr) {
        return L"error " + std::to_wstring(code);
    }
    std::wstring message(buffer, length);
    LocalFree(buffer);
    return trim_message(std::move(message));
}

std::wstring winsock_error_text(int error) {
    return L"WSA " + std::to_wstring(error) + L" (" +
        win32_message(static_cast<DWORD>(error)) + L")";
}

} // namespace lanspeak::core
