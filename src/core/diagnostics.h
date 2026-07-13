#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>

#include <string>

namespace lanspeak::core {

std::wstring hresult_hex(HRESULT result);
std::wstring hresult_text(HRESULT result);
std::wstring win32_message(DWORD code);
std::wstring winsock_error_text(int error = WSAGetLastError());

} // namespace lanspeak::core
