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
    // Capture height the worker wants, i.e. ShaderPlayer's Source Resolution control. The hook
    // downscales the backbuffer to this many lines before handing the frame over, so a CRT mask sees
    // the game's native pixels instead of its upscaled ones. 0 (or >= backbuffer) captures full size.
    //
    // Lines, not a width and height, so the hook can derive the width from the backbuffer it is
    // actually looking at. That lands exactly on the game's native resolution where a fixed width
    // would not: 240 lines of Sonic Mania's 1272x720 is 424 wide, which is precisely what it renders
    // internally before upscaling.
    volatile uint32_t srcLines;
    // The toggle. Set by ShaderPlayer's Hooks panel or by the in-game hotkey; read by the hook DLL
    // every Present. Injection is one-way and permanent, this is what turns shading on and off.
    volatile uint32_t enabled;
    // Which game process currently owns the pipeline, claimed by the first hooked process to
    // present. Anyone else stays passthrough, so a second hooked game cannot fight over the
    // textures. Also what the panel shows as "currently shading".
    volatile uint32_t hookedPid;

    // Paint the shaded frame back over the game's own backbuffer. Independent of whether
    // ShaderPlayer is also showing it: the two destinations are separate checkboxes and both can be
    // on at once, because the capture that feeds them is the same one either way.
    //
    // Only the D3D9 backend can honour this. Capture is easy in every graphics API and painting
    // back into a live game's frame is not, which is why showing in ShaderPlayer is the route that
    // works everywhere -- and the one that scales a mismatched game resolution to the window.
    volatile uint32_t applyToGame;

    // Size of the input texture, i.e. what the hook actually captured after applying srcLines.
    // width/height above stay the backbuffer size because the write-back path needs them; a
    // capture-only consumer needs these instead.
    volatile uint32_t capWidth;
    volatile uint32_t capHeight;

    // Which backend claimed the pipeline, for the panel's status line and the hook test.
    volatile uint32_t api;
};

enum : uint32_t { API_NONE = 0, API_D3D9 = 9, API_DXGI = 11, API_GL = 1 };

inline const char *apiName(uint32_t a)
{
    switch (a) {
    case API_D3D9: return "Direct3D 9";
    case API_DXGI: return "Direct3D 10/11/12";
    case API_GL:   return "OpenGL";
    default:       return "none";
    }
}

// In-game toggle: Ctrl+Alt+S. Deliberately clear of the combinations already spoken for on a normal
// gaming desktop -- Steam is Shift+Tab, Discord is Shift+`, GeForce Experience is Alt+Z, Game Bar is
// Win+G, Fraps sits on the bare function keys. Plain Alt+Shift is a Windows layout switch, so it is
// out too. Ctrl+Alt+S is one constant, here, if it ever does collide with something.
inline constexpr int TOGGLE_VK = 'S';

inline const wchar_t *LOADED_EVENT = L"Local\\sp_hook_loaded";

// Keyed-mutex keys for the OUTPUT texture, which is a genuine one-way GPU handoff: the holder of
// the "0" key produces, releases to "1" for the consumer, who releases back to "0". A freshly
// created keyed mutex starts at 0.
enum : uint64_t { OUT_WORKER = 0, OUT_GAME = 1 };

inline void outName(wchar_t *b, size_t n, uint32_t gen) { _snwprintf(b, n, L"Local\\sp_hook_out_%u", gen); }

// ---- captured frames ------------------------------------------------------
//
// Plain shared memory, not a shared texture. The hook has already read the backbuffer into system
// memory by the time it has a frame to hand over, and ShaderPlayer has to get it back into system
// memory to upload it into its GL chain -- so routing those bytes through a D3D11 texture in
// between meant a GPU upload and a GPU readback that existed only to move CPU bytes between two
// processes.
//
// Worse than the copies was the rendezvous. A keyed mutex is a strict alternation: whoever holds it
// blocks whoever wants it next. Measured, that handshake cost the game 79% of its frame time and
// still only delivered about nine frames a second out of sixty.
//
// This is a seqlock over two slots instead. The hook writes the slot the consumer is not reading
// and then bumps `seq`; nothing it does can block, and nothing it does can be blocked. The worker
// reads `seq`, copies that slot, and re-reads `seq` to find out whether it was overtaken mid-copy,
// in which case it drops the frame and takes the next one. Dropping frames is the right behaviour
// for a capture -- the display shows 60 a second whatever happens -- and it is the game never
// waiting that matters.
inline void frameName(wchar_t *b, size_t n, uint32_t gen) { _snwprintf(b, n, L"Local\\sp_hook_frame_%u", gen); }

struct FrameHeader {
    // Bumped once per completed frame. Even/odd picks the slot, so `seq & 1` is the slot the writer
    // has most recently finished with, and the one the reader should take.
    volatile uint32_t seq;
    uint32_t width, height, pitch;
    uint32_t slotBytes;
    uint32_t _pad[10];              // pushes the pixels to their own cache lines
};

inline size_t frameMapBytes(uint32_t w, uint32_t h) { return sizeof(FrameHeader) + size_t(w) * h * 4 * 2; }

} // namespace hook
