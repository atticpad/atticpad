/* tools/fuzz/fuzz_decoder.c
 *
 * libFuzzer harness against the AtticPad decode path (docs/PROTOCOL.md §3.1,
 * §5, §6, §10). docs/DESIGN.md §9.2: "libFuzzer against codec.c in CI. Not optional
 * hygiene: the parser runs on consoles with no MMU and no exploit
 * mitigations, where a malformed packet from anywhere on the LAN reaching a
 * buffer overrun is a real bug."
 *
 * This harness calls only the public API declared under core/include/. It
 * does not special-case any internal behavior of core/src/ -- if a call
 * documented as "returns a negative apad_result on failure" instead crashes,
 * that is exactly the class of bug this harness exists to find.
 *
 * Fuzzed surface, applied to every input in this order (mirrors how a real
 * receiver would sequence these calls):
 *
 *   1. apad_header_decode()  -- the most primitive parse: every byte pattern
 *      that ever arrives on the wire passes through something like this.
 *   2. apad_packet_parse()   -- the full §3.1 ordered validation.
 *   3. On successful parse, apad_decode_<type>() for the matching known
 *      type (or apad_decode_input_state for INPUT_STATE) -- the payload
 *      decoders, which is where the D-pad LUT, touch clamps, and reserved-
 *      field scrubbing (§2, §5) live.
 *   4. If AUTHENTICATED is set, apad_packet_verify() -- §10, using the fixed
 *      Appendix A (§14) key so that inputs derived from the seed corpus can
 *      occasionally hit the "tag verifies" branch, not just "tag rejected".
 *
 * Build: see build.sh in this directory.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "atticpad/atticpad.h"

/* §14 Appendix A derived session key -- used only so apad_packet_verify has
 * *some* key to exercise both the "matches" and "does not match" branches of
 * the constant-time compare. Fuzzing does not depend on this being secret or
 * even correct for a given input; it only needs to be a fixed 32 bytes. */
static const uint8_t kFuzzKey[APAD_SESSION_KEY_LEN] = {
    0xA9, 0x66, 0x08, 0x61, 0xD6, 0x11, 0xD4, 0x6A,
    0x19, 0x19, 0x71, 0xEC, 0xCF, 0x0C, 0xC8, 0x95,
    0xEE, 0x7C, 0xD5, 0x80, 0x91, 0xC1, 0x97, 0x3E,
    0xE6, 0xD6, 0x0A, 0x5C, 0x4F, 0x30, 0x42, 0x19,
};

static void fuzz_payload_decoders(const apad_packet *pkt) {
    /* Dispatch to the matching decoder for every type that has one.
     * apad_packet_parse (§3.1 check 6) already guarantees pkt->payload_len
     * equals the type's fixed size, so these calls are exercising the
     * decoders' own field-level logic (D-pad LUT, clamps, scrub), not their
     * length checks -- codec.c's own length check (documented as "requires
     * len == APAD_LEN_<TYPE> exactly") is redundant-but-load-bearing
     * defense in depth and is fuzzed too, since we pass pkt->payload_len
     * verbatim rather than a hardcoded constant. */
    switch (pkt->header.type) {
        case APAD_MSG_ANNOUNCE: {
            apad_announce out;
            (void)apad_decode_announce(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_HELLO: {
            apad_hello out;
            (void)apad_decode_hello(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_WELCOME: {
            apad_welcome out;
            (void)apad_decode_welcome(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_BYE: {
            apad_bye out;
            (void)apad_decode_bye(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_INPUT_STATE: {
            apad_input_state out;
            (void)apad_decode_input_state(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_PING:
        case APAD_MSG_PONG: {
            apad_ping out;
            (void)apad_decode_ping(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_RUMBLE: {
            apad_rumble out;
            (void)apad_decode_rumble(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_LED: {
            apad_led out;
            (void)apad_decode_led(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_STATUS: {
            apad_status out;
            (void)apad_decode_status(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_ACK: {
            apad_ack out;
            (void)apad_decode_ack(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_ERROR: {
            apad_error out;
            (void)apad_decode_error(pkt->payload, pkt->payload_len, &out);
            break;
        }
        case APAD_MSG_DISCOVER:
        default:
            /* DISCOVER has no payload and therefore no codec pair (§6.1). */
            break;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* 1. Header-only decode. apad_header_decode is documented to validate
     * magic and version but not type or length -- exercise it directly on
     * every input, including ones far too short or too long to be a full
     * packet. */
    apad_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    (void)apad_header_decode(data, size, &hdr);

    /* 2. Full ordered §3.1 validation. */
    apad_packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    int parse_rc = apad_packet_parse(data, size, &pkt);

    if (parse_rc >= 0) {
        /* 3. Payload-level decode for the now-known-good-length payload. */
        fuzz_payload_decoders(&pkt);

        /* 4. Tag verification, only when the packet claims to carry one.
         * apad_packet_verify takes the whole datagram (header + payload +
         * tag), not just the parsed payload slice. */
        if ((hdr.flags & APAD_FLAG_AUTHENTICATED) != 0) {
            (void)apad_packet_verify(data, size, kFuzzKey, sizeof(kFuzzKey));
        }
    }

    /* apad_payload_size is a pure lookup (type -> size or APAD_ERR_TYPE);
     * cheap to fuzz on the same byte that drove the type field, if any. */
    if (size > 3) {
        (void)apad_payload_size(data[3]); /* §3 offset 3 = type */
    }

    return 0;
}
