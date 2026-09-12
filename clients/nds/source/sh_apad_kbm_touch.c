/* Wrapper TU -- see sh_codec.c.
 *
 * clients/common/apad_kbm_touch.c: the S6.15-S6.17 touchscreen builders,
 * hoisted out of the 3DS client's session screen SPECIFICALLY so this client
 * could link the same tested state machine rather than re-derive the
 * resistive-panel settle/anchor/tap/drag-lock logic and the sticky-modifier
 * latch. Linking it is the whole point of the hoist.
 */
#include "../../../clients/common/apad_kbm_touch.c"
