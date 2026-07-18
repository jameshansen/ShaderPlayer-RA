// Self-check for the Source Resolution width rule. The rounding is the whole point: 16:9 at 480
// lines has to come out 854 and at 240 lines has to come out 426, and a plain round() or a floor()
// gets one of those wrong. Build and run:
//
//   cl /nologo /EHsc /std:c++17 tools\test_sourcelines.cpp && test_sourcelines.exe

#include "../src/sourcelines.h"

#include <cassert>
#include <cstdio>

int main()
{
    // 16:9, the case that fixes the rounding convention.
    assert(widthForLines(1920, 1080, 480) == 854);    // 853.33, called 854 everywhere
    assert(widthForLines(1920, 1080, 240) == 426);    // 426.67, called 426 everywhere
    assert(widthForLines(1920, 1080, 360) == 640);    // exact
    assert(widthForLines(1920, 1080, 720) == 1280);   // exact
    assert(widthForLines(1920, 1080, 1080) == 1920);  // exact, and identity at native
    assert(widthForLines(1920, 1080, 576) == 1024);   // 1024.0, exact

    // 4:3: the same line count must give a different width, which is the reason this is a rule and
    // not a lookup table of fixed sizes.
    assert(widthForLines(640, 480, 480) == 640);
    assert(widthForLines(640, 480, 240) == 320);
    assert(widthForLines(640, 480, 224) == 298);      // 298.67 -> 298

    // A game's backbuffer. Sonic Mania renders 424x240 internally and upscales to 1272x720, so 240
    // lines has to land back on exactly 424 or the mask geometry is wrong.
    assert(widthForLines(1272, 720, 240) == 424);

    // Degenerate input means "no answer", i.e. use the native size.
    assert(widthForLines(0, 0, 480) == 0);
    assert(widthForLines(1920, 1080, 0) == 0);
    assert(widthForLines(1920, 0, 480) == 0);

    // Never collapses to zero width on an extreme aspect.
    assert(widthForLines(2, 1080, 1) == 2);

    std::puts("sourcelines: all checks passed");
    return 0;
}
