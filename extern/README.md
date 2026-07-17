# extern/

Third-party prebuilt binaries and vendored source. **Not committed to git** (see
`.gitignore`), because these are large binaries that bloat history permanently and
two of them exceed GitHub's 100MB file limit. This file documents what to drop here
so a fresh checkout can build.

Layout the build expects:

```
extern/
  mpv/
    include/mpv/*.h        # client.h, render.h, render_gl.h, stream_cb.h
    libmpv.lib             # import lib
    libmpv-2.dll           # runtime (staged next to the exe)
  librashader/
    librashader.h          # C API header
    librashader.dll.lib    # import lib (this is what we link)
    librashader.dll        # runtime (staged as librashader_capi.dll next to the exe)
  slang-shaders/           # the RetroArch shader library (presets), staged as ./shaders
```

## Sources and versions

| Component | Source | Version pinned here |
|-----------|--------|---------------------|
| libmpv | https://github.com/mpv-player/mpv (Windows builds: https://sourceforge.net/projects/mpv-player-windows/) | Client API 2.5 (`libmpv-2.dll`) |
| librashader | https://github.com/SnowflakePowered/librashader/releases | prebuilt `librashader.dll` capi release |
| slang-shaders | https://github.com/libretro/slang-shaders | upstream checkout |

Notes:
* We link `librashader.dll.lib`, not the 121MB static `librashader.lib`; the static lib
  is not needed and should not be dropped here.
* `slang-shaders` is upstream source, so a git submodule is the cleaner way to pin it
  if you'd rather not copy it by hand:
  `git submodule add https://github.com/libretro/slang-shaders extern/slang-shaders`
* Qt (6.8.3 msvc2022_64) is **not** vendored here; it's found via `CMAKE_PREFIX_PATH`
  in `CMakeLists.txt` (`C:/Qt/6.8.3/msvc2022_64`).
