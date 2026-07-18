// sphelper32.exe <pid> <dll-path>
//
// SetWindowsHookEx can only load a DLL of the caller's own bitness, so 64-bit ShaderPlayer cannot
// inject into a 32-bit game by itself. This is that missing 32-bit caller and nothing else. It runs
// for the fraction of a second the injection takes, then exits: the hook is removed as soon as the
// DLL is loaded, so there is no resident helper process hanging around.
//
// Exit code 0 means the DLL is running in the target.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>

#include "../common/hookinject.h"

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) return 2;
    const DWORD pid = wcstoul(argv[1], nullptr, 10);
    if (!pid) return 2;
    if (!hook::injectInto(pid, argv[2])) return 1;

    // Hand the pipeline to this process and switch shading on, so the helper is a complete
    // "turn the shader on in this pid" command. The panel sets the same fields for 64-bit targets
    // it injects itself, and setting them twice costs nothing.
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(hook::Ctrl), hook::CTRL_NAME);
    if (!m) return 0;
    if (auto *c = static_cast<hook::Ctrl *>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook::Ctrl)))) {
        c->hookedPid = pid;
        c->enabled = 1;
    }
    return 0;
}
