#include "winmm_exports.h"
#include "log.h"
#include "menu.h"
#include "overlay.h"
#include "ue.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::wstring ModulePath(HMODULE m)
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(m, buf, MAX_PATH);
    return buf;
}

static DWORD g_mainThread; // UE runs its game thread on the process main thread, which loads us

static void Main(HMODULE self)
{
    fs::path dll = ModulePath(self);
    km::g_dir = dll.parent_path().wstring() + L"\\";
    km::Log("Ketamine Menu v%s", KM_VERSION);
    km::Log("Loaded as: %ls", dll.c_str());

    fs::path exe = ModulePath(nullptr);
    km::Log("Process: %ls", exe.c_str());
    std::ifstream vf(exe.parent_path() / "../../../Version.txt");
    std::string ver;
    std::getline(vf, ver);
    km::Log("Game version: %s", ver.empty() ? "(Version.txt not found)" : ver.c_str());

    // Forwarding self-check: our exports must land in the real System32 winmm.
    HMODULE owner = nullptr;
    if (auto fn = GetProcAddress(self, "timeGetTime"))
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(fn), &owner);
    km::Log("winmm forwarding: %s (%ls)", owner && owner != self ? "OK" : "FAILED",
            owner ? ModulePath(owner).c_str() : L"unresolved");

    menu::Init(self, ver); // settings + game-thread tick, before any hook can call into them
    km::Log("Overlay: %s", overlay::Install() ? "hooks installed" : "disabled");
    ue::Init(g_mainThread);

#ifdef KM_DUMPER // dev build only (cmake -DKM_DUMPER=ON): re-dump the SDK after a game patch
    if (auto d7 = km::g_dir + L"Dumper-7.dll"; GetFileAttributesW(d7.c_str()) != INVALID_FILE_ATTRIBUTES)
        km::Log("Dumper-7: %s", LoadLibraryW(d7.c_str()) ? "loaded" : "load failed");
#endif
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    // Only spawn a thread here: anything heavier would run under the loader lock.
    if (reason == DLL_PROCESS_ATTACH) {
        g_mainThread = GetCurrentThreadId();
        HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { Main(static_cast<HMODULE>(p)); return 0; },
                                self, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
