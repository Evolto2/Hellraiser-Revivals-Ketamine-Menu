// Loads the built proxy and checks that forwarded exports (by name and by ordinal) land in System32\winmm.dll.
#include <windows.h>
#include <cstdio>
#include <string>

static std::wstring OwnerOf(FARPROC fn)
{
    HMODULE owner = nullptr;
    wchar_t buf[MAX_PATH] = {};
    if (fn && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(fn), &owner))
        GetModuleFileNameW(owner, buf, MAX_PATH);
    return buf;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return 2;
    HMODULE proxy = LoadLibraryW(argv[1]);
    if (!proxy) {
        printf("FAIL load error %lu\n", GetLastError());
        return 1;
    }
    // The loader reports the real DLL under its GLOBALROOT name, so match on the suffix.
    const std::wstring want = L"\\System32\\winmm.dll";

    int fails = 0;
    for (auto [label, fn] : {std::pair{"timeGetTime", GetProcAddress(proxy, "timeGetTime")},
                             std::pair{"waveOutGetNumDevs", GetProcAddress(proxy, "waveOutGetNumDevs")},
                             std::pair{"#2", GetProcAddress(proxy, MAKEINTRESOURCEA(2))}}) {
        auto owner = OwnerOf(fn);
        bool ok = owner.size() > want.size() &&
                  _wcsicmp(owner.c_str() + owner.size() - want.size(), want.c_str()) == 0;
        fails += !ok;
        printf("%s %s -> %ls\n", ok ? "PASS" : "FAIL", label, owner.c_str());
    }
    Sleep(500); // let the trainer thread finish writing its log
    return fails ? 1 : 0;
}
