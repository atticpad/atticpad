/* apad_jni.c — the JNI boundary for net.atticpad.AtticPadNative.
 *
 * NAMING, deliberately (docs/DESIGN.md §7.2): the class is AtticPadNative, not
 * AtticPadClientNative, and every entry point is namespaced `client*` rather
 * than assuming the app has exactly one role. docs/DESIGN.md §6.4 proposes an
 * Android SERVER host later; if that happens it adds `server*` entry points
 * here. Renaming a JNI symbol churns both sides of the boundary at once, so
 * the cheap moment to get the name right is before anything calls it. This is
 * naming only — there is no server stub here and there must not be one until
 * the SELinux/Shizuku spike in docs/DESIGN.md §6.4 says the role is possible at all.
 *
 * MARSHALLING: input goes in as one int[] and stats come back as one int[],
 * rather than 20-odd scalar arguments. At 60–125 Hz the JNI call itself is
 * the cost, not the copy, and a fixed-width array means adding a field later
 * changes one constant on each side instead of a method signature.
 *
 * THREADING: none here. Every client* call must arrive on the one thread the
 * host has decided owns the session (AtticPadService's net thread). The
 * engine holds no lock and this layer adds none.
 */

#include <jni.h>
#include <stdio.h>
#include <string.h>

#include "apad_client.h"
#include "apad_qr.h"
#include "apad_ui_strings.h"
#include "atticpad/version.h"

/* ---- int[] layouts, mirrored in AtticPadNative.kt ---------------------- */

#define IN_BUTTONS        0
#define IN_AXIS0          1    /* .. IN_AXIS0 + 7           */
#define IN_TOUCH_COUNT    9
#define IN_TOUCH0         10   /* id|pressure<<8, x, y      */
#define IN_TOUCH1         13
#define IN_ACCEL          16   /* x, y, z                   */
#define IN_GYRO           19   /* pitch, roll, yaw          */
#define IN_BATTERY        22
#define IN_LEN            23

#define OUT_STATE         0
#define OUT_SESSION_ID    1
#define OUT_PAD_SLOT      2
#define OUT_RATE_HZ       3
#define OUT_RTT_MS        4
#define OUT_CLOSE_REASON  5
#define OUT_TX            6
#define OUT_RX            7
#define OUT_LAST_ERROR    8
#define OUT_RUMBLE_SERIAL 9
#define OUT_RUMBLE_LOW    10
#define OUT_RUMBLE_HIGH   11
#define OUT_RUMBLE_MS     12
#define OUT_LED_SERIAL    13
#define OUT_LED_PLAYER    14
#define OUT_LED_RGB       15
#define OUT_STATUS_SERIAL 16
#define OUT_STATUS_CODE   17
/* §10 pairing. Appended, never inserted: these indices are hand-kept in step
 * with AtticPadNative.kt and a renumber would silently repoint every field
 * the Kotlin side reads. */
#define OUT_PAIRING_REQ   18   /* §6.2 ANNOUNCE, -1 = no ANNOUNCE seen */
#define OUT_AUTH_REQUIRED 19   /* §6.4 WELCOME flags bit 0             */
#define OUT_AUTH_STATE    20   /* enum apad_client_auth                */
#define OUT_ERROR_CODE    21   /* §6.11 ERROR code, 0 = none           */
#define OUT_LEN           22

static apad_client *handle_of(jlong h)
{
    return (apad_client *)(intptr_t)h;
}

/* §5.2 touch entries and §5 axes are int16_t on the wire; Kotlin has no
 * int16, so the boundary is int and the narrowing happens here, once. */
static int16_t narrow(jint v)
{
    if (v >  32767) { return  32767; }
    if (v < -32768) { return -32768; }
    return (int16_t)v;
}

JNIEXPORT jstring JNICALL
Java_net_atticpad_AtticPadNative_version(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, APAD_VERSION_STR);
}

JNIEXPORT jint JNICALL
Java_net_atticpad_AtticPadNative_protocolVersion(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jint)APAD_VERSION;
}

JNIEXPORT jint JNICALL
Java_net_atticpad_AtticPadNative_defaultPort(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jint)APAD_DEFAULT_PORT;
}

/*
 * apad_ui_strings.h's customer-copy catalog, crossed into Kotlin.
 *
 * Kotlin's `Msg` object hand-mirrors the apad_msg_id enum (it is APPEND-ONLY,
 * shared with the 3DS client, and cannot be generated at build time the way
 * a native-only client could) and [uiMsgCount] is what proves at self-test
 * time that the mirror has not drifted out of step with the compiled table
 * — see MainActivity's self-test drift guard.
 */
JNIEXPORT jstring JNICALL
Java_net_atticpad_AtticPadNative_uiMsg(JNIEnv *env, jclass cls, jint id)
{
    (void)cls;
    return (*env)->NewStringUTF(env, apad_ui_msg((apad_msg_id)id));
}

JNIEXPORT jint JNICALL
Java_net_atticpad_AtticPadNative_uiMsgCount(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jint)apad_ui_msg_count();
}

/* ---- §13 self-test ----------------------------------------------------- */
/*
 * Runs core/testdata/vectors.h on the device. This is the check that catches
 * an endianness or alignment mistake in the NDK build of the codec — the one
 * bug class a phone shares with a console, since both are ARM and neither is
 * the x86 box the vectors were generated on.
 */
JNIEXPORT jstring JNICALL
Java_net_atticpad_AtticPadNative_selfTest(JNIEnv *env, jclass cls, jintArray out3)
{
    apad_selftest_result result;
    jint counts[3];

    (void)cls;
    memset(&result, 0, sizeof result);
    (void)apad_selftest_run(&result, NULL, NULL);

    if (out3 != NULL && (*env)->GetArrayLength(env, out3) >= 3) {
        counts[0] = (jint)result.total;
        counts[1] = (jint)result.passed;
        counts[2] = (jint)result.failed;
        (*env)->SetIntArrayRegion(env, out3, 0, 3, counts);
    }
    return (result.first_failure != NULL)
        ? (*env)->NewStringUTF(env, result.first_failure)
        : NULL;
}

/* ---- client lifecycle -------------------------------------------------- */

JNIEXPORT jlong JNICALL
Java_net_atticpad_AtticPadNative_clientCreate(JNIEnv *env, jclass cls,
                                              jstring device_name, jint caps)
{
    const char *name = NULL;
    apad_client *c;

    (void)cls;
    if (device_name != NULL) {
        name = (*env)->GetStringUTFChars(env, device_name, NULL);
    }
    c = apad_client_create(name, (uint32_t)caps);
    if (name != NULL) {
        (*env)->ReleaseStringUTFChars(env, device_name, name);
    }
    return (jlong)(intptr_t)c;
}

/*
 * §10 — the pairing secret, from the user to the C engine and no further.
 *
 * GetStringUTFChars gives modified UTF-8; for a §10.1 secret (decimal digits,
 * or a token drawn from an ASCII alphabet) that is byte-identical to UTF-8,
 * and the engine treats it as an opaque byte string in either case. What
 * matters is the release: the JVM's copy is zeroed before it is handed back,
 * because §10 says the secret never appears on the wire and leaving it in a
 * JNI scratch buffer is the same mistake one layer down.
 */
JNIEXPORT jint JNICALL
Java_net_atticpad_AtticPadNative_clientSetSecret(JNIEnv *env, jclass cls,
                                                 jlong h, jstring secret)
{
    const char *s;
    jboolean is_copy = JNI_FALSE;
    jint rc;

    (void)cls;
    if (handle_of(h) == NULL) {
        return (jint)APAD_ERR_ARG;
    }
    if (secret == NULL) {
        return (jint)apad_client_set_secret(handle_of(h), NULL);
    }
    s = (*env)->GetStringUTFChars(env, secret, &is_copy);
    if (s == NULL) {
        return (jint)APAD_ERR_ARG;
    }
    rc = (jint)apad_client_set_secret(handle_of(h), s);
    if (is_copy == JNI_TRUE) {
        /* Only a copy may be written to — scribbling on the VM's own string
         * storage would corrupt a live java.lang.String. ART always copies
         * here (it stores UTF-16 and has to convert), so this branch is the
         * one that runs; the check is for the JNI contract, not for ART. */
        apad_secure_zero((void *)(intptr_t)s, strlen(s));
    }
    (*env)->ReleaseStringUTFChars(env, secret, s);
    return rc;
}

/* §7/§8 — one unicast DISCOVER, so ANNOUNCE.pairing_required is known before
 * a HELLO goes out. APAD_ERR_STATE on timeout is the ordinary tier-3 answer. */
JNIEXPORT jint JNICALL
Java_net_atticpad_AtticPadNative_clientProbe(JNIEnv *env, jclass cls, jlong h,
                                             jstring ip, jint port,
                                             jint timeout_ms)
{
    const char *ipstr;
    jint rc;

    (void)cls;
    if (handle_of(h) == NULL || ip == NULL) {
        return (jint)APAD_ERR_ARG;
    }
    ipstr = (*env)->GetStringUTFChars(env, ip, NULL);
    if (ipstr == NULL) {
        return (jint)APAD_ERR_ARG;
    }
    rc = (jint)apad_client_probe(handle_of(h), ipstr, (uint16_t)port,
                                 (int)timeout_ms);
    (*env)->ReleaseStringUTFChars(env, ip, ipstr);
    return rc;
}

JNIEXPORT jint JNICALL
Java_net_atticpad_AtticPadNative_clientConnect(JNIEnv *env, jclass cls, jlong h,
                                               jstring ip, jint port,
                                               jint rate_hz, jint timeout_ms)
{
    const char *ipstr;
    jint rc;

    (void)cls;
    if (handle_of(h) == NULL || ip == NULL) {
        return (jint)APAD_ERR_ARG;
    }
    ipstr = (*env)->GetStringUTFChars(env, ip, NULL);
    if (ipstr == NULL) {
        return (jint)APAD_ERR_ARG;
    }
    rc = (jint)apad_client_connect(handle_of(h), ipstr, (uint16_t)port,
                                   (uint16_t)rate_hz, (int)timeout_ms);
    (*env)->ReleaseStringUTFChars(env, ip, ipstr);
    return rc;
}

JNIEXPORT jint JNICALL
Java_net_atticpad_AtticPadNative_clientPump(JNIEnv *env, jclass cls, jlong h,
                                            jintArray in, jint max_wait_ms)
{
    apad_input_state st;
    jint raw[IN_LEN];
    int i;

    (void)cls;
    if (handle_of(h) == NULL) {
        return (jint)APAD_CLIENT_CLOSED;
    }
    memset(&st, 0, sizeof st);

    if (in != NULL && (*env)->GetArrayLength(env, in) >= IN_LEN) {
        (*env)->GetIntArrayRegion(env, in, 0, IN_LEN, raw);

        /* §5.1: bits 20..31 are reserved and MUST be zero on send. The
         * encoder masks them too, but a client that puts them on the wire is
         * non-conforming and masking here keeps the mistake from ever
         * reaching apad_encode_input_state in the first place. */
        st.buttons = (uint32_t)raw[IN_BUTTONS] & APAD_BTN_VALID_MASK;
        for (i = 0; i < APAD_AXIS_COUNT; i++) {
            st.axes[i] = narrow(raw[IN_AXIS0 + i]);
        }
        /* Indices 6 and 7 are reserved (§5) — whatever the host put there. */
        st.axes[6] = 0;
        st.axes[7] = 0;

        st.touch_count = (uint8_t)((raw[IN_TOUCH_COUNT] < 0) ? 0
                          : (raw[IN_TOUCH_COUNT] > APAD_TOUCH_MAX) ? APAD_TOUCH_MAX
                          : raw[IN_TOUCH_COUNT]);
        st.touches[0].id       = (uint8_t)(raw[IN_TOUCH0] & 0xFF);
        st.touches[0].pressure = (uint8_t)((raw[IN_TOUCH0] >> 8) & 0xFF);
        st.touches[0].x        = narrow(raw[IN_TOUCH0 + 1]);
        st.touches[0].y        = narrow(raw[IN_TOUCH0 + 2]);
        st.touches[1].id       = (uint8_t)(raw[IN_TOUCH1] & 0xFF);
        st.touches[1].pressure = (uint8_t)((raw[IN_TOUCH1] >> 8) & 0xFF);
        st.touches[1].x        = narrow(raw[IN_TOUCH1 + 1]);
        st.touches[1].y        = narrow(raw[IN_TOUCH1 + 2]);

        for (i = 0; i < 3; i++) {
            st.accel[i] = narrow(raw[IN_ACCEL + i]);
            st.gyro[i]  = narrow(raw[IN_GYRO + i]);
        }
        st.battery = (uint8_t)((raw[IN_BATTERY] < 0 || raw[IN_BATTERY] > 255)
                               ? APAD_BATTERY_UNKNOWN : raw[IN_BATTERY]);
    }

    return (jint)apad_client_pump(handle_of(h), &st, (int)max_wait_ms);
}

JNIEXPORT void JNICALL
Java_net_atticpad_AtticPadNative_clientStats(JNIEnv *env, jclass cls,
                                             jlong h, jintArray out)
{
    apad_client_stats s;
    jint v[OUT_LEN];

    (void)cls;
    if (out == NULL || (*env)->GetArrayLength(env, out) < OUT_LEN) {
        return;
    }
    apad_client_get_stats(handle_of(h), &s);

    memset(v, 0, sizeof v);
    v[OUT_STATE]         = s.state;
    v[OUT_SESSION_ID]    = s.session_id;
    v[OUT_PAD_SLOT]      = s.pad_slot;
    v[OUT_RATE_HZ]       = s.input_rate_hz;
    v[OUT_RTT_MS]        = s.rtt_ms;
    v[OUT_CLOSE_REASON]  = s.close_reason;
    v[OUT_TX]            = (jint)s.tx_packets;
    v[OUT_RX]            = (jint)s.rx_packets;
    v[OUT_LAST_ERROR]    = s.last_error;
    v[OUT_RUMBLE_SERIAL] = (jint)s.rumble_serial;
    v[OUT_RUMBLE_LOW]    = s.rumble_low;
    v[OUT_RUMBLE_HIGH]   = s.rumble_high;
    v[OUT_RUMBLE_MS]     = s.rumble_duration_ms;
    v[OUT_LED_SERIAL]    = (jint)s.led_serial;
    v[OUT_LED_PLAYER]    = s.led_player;
    v[OUT_LED_RGB]       = s.led_rgb;
    v[OUT_STATUS_SERIAL] = (jint)s.status_serial;
    v[OUT_STATUS_CODE]   = s.status_code;
    v[OUT_PAIRING_REQ]   = s.pairing_required;
    v[OUT_AUTH_REQUIRED] = s.auth_required;
    v[OUT_AUTH_STATE]    = s.auth_state;
    v[OUT_ERROR_CODE]    = s.error_code;

    (*env)->SetIntArrayRegion(env, out, 0, OUT_LEN, v);
}

JNIEXPORT jstring JNICALL
Java_net_atticpad_AtticPadNative_clientMessage(JNIEnv *env, jclass cls, jlong h)
{
    (void)cls;
    return (*env)->NewStringUTF(env, apad_client_message(handle_of(h)));
}

JNIEXPORT void JNICALL
Java_net_atticpad_AtticPadNative_clientDisconnect(JNIEnv *env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    apad_client_disconnect(handle_of(h));
}

JNIEXPORT void JNICALL
Java_net_atticpad_AtticPadNative_clientDestroy(JNIEnv *env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    apad_client_destroy(handle_of(h));
}

/* ---- §10.3 pairing URI / QR --------------------------------------------- */
/*
 * Two doors into the SAME parse: a deep link hands MainActivity a URI
 * string directly (system camera, or any app that read one), and the
 * in-app scanner hands apad_qr_decode() a camera frame that only reaches
 * apad_pair_uri_parse() after quirc has turned it into the same kind of
 * string. Neither Kotlin path ever builds or walks the URI grammar itself —
 * both land here and get back {ip, port, secret} or an error code.
 */

/* {ip, port, secret} on success, or NULL. Never partially filled, matching
 * apad_pair_uri_parse()'s own contract: this is only called after a parse
 * that returned APAD_OK. */
static jobjectArray build_pair_result(JNIEnv *env, const apad_pair_uri *u)
{
    char ipbuf[16];             /* "255.255.255.255" + NUL                  */
    char portbuf[6];            /* "65535" + NUL                            */
    jclass string_cls;
    jobjectArray arr;

    (void)snprintf(ipbuf, sizeof ipbuf, "%u.%u.%u.%u",
                   (unsigned)u->addr.ip[0], (unsigned)u->addr.ip[1],
                   (unsigned)u->addr.ip[2], (unsigned)u->addr.ip[3]);
    (void)snprintf(portbuf, sizeof portbuf, "%u", (unsigned)u->addr.port);

    string_cls = (*env)->FindClass(env, "java/lang/String");
    if (string_cls == NULL) {
        return NULL;
    }
    arr = (*env)->NewObjectArray(env, 3, string_cls, NULL);
    if (arr == NULL) {
        return NULL;
    }
    (*env)->SetObjectArrayElement(env, arr, 0, (*env)->NewStringUTF(env, ipbuf));
    (*env)->SetObjectArrayElement(env, arr, 1, (*env)->NewStringUTF(env, portbuf));
    /* §10.3: a URI is exactly as sensitive as a displayed PIN. This is the
     * one place the secret crosses into a fresh jstring; the caller's job
     * (Kotlin) is to hand it straight to clientSetSecret() and hold it
     * nowhere else — the same contract clientSetSecret() already documents
     * for a typed PIN, and pairing_flow.md's rule that the secret is never
     * persisted applies here unchanged. */
    (*env)->SetObjectArrayElement(env, arr, 2, (*env)->NewStringUTF(env, u->secret));
    return arr;
}

/*
 * Parses a §10.3 URI, from a deep link or pasted text. `rc_out[0]` receives
 * APAD_OK, APAD_ERR_VERSION ("this server is newer than I am" — §10.3's `v`
 * check), or APAD_ERR_ARG (not a conforming pairing URI at all). Returns
 * {ip, port, secret} only on APAD_OK.
 */
JNIEXPORT jobjectArray JNICALL
Java_net_atticpad_AtticPadNative_pairUriParse(JNIEnv *env, jclass cls,
                                              jstring uri, jintArray rc_out)
{
    apad_pair_uri u;
    const char *s;
    jint rc;
    jobjectArray result = NULL;

    (void)cls;
    memset(&u, 0, sizeof u);
    if (uri == NULL) {
        rc = (jint)APAD_ERR_ARG;
    } else {
        s = (*env)->GetStringUTFChars(env, uri, NULL);
        if (s == NULL) {
            rc = (jint)APAD_ERR_ARG;
        } else {
            rc = (jint)apad_pair_uri_parse(&u, s);
            (*env)->ReleaseStringUTFChars(env, uri, s);
        }
    }
    if (rc == (jint)APAD_OK) {
        result = build_pair_result(env, &u);
    }
    apad_secure_zero(&u, sizeof u);
    if (rc_out != NULL && (*env)->GetArrayLength(env, rc_out) >= 1) {
        (*env)->SetIntArrayRegion(env, rc_out, 0, 1, &rc);
    }
    return result;
}

/* One recognizer per in-app scan session — create when the scanner screen
 * opens, destroy when it closes or the user cancels. Independent of any
 * apad_client handle: scanning has no session yet, by construction. */
JNIEXPORT jlong JNICALL
Java_net_atticpad_AtticPadNative_qrCreate(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jlong)(intptr_t)apad_qr_create();
}

JNIEXPORT void JNICALL
Java_net_atticpad_AtticPadNative_qrDestroy(JNIEnv *env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    apad_qr_destroy((apad_qr *)(intptr_t)h);
}

/*
 * Decodes one Camera2 YUV_420_888 Y-plane frame. `rowStride` is
 * Image.Plane.getRowStride() — it commonly exceeds `width` (alignment
 * padding); apad_qr_decode() strips it, the caller does not have to.
 *
 * `rc_out[0]` receives APAD_OK, APAD_ERR_STATE (no code in THIS frame — the
 * ordinary outcome while the user lines up the camera, not an error worth
 * showing), APAD_ERR_VERSION, or APAD_ERR_ARG (a QR code that is not a
 * pairing URI at all). Returns {ip, port, secret} only on APAD_OK.
 */
JNIEXPORT jobjectArray JNICALL
Java_net_atticpad_AtticPadNative_qrDecodeFrame(JNIEnv *env, jclass cls, jlong h,
                                               jbyteArray y, jint width,
                                               jint height, jint row_stride,
                                               jintArray rc_out)
{
    apad_qr *d = (apad_qr *)(intptr_t)h;
    apad_pair_uri u;
    jbyte *bytes;
    jint rc;
    jobjectArray result = NULL;

    (void)cls;
    memset(&u, 0, sizeof u);
    if (d == NULL || y == NULL) {
        rc = (jint)APAD_ERR_ARG;
    } else {
        bytes = (*env)->GetByteArrayElements(env, y, NULL);
        if (bytes == NULL) {
            rc = (jint)APAD_ERR_ARG;
        } else {
            rc = (jint)apad_qr_decode(d, (const uint8_t *)bytes, (int)width,
                                      (int)height, (int)row_stride, &u);
            /* JNI_ABORT: a camera frame is read-only here and never
             * modified, so there is nothing to copy back — and copying back
             * would cost a copy at up to the camera's frame rate for
             * nothing. */
            (*env)->ReleaseByteArrayElements(env, y, bytes, JNI_ABORT);
        }
    }
    if (rc == (jint)APAD_OK) {
        result = build_pair_result(env, &u);
    }
    apad_secure_zero(&u, sizeof u);
    if (rc_out != NULL && (*env)->GetArrayLength(env, rc_out) >= 1) {
        (*env)->SetIntArrayRegion(env, rc_out, 0, 1, &rc);
    }
    return result;
}
