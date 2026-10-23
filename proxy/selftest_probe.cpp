// Fake stand-in for native-probe.dll, used only by the proxy self-test.
//
// The real probe refuses to load without the launcher's COOP_PROBE_* environment,
// so the self-test substitutes this harmless DLL under the name native-probe.dll.
// On load it writes a marker file; the fake game later checks that the marker
// already exists when main() runs, proving the proxy loaded the "probe" before the
// game's entrypoint executed - the ordering the real probe depends on.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        wchar_t marker[MAX_PATH] = L"probe-loaded.marker";
        GetEnvironmentVariableW(L"COOP_PROXY_SELFTEST_MARKER", marker, MAX_PATH);
        HANDLE f = CreateFileW(marker, GENERIC_WRITE, FILE_SHARE_READ, 0,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (f != INVALID_HANDLE_VALUE) {
            const char* msg = "probe attach before entry\n";
            DWORD wrote = 0;
            WriteFile(f, msg, (DWORD)lstrlenA(msg), &wrote, 0);
            CloseHandle(f);
        }
    }
    return TRUE;
}
