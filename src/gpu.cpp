#include "gpu.h"

#include <dxgi.h>
#include <pdhmsg.h>

#include <string>
#include <vector>

namespace afk {

namespace {
struct NvmlUtil { unsigned gpu, memory; };
struct NvmlMem { unsigned long long total, free, used; };
typedef int (*FnInit)();
typedef int (*FnHandle)(unsigned, void**);
typedef int (*FnUtil)(void*, NvmlUtil*);
typedef int (*FnMem)(void*, NvmlMem*);
FnUtil g_util;
FnMem g_mem;

template <class T>
std::vector<BYTE> counter_array(PDH_HCOUNTER c, DWORD fmt, DWORD* count) {
    DWORD size = 0;
    *count = 0;
    std::vector<BYTE> buf;
    if (PdhGetFormattedCounterArrayW(c, fmt, &size, count, nullptr) != PDH_MORE_DATA) return buf;
    buf.resize(size);
    if (PdhGetFormattedCounterArrayW(c, fmt, &size, count, (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data()) != ERROR_SUCCESS)
        *count = 0;
    return buf;
}

uint32_t instance_pid(const wchar_t* name) {
    const wchar_t* p = wcsstr(name, L"pid_");
    return p ? (uint32_t)wcstoul(p + 4, nullptr, 10) : 0;
}
}  // namespace

void Gpu::init() {
    nvml_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (nvml_) {
        auto init = (FnInit)GetProcAddress(nvml_, "nvmlInit_v2");
        auto handle = (FnHandle)GetProcAddress(nvml_, "nvmlDeviceGetHandleByIndex_v2");
        g_util = (FnUtil)GetProcAddress(nvml_, "nvmlDeviceGetUtilizationRates");
        g_mem = (FnMem)GetProcAddress(nvml_, "nvmlDeviceGetMemoryInfo");
        NvmlMem m{};
        if (init && handle && g_util && g_mem && init() == 0 && handle(0, &dev_) == 0 && g_mem(dev_, &m) == 0) {
            vram_total_ = m.total;
            return;
        }
        dev_ = nullptr;
    }
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            d.DedicatedVideoMemory > vram_total_)
            vram_total_ = d.DedicatedVideoMemory;
        a->Release();
    }
    f->Release();
}

bool Gpu::global(GpuUsage* out) {
    if (dev_) {
        NvmlUtil u{};
        NvmlMem m{};
        if (g_util(dev_, &u) != 0 || g_mem(dev_, &m) != 0) return false;
        out->util = u.gpu;
        out->vram = m.used;
        return true;
    }
    if (!adapter_query_) {  // a handful of instances: cheap, unlike the per-engine counters
        if (PdhOpenQueryW(nullptr, 0, &adapter_query_) != ERROR_SUCCESS) return false;
        PdhAddEnglishCounterW(adapter_query_, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &adapter_mem_);
    }
    if (!adapter_mem_ || PdhCollectQueryData(adapter_query_) != ERROR_SUCCESS) return false;
    DWORD n = 0;
    auto buf = counter_array<LONGLONG>(adapter_mem_, PDH_FMT_LARGE, &n);
    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    out->util = -1;
    out->vram = 0;
    for (DWORD i = 0; i < n; ++i)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && (uint64_t)items[i].FmtValue.largeValue > out->vram)
            out->vram = (uint64_t)items[i].FmtValue.largeValue;  // the busiest adapter
    return n > 0;
}

std::map<uint32_t, GpuUsage> Gpu::per_process() {
    std::map<uint32_t, GpuUsage> out;
    if (!query_) {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return out;
        PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &util_);
        PdhAddEnglishCounterW(query_, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &mem_);
    }
    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return out;

    DWORD n = 0;
    if (util_) {
        auto buf = counter_array<double>(util_, PDH_FMT_DOUBLE, &n);
        auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
        for (DWORD i = 0; i < n; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
            const wchar_t* t = wcsstr(items[i].szName, L"engtype_");
            uint32_t pid = instance_pid(items[i].szName);
            if (pid && t) out[pid].util_by_type[t + 8] += items[i].FmtValue.doubleValue;
        }
    }
    if (mem_) {
        auto buf = counter_array<LONGLONG>(mem_, PDH_FMT_LARGE, &n);
        auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
        for (DWORD i = 0; i < n; ++i) {
            uint32_t pid = instance_pid(items[i].szName);
            if (pid && items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
                out[pid].vram += (uint64_t)items[i].FmtValue.largeValue;
        }
    }
    return out;
}

void Gpu::close_query() {
    if (query_) PdhCloseQuery(query_);
    query_ = nullptr;
    util_ = mem_ = nullptr;
}

Gpu::~Gpu() {
    close_query();
    if (adapter_query_) PdhCloseQuery(adapter_query_);
}

}  // namespace afk
