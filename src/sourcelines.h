// The Source Resolution rule, in one place because two very different things apply it: the video
// path (against mpv's frame size) and the hook DLL (against a game's backbuffer).
//
// Deliberately dependency-free -- no Qt, no Windows -- so the 32-bit hook DLL and a plain test can
// both include it.
#pragma once

#include <cmath>

// Width that pairs with `lines` at the aspect ratio of fullW x fullH. Returns 0 when there is no
// sensible answer, which callers read as "use the native size".
//
// Rounded to the nearest even number, which is what the conventional figures actually are: 16:9 at
// 480 lines is 853.33 wide and is universally called 854, while at 240 lines it is 426.67 and is
// universally called 426. Nearest-even lands on both without special-casing either, and an even
// width is what chroma-subsampled formats want anyway.
inline int widthForLines(int fullW, int fullH, int lines)
{
    if (fullW <= 0 || fullH <= 0 || lines <= 0)
        return 0;
    const double w = double(fullW) * lines / fullH;
    const int even = int(std::lround(w / 2.0)) * 2;
    return even < 2 ? 2 : even;
}
