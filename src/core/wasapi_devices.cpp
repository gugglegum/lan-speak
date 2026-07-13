#include "core/wasapi_devices.h"

#include "core/diagnostics.h"
#include "core/options.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propkey.h>
#include <propsys.h>

#include <algorithm>
#include <iostream>

namespace lanspeak::core {

std::wstring flow_name(EDataFlow flow) {
    switch (flow) {
    case eRender: return L"render";
    case eCapture: return L"capture";
    case eAll: return L"all";
    default: return L"unknown";
    }
}

std::wstring role_name(ERole role) {
    switch (role) {
    case eConsole: return L"console";
    case eMultimedia: return L"multimedia";
    case eCommunications: return L"communications";
    default: return L"unknown";
    }
}

std::wstring get_device_id(IMMDevice& device) {
    LPWSTR raw_id = nullptr;
    const HRESULT result = device.GetId(&raw_id);
    if (FAILED(result)) {
        return L"<GetId failed: " + hresult_text(result) + L">";
    }
    std::wstring id(raw_id);
    CoTaskMemFree(raw_id);
    return id;
}

std::wstring get_friendly_name(IMMDevice& device) {
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    HRESULT result = device.OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(result)) {
        return L"<OpenPropertyStore failed: " + hresult_text(result) + L">";
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    result = properties->GetValue(PKEY_Device_FriendlyName, &value);
    if (FAILED(result)) {
        PropVariantClear(&value);
        return L"<PKEY_Device_FriendlyName failed: " + hresult_text(result) + L">";
    }
    std::wstring name = value.vt == VT_LPWSTR && value.pwszVal != nullptr
        ? value.pwszVal
        : L"<unnamed>";
    PropVariantClear(&value);
    return name;
}

std::optional<EDataFlow> get_device_flow(IMMDevice& device) {
    Microsoft::WRL::ComPtr<IMMEndpoint> endpoint;
    HRESULT result = device.QueryInterface(IID_PPV_ARGS(&endpoint));
    if (FAILED(result)) {
        std::wcout << L"  flow: QueryInterface(IMMEndpoint) failed: "
                   << hresult_text(result) << L"\n";
        return std::nullopt;
    }
    EDataFlow flow = eAll;
    result = endpoint->GetDataFlow(&flow);
    if (FAILED(result)) {
        std::wcout << L"  flow: GetDataFlow failed: " << hresult_text(result) << L"\n";
        return std::nullopt;
    }
    return flow;
}

bool collect_active_endpoints(
    IMMDeviceEnumerator& enumerator,
    std::vector<EndpointCandidate>& endpoints) {
    endpoints.clear();
    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    HRESULT result = enumerator.EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(result)) {
        std::wcout << L"EnumAudioEndpoints failed: " << hresult_text(result) << L"\n";
        return false;
    }
    UINT count = 0;
    result = collection->GetCount(&count);
    if (FAILED(result)) {
        std::wcout << L"IMMDeviceCollection::GetCount failed: "
                   << hresult_text(result) << L"\n";
        return false;
    }
    endpoints.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        result = collection->Item(index, &device);
        if (FAILED(result)) {
            std::wcout << L"[" << index << L"] Item failed: "
                       << hresult_text(result) << L"\n";
            continue;
        }
        const std::optional<EDataFlow> flow = get_device_flow(*device.Get());
        if (!flow) continue;
        endpoints.push_back(EndpointCandidate{
            index,
            device,
            *flow,
            get_friendly_name(*device.Get()),
            get_device_id(*device.Get())});
    }
    return true;
}

void print_endpoint_choices(
    const std::vector<EndpointCandidate>& endpoints,
    EDataFlow flow,
    const wchar_t* label) {
    std::wcout << L"Active " << label << L" endpoints:\n";
    for (const EndpointCandidate& endpoint : endpoints) {
        if (endpoint.flow != flow) continue;
        std::wcout << L"  [" << endpoint.index << L"] " << endpoint.name << L"\n"
                   << L"      id: " << endpoint.id << L"\n";
    }
}

bool select_audio_endpoint(
    IMMDeviceEnumerator& enumerator,
    EDataFlow flow,
    ERole role,
    const std::optional<std::wstring>& selector,
    const wchar_t* label,
    Microsoft::WRL::ComPtr<IMMDevice>& device) {
    if (!selector) {
        const HRESULT result = enumerator.GetDefaultAudioEndpoint(flow, role, &device);
        if (FAILED(result)) {
            std::wcout << L"Default " << role_name(role) << L" " << label
                       << L" endpoint unavailable: " << hresult_text(result) << L"\n";
            return false;
        }
        return true;
    }

    std::vector<EndpointCandidate> endpoints;
    if (!collect_active_endpoints(enumerator, endpoints)) return false;
    const std::wstring selector_text = *selector;
    if (const std::optional<UINT> requested_index = parse_zero_based_index(selector_text)) {
        const auto match = std::find_if(
            endpoints.begin(), endpoints.end(),
            [requested_index](const EndpointCandidate& endpoint) {
                return endpoint.index == *requested_index;
            });
        if (match == endpoints.end()) {
            std::wcout << L"No active endpoint with index [" << *requested_index << L"]\n";
            print_endpoint_choices(endpoints, flow, label);
            return false;
        }
        if (match->flow != flow) {
            std::wcout << L"Endpoint [" << *requested_index << L"] is "
                       << flow_name(match->flow) << L", but " << label << L" requires "
                       << flow_name(flow) << L"\n";
            print_endpoint_choices(endpoints, flow, label);
            return false;
        }
        device = match->device;
        std::wcout << L"Selected " << label << L" endpoint [" << match->index
                   << L"]: " << match->name << L"\n";
        return true;
    }

    const std::wstring needle = lowercase_copy(selector_text);
    std::vector<const EndpointCandidate*> matches;
    for (const EndpointCandidate& endpoint : endpoints) {
        if (endpoint.flow != flow) continue;
        const std::wstring name = lowercase_copy(endpoint.name);
        const std::wstring id = lowercase_copy(endpoint.id);
        if (name.find(needle) != std::wstring::npos || id.find(needle) != std::wstring::npos) {
            matches.push_back(&endpoint);
        }
    }
    if (matches.empty()) {
        std::wcout << L"No " << label << L" endpoint matches selector: "
                   << selector_text << L"\n";
        print_endpoint_choices(endpoints, flow, label);
        return false;
    }
    if (matches.size() > 1) {
        std::wcout << L"Ambiguous " << label << L" endpoint selector: "
                   << selector_text << L"\n";
        for (const EndpointCandidate* match : matches) {
            std::wcout << L"  [" << match->index << L"] " << match->name << L"\n"
                       << L"      id: " << match->id << L"\n";
        }
        return false;
    }
    device = matches.front()->device;
    std::wcout << L"Selected " << label << L" endpoint [" << matches.front()->index
               << L"]: " << matches.front()->name << L"\n";
    return true;
}

} // namespace lanspeak::core
