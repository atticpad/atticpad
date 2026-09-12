/*
 * clients/common/apad_glyph_mouse.h -- the mouse glyph both consoles draw in
 * the middle of the MOUSE-mode trackpad, as pixel art rather than as a
 * sentence ("drag to move, tap to click") -- the shape says what the surface
 * is. One asset, drawn 1:1 on the 3DS and the DS so it stays crisp.
 *
 * Cells: ' ' transparent, 'o' outline, 'b' body, 'h' highlight (top-left
 * light), 's' inner shadow (bottom-right), 'g' gap/seam (the button split,
 * the seam under the buttons, the wheel slot), 'w' wheel roller.
 * Each platform maps those to its own palette and draws a one-pixel drop
 * shadow first by drawing the silhouette offset (+1, +1) in the background
 * colour. Authored 2026-09-09; regenerate by hand, it is 22x32.
 */
#ifndef ATTICPAD_COMMON_APAD_GLYPH_MOUSE_H
#define ATTICPAD_COMMON_APAD_GLYPH_MOUSE_H

#define APAD_GLYPH_MOUSE_W 22
#define APAD_GLYPH_MOUSE_H 32

static const char *const apad_glyph_mouse[APAD_GLYPH_MOUSE_H] = {
    "      oooooooooo      ",
    "    ooohhhgbbbbooo    ",
    "   ohhhbbbgbbbbbbbo   ",
    "  ohbbbbgggggbbbbbbo  ",
    " ohbbbbbgwwwgbbbbbbbo ",
    " ohbbbbbgwwwgbbbbbbbo ",
    "oohbbbbbgwgwgbbbbbbboo",
    "ohbbbbbbgwwwgbbbbbbbbo",
    "ohbbbbbbgwgwgbbbbbbbbo",
    "ohbbbbbbgwwwgbbbbbbbbo",
    "ohbbbbbbgggggbbbbbbbbo",
    "ohbbbbbbbbgbbbbbbbbbbo",
    "oggggggggggggggggggggo",
    "obbbbbbbbbbbbbbbbbbbso",
    "obbbbbbbbbbbbbbbbbbbso",
    "obbbbbbbbbbbbbbbbbbbso",
    "obbbbbbbbbbbbbbbbbbbso",
    "obbbbbbbbbbbbbbbbbbbso",
    "obbbbbbbbbbbbbbbbbbbso",
    "obbbbbbbbbbbbbbbbbbbso",
    "obbbbbbbbbbbbbbbbbbbso",
    "obbbbbbbbbbbbbbbbbbbso",
    "obbbbbbbbbbbbbbbbbbbso",
    "osbbbbbbbbbbbbbbbbbbso",
    " obbbbbbbbbbbbbbbbbso ",
    " osbbbbbbbbbbbbbbbbso ",
    "  obbbbbbbbbbbbbbbso  ",
    "  osbbbbbbbbbbbbbbso  ",
    "   ossbbbbbbbbbbsso   ",
    "    oossbbbbbbssoo    ",
    "      oossssssoo      ",
    "        oooooo        ",
};

#endif /* ATTICPAD_COMMON_APAD_GLYPH_MOUSE_H */
