// GPU usage: cheap adapter-wide numbers via NVML when present, per-process numbers via PDH
// (any vendor) only when asked.
#pragma once
#include <windows.h>
#include <pdh.h>

#include <cstdint>
#include <map>
#include <string>

namespace yielding {

struct GpuUsage {
    double util = -1;   // adapter-wide percent; -1 = unknown
    uint64_t vram = 0;  // dedicated bytes
    std::map<std::wstring, double> util_by_type;  // per-process: engine type -> percent
};

class Gpu {
public:
    ~Gpu();
    void init();
    uint64_t vram_total() const { return vram_total_; }
    bool present() const { return vram_total_ > 0; }
    // Adapter-wide usage: NVML (utilization + VRAM) or, without it, PDH adapter memory (VRAM only).
    bool global(GpuUsage* out);
    // Per-process usage. Utilization needs two samples, so the first call after open returns
    // zero utilization.
    std::map<uint32_t, GpuUsage> per_process();
    void close_query();

private:
    HMODULE nvml_ = nullptr;
    void* dev_ = nullptr;
    uint64_t vram_total_ = 0;
    PDH_HQUERY query_ = nullptr, adapter_query_ = nullptr;
    PDH_HCOUNTER util_ = nullptr, mem_ = nullptr, adapter_mem_ = nullptr;
};

}  // namespace yielding
