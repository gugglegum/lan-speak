#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

namespace lanspeak::core {

struct EndpointCandidate {
    UINT index = 0;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    EDataFlow flow = eAll;
    std::wstring name;
    std::wstring id;
};

std::wstring flow_name(EDataFlow flow);
std::wstring role_name(ERole role);
std::wstring get_device_id(IMMDevice& device);
std::wstring get_friendly_name(IMMDevice& device);
std::optional<EDataFlow> get_device_flow(IMMDevice& device);
bool collect_active_endpoints(
    IMMDeviceEnumerator& enumerator,
    std::vector<EndpointCandidate>& endpoints);
void print_endpoint_choices(
    const std::vector<EndpointCandidate>& endpoints,
    EDataFlow flow,
    const wchar_t* label);
bool select_audio_endpoint(
    IMMDeviceEnumerator& enumerator,
    EDataFlow flow,
    ERole role,
    const std::optional<std::wstring>& selector,
    const wchar_t* label,
    Microsoft::WRL::ComPtr<IMMDevice>& device);

} // namespace lanspeak::core
