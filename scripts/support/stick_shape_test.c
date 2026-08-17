/*
 * Guard: a stick's reachable set must be a DISC, not a diamond.
 *
 * Regression test for the bug where server/src/mapping.c shaped each stick
 * axis independently. With the default quadratic curve that turns a circular
 * input (cos t, sin t) into (cos^2 t, sin^2 t) -- components that sum to 1,
 * i.e. the locus |x| + |y| = 1, an exact diamond. Full deflection at 45 deg
 * measured 0.657 of the magnitude it produced at 0 deg, so every diagonal was
 * a third slower than it should have been. Both clients send a round stick;
 * the squashing was entirely server-side, which is why it showed up on the
 * 3DS and Android at once.
 *
 * This drives the REAL apad_mapping_apply() -- not a copy of its arithmetic --
 * with the REAL default profile, sweeping full-deflection inputs around a
 * circle and asserting the output magnitude stays constant.
 *
 * Built and run by scripts/build.sh's server target.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atticpad/atticpad.h"
#include "mapping.h"
#include "profiles.h"
#include "backend.h"

#define PI 3.14159265358979323846

/* Full deflection is 32767 on the wire (§5.3). The tolerance covers the
 * int16 quantisation of a re-projected vector, not a shape difference: a
 * diamond fails this by ~34%, three hundred times the tolerance. */
#define TOL 0.01

int main(void)
{
    const apad_profile *p = apad_profiles_builtin_default();
    apad_mapping_state st;
    int deg, failures = 0;
    double ref = -1.0;

    apad_mapping_state_init(&st);

    printf("== stick shape: sweeping full deflection, expecting a disc ==\n");
    printf("   angle |   out (x, y)      | |out|\n");
    printf("   ------+-------------------+-------\n");

    for (deg = 0; deg <= 360; deg += 15) {
        double t = (double)deg * PI / 180.0;
        apad_input_state in;
        apad_pad_state out;
        double ox, oy, mag;

        memset(&in, 0, sizeof in);
        in.axes[APAD_AXIS_LX] = (int16_t)lrint(cos(t) * 32767.0);
        in.axes[APAD_AXIS_LY] = (int16_t)lrint(sin(t) * 32767.0);

        apad_mapping_apply(&in, APAD_CAP_STICK_L, p, &st, &out);

        ox  = (double)out.lx / 32767.0;
        oy  = (double)out.ly / 32767.0;
        mag = sqrt(ox * ox + oy * oy);
        if (ref < 0.0) {
            ref = mag;
        }
        if (deg % 45 == 0) {
            printf("    %4d | %+.4f, %+.4f | %.4f\n", deg, ox, oy, mag);
        }
        if (fabs(mag - ref) > TOL) {
            printf("    FAIL at %d deg: |out| = %.4f, expected %.4f +- %.2f\n",
                   deg, mag, ref, TOL);
            failures++;
        }
    }

    if (failures != 0) {
        printf("== stick shape: %d direction(s) off the disc -- the reachable set is "
               "not circular. See shape_stick() in server/src/mapping.c ==\n", failures);
        return 1;
    }
    printf("== stick shape: OK -- |out| constant to within %.2f over 24 directions ==\n", TOL);
    return 0;
}
