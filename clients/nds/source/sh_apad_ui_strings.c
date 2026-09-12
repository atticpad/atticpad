/* Wrapper TU -- see sh_codec.c.
 *
 * The pragma is a TOOLCHAIN DIFFERENCE, not a bug in the shared file, and it
 * is here rather than in clients/common/ because clients/common/ is not this
 * client's to edit. apad_ui_strings.c guards its lookup with
 *
 *     if ((int)id < 0 || id >= APAD_MSG_COUNT)
 *
 * On the ARM EABI an enum whose enumerators are all non-negative has an
 * UNSIGNED underlying type, so gcc 16 proves `(int)id < 0` can never be true
 * and -Wtype-limits fires; under -Werror that is a hard build failure. On
 * x86-64 Linux, where every other build of this file happens, the same enum is
 * a signed int and the comparison is live, so the warning never appears there.
 * The guard is correct and defensive on both targets -- suppress the
 * diagnostic here, do not remove the check there.
 */
#pragma GCC diagnostic ignored "-Wtype-limits"
#include "../../../clients/common/apad_ui_strings.c"
