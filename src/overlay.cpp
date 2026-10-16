#include "overlay.h"
#include "log.h"
#include "menu.h"
#include "ue.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// COM vtable slots: stable Windows ABI, not game offsets.
constexpr int kPresent = 8, kPresent1 = 22, kExecuteCommandLists = 10;
constexpr UINT kSrvSlots = 64;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PresentFn oPresent;
Present1Fn oPresent1;
ExecuteCommandListsFn oExecuteCommandLists;

// ImGui runs on the render thread while input arrives on the game thread: everything below is guarded by g_mutex.
std::timed_mutex g_mutex;
// Direct queues seen in ExecuteCommandLists (AddRef'd, kept): the swapchain won't hand out its queue on every setup.
std::mutex g_queueMutex;
std::vector<ID3D12CommandQueue*> g_queues;
int g_queueWait; // frames waited for the presenting queue to show up before settling for another one
const char* g_api = "...";
std::string g_gpu;

HWND g_hwnd;
WNDPROC g_origWndProc;
bool g_ecHooked;
std::atomic<bool> g_failed; // overlay off for good (init failure or exception): the game keeps running untouched
int g_framesDrawn;          // per renderer build, for first-frame diagnostics

// What the renderer was built for. Back buffers are fetched per frame (never held), so only these force a rebuild.
struct Key {
    IUnknown* device;
    UINT buffers;
    DXGI_FORMAT format;
    bool operator==(const Key&) const = default;
} g_key;

struct Dx11 {
    ID3D11Device* device;
    ID3D11DeviceContext* ctx;
} g11;

struct Frame {
    ID3D12CommandAllocator* alloc;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    UINT64 fenceValue;
};

struct Dx12 {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;       // the game's present queue
    ID3D12CommandQueue* uploadQueue; // ours, for ImGui texture uploads
    ID3D12DescriptorHeap* rtvHeap;
    ID3D12DescriptorHeap* srvHeap;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE fenceEvent;
    UINT64 fenceCounter;
    UINT srvStride;
    std::vector<Frame> frames;
    std::vector<UINT> freeSrv;
} g12;

bool g_backendReady; // ImGui renderer backend initialised (DX11 or DX12)

template <class T> void SafeRelease(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

// ---------------------------------------------------------------- input

bool IsGameInput(UINT msg)
{
    switch (msg) {
    case WM_INPUT: case WM_KEYDOWN: case WM_CHAR: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        return true;
    }
    return false; // key/button releases still reach the game, so nothing stays "held" after closing the menu
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Menu key, hotkeys and key binding (bit 30 = auto-repeat).
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) && menu::OnKeyDown(UINT(wp))) return 0;
    if (menu::IsOpen() && !g_failed) {
        // Bounded wait: never let the game's window thread hang on the render thread.
        if (std::unique_lock lock(g_mutex, std::chrono::milliseconds(100)); lock.owns_lock())
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        if (msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wp, lp); // raw mouse look: swallowed, but freed
        if (IsGameInput(msg)) return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- DX12

void WaitFence(UINT64 value)
{
    if (g12.fence && g12.fence->GetCompletedValue() < value) {
        g12.fence->SetEventOnCompletion(value, g12.fenceEvent);
        WaitForSingleObject(g12.fenceEvent, 2000);
    }
}

void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard lock(g_queueMutex);
        if (g_queues.size() < 8 && std::find(g_queues.begin(), g_queues.end(), q) == g_queues.end()) {
            q->AddRef();
            g_queues.push_back(q);
            km::Log("DX12: direct queue %p seen in ExecuteCommandLists", static_cast<void*>(q));
        }
    }
    oExecuteCommandLists(q, n, lists);
}

// The queue the swapchain presents on. With frame generation or another overlay the game submits to several direct
// queues, and drawing on one that doesn't present races the present (corruption or device removal).
ID3D12CommandQueue* FindPresentQueue(IDXGISwapChain* sc, ID3D12Device* device, bool& inSwapchain)
{
    // DXGI keeps a pointer to its queue inside the swapchain object: look for a queue we know there.
    uintptr_t mem[512] = {};
    bool scanned = ue::SafeRead(sc, mem, sizeof(mem)) || ue::SafeRead(sc, mem, sizeof(mem) / 4);
    std::lock_guard lock(g_queueMutex);
    ID3D12CommandQueue* first = nullptr;
    for (ID3D12CommandQueue* q : g_queues) {
        ID3D12Device* d = nullptr;
        bool sameDevice = SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d))) && d == device;
        SafeRelease(d);
        if (!sameDevice) continue;
        inSwapchain = scanned && std::find(std::begin(mem), std::end(mem), reinterpret_cast<uintptr_t>(q)) != std::end(mem);
        if (inSwapchain) return q;
        if (!first) first = q;
    }
    return first;
}

void HookExecuteCommandLists(ID3D12Device* device)
{
    g_ecHooked = true;
    D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* tmp = nullptr;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&tmp)))) {
        km::Log("DX12: could not create a queue to hook ExecuteCommandLists");
        return;
    }
    void* target = (*reinterpret_cast<void***>(tmp))[kExecuteCommandLists];
    MH_STATUS s = MH_CreateHook(target, &hkExecuteCommandLists, reinterpret_cast<void**>(&oExecuteCommandLists));
    if (s == MH_OK) s = MH_EnableHook(target);
    km::Log("DX12: ExecuteCommandLists hook %s", MH_StatusToString(s));
    tmp->Release();
}

void ShutdownDx12()
{
    WaitFence(g12.fenceCounter);
    if (g_backendReady) ImGui_ImplDX12_Shutdown();
    for (Frame& f : g12.frames) SafeRelease(f.alloc);
    SafeRelease(g12.list);
    SafeRelease(g12.fence);
    if (g12.fenceEvent) CloseHandle(g12.fenceEvent);
    SafeRelease(g12.rtvHeap);
    SafeRelease(g12.srvHeap);
    SafeRelease(g12.uploadQueue);
    SafeRelease(g12.queue);
    SafeRelease(g12.device);
    g12 = {};
}

// Returns false while the game's queue is still unknown (retried next frame) or on failure (g_failed set).
bool InitDx12(IDXGISwapChain* sc, const DXGI_SWAP_CHAIN_DESC& sd, ID3D12Device* device)
{
    ID3D12CommandQueue* queue = nullptr;
    const char* how = "from swapchain";
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&queue)))) {
        if (!g_ecHooked) HookExecuteCommandLists(device);
        bool inSwapchain = false;
        queue = FindPresentQueue(sc, device, inSwapchain);
        // Not referenced by the swapchain yet: give its queue ~2 s of frames to submit before settling.
        if (!queue || (!inSwapchain && ++g_queueWait < 120)) return false;
        how = inSwapchain ? "found in swapchain" : "first direct queue of the device";
        queue->AddRef();
    }
    km::Log("DX12: %u buffers, format %d, %ux%u, queue %p (%s)", sd.BufferCount, sd.BufferDesc.Format,
            sd.BufferDesc.Width, sd.BufferDesc.Height, static_cast<void*>(queue), how);

    device->AddRef();
    g12.device = device;
    g12.queue = queue;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, sd.BufferCount};
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvSlots,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g12.rtvHeap))) ||
        FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g12.srvHeap))) ||
        FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g12.uploadQueue))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g12.fence))) ||
        !(g12.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr))) {
        km::Log("DX12: resource creation failed");
        g_failed = true;
        return false;
    }

    UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g12.frames.resize(sd.BufferCount);
    for (UINT i = 0; i < sd.BufferCount; i++) {
        Frame& f = g12.frames[i];
        f.rtv.ptr = g12.rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * rtvStride;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) {
            km::Log("DX12: command allocator creation failed");
            g_failed = true;
            return false;
        }
    }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g12.frames[0].alloc, nullptr,
                                         IID_PPV_ARGS(&g12.list)))) {
        km::Log("DX12: command list creation failed");
        g_failed = true;
        return false;
    }
    g12.list->Close();

    g12.srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = kSrvSlots; i-- > 0;) g12.freeSrv.push_back(i);

    ImGui_ImplDX12_InitInfo info;
    info.Device = device;
    info.CommandQueue = g12.uploadQueue;
    info.NumFramesInFlight = int(sd.BufferCount);
    info.RTVFormat = sd.BufferDesc.Format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g12.srvHeap;
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
        UINT i = 0; // shortcut: 64 slots, ImGui uses ~1 per texture; if ever exhausted, slot 0 is shared (glitch, no crash)
        if (!g12.freeSrv.empty()) {
            i = g12.freeSrv.back();
            g12.freeSrv.pop_back();
        }
        cpu->ptr = g12.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * g12.srvStride;
        gpu->ptr = g12.srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * g12.srvStride;
    };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                  D3D12_GPU_DESCRIPTOR_HANDLE) {
        g12.freeSrv.push_back(UINT((cpu.ptr - g12.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g12.srvStride));
    };
    g_backendReady = ImGui_ImplDX12_Init(&info);
    if (!g_backendReady) g_failed = true;
    return g_backendReady;
}

void Barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    g12.list->ResourceBarrier(1, &b);
}

void RenderDx12(IDXGISwapChain* sc)
{
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    UINT idx = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    if (idx >= g12.frames.size()) return;

    ID3D12Resource* buffer = nullptr;
    if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&buffer)))) return;
    Frame& f = g12.frames[idx];
    WaitFence(f.fenceValue);
    g12.device->CreateRenderTargetView(buffer, nullptr, f.rtv);
    f.alloc->Reset();
    g12.list->Reset(f.alloc, nullptr);
    Barrier(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g12.list->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    g12.list->SetDescriptorHeaps(1, &g12.srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g12.list);
    Barrier(buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g12.list->Close();
    ID3D12CommandList* lists[] = {g12.list};
    g12.queue->ExecuteCommandLists(1, lists);
    g12.queue->Signal(g12.fence, ++g12.fenceCounter);
    f.fenceValue = g12.fenceCounter;
    buffer->Release();
    if (HRESULT r = g12.device->GetDeviceRemovedReason(); FAILED(r)) {
        km::Log("DX12: device removed (0x%08lX) after %d overlay frames, overlay disabled",
                static_cast<unsigned long>(r), g_framesDrawn);
        g_failed = true;
    }
}

// ---------------------------------------------------------------- DX11

void ShutdownDx11()
{
    if (g_backendReady) ImGui_ImplDX11_Shutdown();
    SafeRelease(g11.ctx);
    SafeRelease(g11.device);
}

bool InitDx11(const DXGI_SWAP_CHAIN_DESC& sd, ID3D11Device* device)
{
    km::Log("DX11: %u buffers, format %d, %ux%u", sd.BufferCount, sd.BufferDesc.Format, sd.BufferDesc.Width,
            sd.BufferDesc.Height);
    device->AddRef();
    g11.device = device;
    device->GetImmediateContext(&g11.ctx);
    g_backendReady = ImGui_ImplDX11_Init(g11.device, g11.ctx);
    if (!g_backendReady) g_failed = true;
    return g_backendReady;
}

void RenderDx11(IDXGISwapChain* sc)
{
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return;
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = g11.device->CreateRenderTargetView(buffer, nullptr, &rtv);
    buffer->Release();
    if (FAILED(hr)) return;
    g11.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    rtv->Release();
}

// ---------------------------------------------------------------- frame

void ShutdownRenderer()
{
    if (g11.device) ShutdownDx11();
    if (g12.device || g12.queue) ShutdownDx12();
    g_backendReady = false;
    g_key = {};
    g_framesDrawn = 0;
}

// Frame generation and other overlays change how frames reach the screen: name them so a log can explain a crash.
void LogEnvironment()
{
    std::string found;
    for (const wchar_t* m : {L"sl.interposer.dll", L"sl.dlss_g.dll", L"amd_fidelityfx_framegeneration_dx12.dll",
                             L"libxess_fg.dll", L"GameOverlayRenderer64.dll", L"RTSSHooks64.dll", L"DiscordHook64.dll",
                             L"nvspcap64.dll", L"ReShade64.dll"})
        if (GetModuleHandleW(m)) found += km::Utf8(m) + " ";
    wchar_t dxgi[MAX_PATH] = L"?";
    if (HMODULE h = GetModuleHandleW(L"dxgi.dll")) GetModuleFileNameW(h, dxgi, MAX_PATH);
    km::Log("Loaded: %s| dxgi.dll: %ls", found.empty() ? "nothing notable " : found.c_str(), dxgi);
}

// Logs where an exception hit (module + offset): a log from a setup that crashes then says what broke.
int CrashFilter(const char* step, EXCEPTION_POINTERS* ep)
{
    void* at = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(at), &mod))
        GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
    km::Log("Overlay disabled: exception 0x%08lX during %s at %ls+0x%llX", ep->ExceptionRecord->ExceptionCode, step,
            name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(mod)));
    return EXCEPTION_EXECUTE_HANDLER;
}

// Runs f; an exception turns the overlay off instead of taking the game down (/EHa: locks are released on the way).
template <class F> bool Guarded(const char* step, F&& f)
{
    __try {
        f();
        return true;
    } __except (CrashFilter(step, GetExceptionInformation())) {
        return false;
    }
}

void InitImGui(HWND hwnd)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // settings live in KetamineMenu.ini
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // the game owns the OS cursor: ImGui draws its own
    menu::SetupImGui();
    ImGui_ImplWin32_Init(hwnd);
    g_hwnd = hwnd;
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
    km::Log("ImGui ready, window %p subclassed: %s", static_cast<void*>(hwnd), g_origWndProc ? "OK" : "FAILED");
}

// "NVIDIA GeForce RTX 4080 · 16 GB": the adapter the game's device was created on.
std::string AdapterName(ID3D12Device* d12, ID3D11Device* d11)
{
    DXGI_ADAPTER_DESC1 desc{};
    bool ok = false;
    if (d12) {
        IDXGIFactory4* factory = nullptr;
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(d12->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
            ok = SUCCEEDED(adapter->GetDesc1(&desc));
        SafeRelease(adapter);
        SafeRelease(factory);
    } else if (d11) {
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIAdapter1* adapter1 = nullptr;
        if (SUCCEEDED(d11->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter1))))
            ok = SUCCEEDED(adapter1->GetDesc1(&desc));
        SafeRelease(adapter1);
        SafeRelease(adapter);
        SafeRelease(dxgi);
    }
    if (!ok) return {};
    char gb[32];
    snprintf(gb, sizeof(gb), "  ·  %.0f GB", double(desc.DedicatedVideoMemory) / (1ull << 30));
    return km::Utf8(desc.Description) + gb;
}

// Makes sure ImGui and the renderer match the current swapchain. False = skip this frame.
bool EnsureRenderer(IDXGISwapChain* sc)
{
    DXGI_SWAP_CHAIN_DESC sd;
    if (FAILED(sc->GetDesc(&sd))) return false;
    if (!g_hwnd) {
        LogEnvironment();
        InitImGui(sd.OutputWindow);
    }

    ID3D12Device* d12 = nullptr;
    ID3D11Device* d11 = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&d12))) && FAILED(sc->GetDevice(IID_PPV_ARGS(&d11)))) {
        km::Log("Swapchain has neither a D3D12 nor a D3D11 device, overlay disabled");
        g_failed = true;
        return false;
    }
    IUnknown* device = d12 ? static_cast<IUnknown*>(d12) : d11;
    Key key{device, sd.BufferCount, sd.BufferDesc.Format};
    bool ok = g_backendReady && key == g_key;
    if (!ok) {
        ShutdownRenderer();
        g_api = d12 ? "DX12" : "DX11";
        ok = d12 ? InitDx12(sc, sd, d12) : InitDx11(sd, d11);
        if (ok) {
            g_key = key;
            g_gpu = AdapterName(d12, d11);
        }
        else ShutdownRenderer(); // releases partial init; g_failed decides whether we retry
    }
    device->Release();
    return ok;
}

void DrawOverlay(IDXGISwapChain* sc)
{
    // Menu closed and no toast: nothing is rendered.
    if (!EnsureRenderer(sc) || !menu::WantsFrame()) return;
    if (g_framesDrawn == 0) km::Log("Overlay: first frame (%s)", g_api);
    if (menu::IsOpen()) ClipCursor(nullptr);
    if (g11.device) ImGui_ImplDX11_NewFrame();
    else ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    menu::Draw();
    ImGui::Render();
    if (g11.device) RenderDx11(sc);
    else RenderDx12(sc);
    if (++g_framesDrawn == 1) km::Log("Overlay: first frame submitted");
}

void OnPresent(IDXGISwapChain* sc)
{
    static thread_local bool busy; // Present1 may route through Present
    if (busy || g_failed) return;
    busy = true;
    {
        std::lock_guard lock(g_mutex);
        if (!Guarded("overlay frame", [&] { DrawOverlay(sc); })) g_failed = true;
    }
    busy = false;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
    return oPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
{
    if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
    return oPresent1(sc, sync, flags, params);
}

// ---------------------------------------------------------------- install

HWND FindVisibleProcessWindow()
{
    HWND found = nullptr;
    EnumWindows(
        [](HWND w, LPARAM out) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
            *reinterpret_cast<HWND*>(out) = w;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

bool Hook(void* target, void* detour, void* original, const char* name)
{
    MH_STATUS s = MH_CreateHook(target, detour, static_cast<void**>(original));
    if (s == MH_OK) s = MH_EnableHook(target);
    km::Log("Hook %s at %p: %s", name, target, MH_StatusToString(s));
    return s == MH_OK;
}

} // namespace

namespace overlay {

const char* ApiName() { return g_api; }
const std::string& GpuName() { return g_gpu; }

bool Install()
{
    // Wait until the game has a window: by then DXGI is initialised and we don't race the engine's startup.
    while (!FindVisibleProcessWindow()) Sleep(250);

    if (MH_STATUS s = MH_Initialize(); s != MH_OK) {
        km::Log("MinHook init failed: %s", MH_StatusToString(s));
        return false;
    }

    // A throwaway DX11 swapchain on a hidden window gives us DXGI's Present address, shared by DX11 and DX12 swapchains.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KetamineMenuDummy";
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                 wc.hInstance, nullptr);

    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        GetProcAddress(LoadLibraryW(L"d3d11.dll"), "D3D11CreateDeviceAndSwapChain"));
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                 &sc, &dev, nullptr, &ctx)
                        : E_FAIL;
    bool ok = false;
    if (SUCCEEDED(hr)) {
        void** vt = *reinterpret_cast<void***>(sc);
        ok = Hook(vt[kPresent], &hkPresent, &oPresent, "Present");
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            Hook((*reinterpret_cast<void***>(sc1))[kPresent1], &hkPresent1, &oPresent1, "Present1");
            sc1->Release();
        }
    } else {
        km::Log("Dummy swapchain creation failed (hr=0x%08lX), no overlay", static_cast<unsigned long>(hr));
    }
    SafeRelease(sc);
    SafeRelease(ctx);
    SafeRelease(dev);
    DestroyWindow(dummy);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace overlay
