**ShaderPlayer-RA - In Development**


Media player with CRT shaders
<img width="1282" height="832" alt="image" src="https://github.com/user-attachments/assets/772a7cd9-d65e-445d-a290-b7f44b3eb104" />

Uses RetroArch shaders for nostalgic CRT style video playback.

Also has Hooks to hook into games on the system.

<img width="3606" height="1326" alt="image" src="https://github.com/user-attachments/assets/d91b3e8e-32b8-45d2-8a08-f8bb28f04a07" />

All in alpha/testing stages / proof-of-concept

Thanks to:
* [librashader](https://github.com/SnowflakePowered/librashader), for doing all the hard work to make the RetroArch slang shader runtime standalone.
* [libmpv](https://github.com/mpv-player/mpv) (libmpv-2.dll) for video decode and playback.
* [slang-shaders](https://github.com/libretro/slang-shaders), the RetroArch shader library itself, for all the great presets.

Alpha Release
https://github.com/jameshansen/ShaderPlayer-RA/releases/latest

**About**

Ever since seeing the CRT-Royale shader in Retro Arch, I've always wanted to use it on old TV shows and movies, particularly those filmed on tape in the 90s. Something about standard definition content looks so much better on a CRT TV.

The problem - much of this is C++ based and low-level difficult code, working with shaders, OpenGL and so on, so I never was able to commit the time to make it happen. However, in the era of AI-assisted development, I was finally able to create this "dream" app of mine, and even get it doing some amazing things like hook into running games, a typically very difficult task.

So here it is, still in progress!

**Similar Projects**

ShaderGlass by mausimus offers similar capabilities, applying to any windows desktop app: https://mausimus.itch.io/shaderglass - great project!
