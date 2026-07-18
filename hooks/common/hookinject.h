// Getting the hook DLL into a game, the way overlays have always done it.
//
// SetWindowsHookExW with a thread id makes Windows itself load our DLL into that thread's process.
// No OpenProcess, no WriteProcessMemory, no CreateRemoteThread, and nothing written into the game's
// install folder. That last part is the whole point: the old proxy-DLL approach needed to drop a
// d3d9.dll next to the game exe, which needs elevation, looks exactly like DLL hijacking, and breaks
// whenever the game updates. This is what Discord, Steam and RivaTuner do instead.
//
// Scoped to one thread, not the desktop. A global hook (thread id 0) would load us into every GUI
// process on the machine, which is a lot of blast radius for a video player. Targeting the game's
// own window thread gets us into exactly one process.
//
// The hook only exists long enough to deliver the DLL. Once the DLL signals that it is in, we
// unhook: the DLL has pinned itself and lives on independently, so nothing needs to stay installed.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hookshared.h"

namespace hook {

// Same log file the DLL writes to, so an injection attempt and what the DLL did next read as one
// sequence. Injection failures are all in the "silently nothing happened" family otherwise.
inline void injlog(const char *fmt, ...)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("TEMP", path, MAX_PATH)) return;
    strcat_s(path, "\\sp_hook.log");
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

struct FindThread { DWORD pid; DWORD tid; };

inline BOOL CALLBACK findThreadProc(HWND hwnd, LPARAM lp)
{
    auto *f = reinterpret_cast<FindThread *>(lp);
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid == f->pid && IsWindowVisible(hwnd)) { f->tid = tid; return FALSE; }
    return TRUE;
}

// The GUI thread to hang the hook off. A message hook needs a thread with a message queue, and the
// thread that owns the visible window is guaranteed to have one and to be pumping it.
inline DWORD guiThreadOf(DWORD pid)
{
    FindThread f{ pid, 0 };
    EnumWindows(&findThreadProc, reinterpret_cast<LPARAM>(&f));
    return f.tid;
}

inline void loadedEventName(wchar_t *buf, size_t n, DWORD pid)
{
    _snwprintf(buf, n, L"%s_%lu", hook::LOADED_EVENT, pid);
}

// Load dllPath into the process that owns pid. Returns true once the DLL confirms it is running
// there. Caller must supply a DLL of the same bitness as the target: a 32-bit process can only be
// hooked with a 32-bit DLL from a 32-bit installer process, which is what the helper exe is for.
inline bool injectInto(DWORD pid, const wchar_t *dllPath, DWORD waitMs = 5000)
{
    const DWORD tid = guiThreadOf(pid);
    injlog("inject: pid %lu -> gui thread %lu", pid, tid);
    if (!tid) return false;

    // The DLL sets this once it is attached and has claimed the process. Create it before hooking,
    // otherwise a fast load can signal into an event that does not exist yet.
    wchar_t evName[128];
    loadedEventName(evName, 128, pid);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, evName);
    if (!ev) return false;

    // SetWindowsHookEx needs an HMODULE for the DLL, which means loading it here as well. That runs
    // its DllMain in *this* process, where it must do nothing: mark ourselves so it bails out.
    SetEnvironmentVariableW(L"SP_HOOK_INJECTOR", L"1");
    HMODULE dll = LoadLibraryW(dllPath);          // our own copy, purely to get a valid HMODULE
    if (!dll) { injlog("inject: LoadLibrary failed %lu", GetLastError()); CloseHandle(ev); return false; }
    auto proc = reinterpret_cast<HOOKPROC>(GetProcAddress(dll, "SpHookProc"));
    if (!proc) { injlog("inject: no SpHookProc export"); FreeLibrary(dll); CloseHandle(ev); return false; }

    HHOOK hh = SetWindowsHookExW(WH_GETMESSAGE, proc, dll, tid);
    if (!hh) { injlog("inject: SetWindowsHookEx failed %lu", GetLastError()); FreeLibrary(dll); CloseHandle(ev); return false; }

    // Nudge the queue so the hook fires now rather than whenever the game next sees input.
    PostThreadMessageW(tid, WM_NULL, 0, 0);
    const DWORD w = WaitForSingleObject(ev, waitMs);
    const bool ok = w == WAIT_OBJECT_0;
    injlog("inject: hook installed, wait -> %lu (%s)", w, ok ? "loaded" : "TIMEOUT");

    UnhookWindowsHookEx(hh);                       // delivered; the DLL keeps itself alive from here
    FreeLibrary(dll);
    CloseHandle(ev);
    return ok;
}

// Already carrying our DLL? Cheap enough to ask by trying to open the event the DLL leaves behind,
// which avoids walking the target's module list (and avoids needing a handle to it at all).
inline bool alreadyInjected(DWORD pid)
{
    wchar_t evName[128];
    loadedEventName(evName, 128, pid);
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, evName);
    if (!ev) return false;
    const bool set = WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
    CloseHandle(ev);
    return set;
}

} // namespace hook
