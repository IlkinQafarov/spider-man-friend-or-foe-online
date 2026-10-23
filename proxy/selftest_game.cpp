// Fake game for the proxy self-test. It statically imports Direct3DCreate9 from our
// d3d9.dll (so Windows loads the proxy during startup, exactly as the real game
// does) and checks two things the proxy must guarantee:
//   1. native-probe.dll was loaded before main() ran   -> the probe-loaded marker
//      already exists at entry (proves hooks-before-entry ordering);
//   2. Direct3DCreate9 still returns a real IDirect3D9  -> the forward to the
//      chainloaded system d3d9 works (proves the game keeps rendering normally).
// Prints PROXY_SELFTEST_PASS only if both hold. No game files are touched.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

int main() {
    wchar_t marker[MAX_PATH] = L"probe-loaded.marker";
    GetEnvironmentVariableW(L"COOP_PROXY_SELFTEST_MARKER", marker, MAX_PATH);
    bool probeLoaded = GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    bool forwarded = d3d != 0;
    if (d3d) d3d->Release();

    printf("probe_loaded_before_entry=%d\n", probeLoaded ? 1 : 0);
    printf("direct3dcreate9_forwarded=%d\n", forwarded ? 1 : 0);
    if (probeLoaded && forwarded) {
        printf("PROXY_SELFTEST_PASS\n");
        return 0;
    }
    printf("PROXY_SELFTEST_FAIL\n");
    return 1;
}
