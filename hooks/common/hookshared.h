// Shared contract between the hook DLL (loaded into the game) and the worker thread in
// ShaderPlayer. The hook DLL owns everything durable; the worker attaches and detaches freely.
//
//   Control block: a named shared-memory struct. The hook writes size/gen/magic, the worker writes
//   heartbeat/session. Distinct fields, aligned, so no lock is needed.
//   Textures: two keyed-mutex NT-handle D3D11 textures the hook creates and the worker opens by
//   name. Names carry the generation so a resize makes the worker reopen cleanly.
//
// Liveness: the worker stamps a heartbeat every loop; the hook shades only while the heartbeat is
// fresh, otherwise it presents the game untouched. That is the "passthrough when ShaderPlayer is
// closed, resume when it opens" behaviour.
//
// Engagement is separate from injection. Once the DLL is in a process it stays for that process's
// lifetime (it pins itself, because its vtable patches must outlive the Windows hook that got it
// there). Turning the shader on and off only flips `enabled`, from ShaderPlayer's panel or from the
// in-game hotkey. That keeps the toggle instant and, more importantly, keeps it from ever unloading
// patched code out from under a live render thread.
#pragma once
#include <cstdint>
#include <cstdio>

namespace hook {

inline const wchar_t *CTRL_NAME = L"Local\\sp_hook_ctrl";
inline constexpr uint32_t MAGIC = 0x53504831;   // 'SPH1'
// Liveness, not latency. This only answers "is a worker still there", so it must be well clear of
// how long one heavy frame can take: a CRT Royale pass under GPU load can blow past half a second,
// and a tight window made the proxy read a *slow* worker as a *dead* one and flicker in and out.
inline constexpr uint32_t HEARTBEAT_STALE_MS = 2000;

// volatile because two processes share this and neither compiler may cache or sink the accesses;
// the worker's heartbeat store sits in a tight loop that an optimiser would happily hoist.
struct Ctrl {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t width;            // shared texture / backbuffer size, written by the proxy
    volatile uint32_t height;
    volatile uint32_t texGen;           // bumped by the proxy when it (re)creates textures; 0 = none yet
    uint32_t _pad;
    volatile uint64_t workerHeartbeat;  // GetTickCount64() stamped by the worker as it works
    volatile uint64_t workerSession;    // nonzero while a worker is attached (its pid)
    // Capture resolution the worker wants, i.e. ShaderPlayer's Source Resolution control. The proxy
    // downscales the backbuffer to this before handing the frame over, so a CRT mask sees the game's
    // native pixels instead of its upscaled ones. 0 (or >= backbuffer) means capture full size.
    // Appended at the end so an older worker binary reading the head of the struct still agrees.
    volatile uint32_t srcWidth;
    volatile uint32_t srcHeight;
    // The toggle. Set by ShaderPlayer's Hooks panel or by the in-game hotkey; read by the hook DLL
    // every Present. Injection is one-way and permanent, this is what turns shading on and off.
    volatile uint32_t enabled;
    // Which game process currently owns the pipeline, claimed by the first hooked process to
    // present. Anyone else stays passthrough, so a second hooked game cannot fight over the
    // textures. Also what the panel shows as "currently shading".
    volatile uint32_t hookedPid;
};

// In-game toggle: Ctrl+Alt+S. Deliberately clear of the combinations already spoken for on a normal
// gaming desktop -- Steam is Shift+Tab, Discord is Shift+`, GeForce Experience is Alt+Z, Game Bar is
// Win+G, Fraps sits on the bare function keys. Plain Alt+Shift is a Windows layout switch, so it is
// out too. Ctrl+Alt+S is one constant, here, if it ever does collide with something.
inline constexpr int TOGGLE_VK = 'S';

inline const wchar_t *LOADED_EVENT = L"Local\\sp_hook_loaded";

// Keyed-mutex keys. Each texture is a one-way handoff: the holder of the "0" key produces, releases
// to "1" for the consumer, who releases back to "0". A freshly created keyed mutex starts at 0.
enum : uint64_t { IN_GAME = 0, IN_WORKER = 1, OUT_WORKER = 0, OUT_GAME = 1 };

inline void inName(wchar_t *b, size_t n, uint32_t gen)  { _snwprintf(b, n, L"Local\\sp_hook_in_%u", gen); }
inline void outName(wchar_t *b, size_t n, uint32_t gen) { _snwprintf(b, n, L"Local\\sp_hook_out_%u", gen); }

} // namespace hook
