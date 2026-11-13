#include "windows_gpu.hpp"
#include "labyrinth.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <map>

namespace labyrinth {

std::vector<WindowsGpuAdapter> windows_gpu_adapters() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return {};

    struct Entry {
        WindowsGpuAdapter adapter;
        LUID luid;
        UINT preference = std::numeric_limits<UINT>::max();
    };
    std::vector<Entry> entries;
    std::map<std::string, unsigned> counts;
    for (UINT index = 0; ; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto status = factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (status == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(status)) return {};
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            continue;

        const auto hardware = std::to_string(description.VendorId) + ":" +
            std::to_string(description.DeviceId) + ":" + std::to_string(description.SubSysId) + ":" +
            std::to_string(description.Revision);
        auto id = hardware + ":" + std::to_string(counts[hardware]++);
        entries.push_back({{static_cast<int>(index), std::move(id), path_utf8(description.Description)},
                           description.AdapterLuid});
    }

    ComPtr<IDXGIFactory6> preferred;
    if (SUCCEEDED(factory.As(&preferred))) {
        for (UINT rank = 0; ; ++rank) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto status = preferred->EnumAdapterByGpuPreference(
                rank, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.GetAddressOf()));
            if (FAILED(status)) break;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description))) continue;
            const auto entry = std::ranges::find_if(entries, [&](const Entry& item) {
                return item.luid.HighPart == description.AdapterLuid.HighPart &&
                       item.luid.LowPart == description.AdapterLuid.LowPart;
            });
            if (entry != entries.end()) entry->preference = rank;
        }
        std::ranges::stable_sort(entries, {}, &Entry::preference);
    }

    std::vector<WindowsGpuAdapter> result;
    result.reserve(entries.size());
    for (auto& entry : entries) result.push_back(std::move(entry.adapter));
    return result;
}

} // namespace labyrinth
