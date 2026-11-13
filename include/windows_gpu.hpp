#pragma once

#include <string>
#include <vector>

namespace labyrinth {

struct WindowsGpuAdapter {
    int index;
    std::string id, name;
};

// Ordered by Windows' high-performance preference. index is the native DXGI index.
std::vector<WindowsGpuAdapter> windows_gpu_adapters();

} // namespace labyrinth
