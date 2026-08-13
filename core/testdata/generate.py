#!/usr/bin/env python3
"""
core/testdata/generate.py

Regenerates core/testdata/vectors.h -- byte-exact conformance vectors for the
AtticPad wire protocol.

INDEPENDENCE NOTE (see docs/DESIGN.md S9.1): every byte
pattern and every expected decoded value in this file is derived solely from
docs/PROTOCOL.md. This script does not read, import, or otherwise consult
anything under core/src/, core/include/, server/, clients/, or shim/. If you
are maintaining this script, keep it that way -- the entire point of a
separately-authored conformance vector is that it cannot encode the same
misunderstanding as the implementation it tests.

The one exception: Section F cross-checks the Appendix A authentication
values against Python's own `hashlib`/`hmac` (standard library, not this
project's codec) purely as a transcription sanity check on this script. That
is not a dependency on core/src -- it is independent, well-known,
general-purpose crypto code, the same category of thing a second
implementation would use.

Determinism: this script has no dependency on wall-clock time, PYTHONHASHSEED,
or filesystem iteration order. Running it twice must produce a byte-identical
vectors.h. All vector lists are plain Python lists (not sets/dicts iterated
for ordering) so emission order is stable.

Usage:
    python3 core/testdata/generate.py

REVISION NOTE (this pass): docs/PROTOCOL.md was revised after the previous
pass. Two rulings changed and are reflected throughout this file:

  1. S2 now states "ignored on receive" means SCRUBBED, not passed through:
     a receiver MUST zero every reserved field, every reserved bit, and every
     touch entry at index >= touch_count in its DECODED output. Previously
     this file assumed verbatim passthrough. Section C now asserts zeroed
     reserved fields.
  2. S3.1 now defines a single ordered 7-check accept/discard/reject decision,
     stopping at the first failure, rather than three independent booleans
     (frame-valid / type-known / version-ok). Section A is restructured
     around "which check (if any) is the first to fail" plus the resulting
     action (accept / discard / reject) and ERROR code.

REVISION NOTE (later pass, same milestone): docs/PROTOCOL.md gained a new
S5.5 "Battery normalisation" section, resolving a gap S5 previously left
open (101-254 said "MUST be treated as unknown on receive" without saying
by whom). S5.5 rules it a decode-time normalisation, like the touch_count
and trigger-axis clamps: a decoder MUST turn 101-254 into 255 in its
decoded output. Section C's `normalize_battery()` implements this and new
boundary vectors (100, 101, 173, 254) were added around it.

REVISION NOTE (later pass still, same milestone): Section G (the eleven S6
payloads) was added, then docs/PROTOCOL.md gained a new S6.0 "Out-of-range
values in this section" and an explicit S6.6 rule, both after Section G's
first draft:

  - S6.0 states the mirror image of S6.2/S6.8/S5.5's clamps: BYE.reason,
    STATUS.code and ERROR.code carry no clamp at all -- an unrecognised value
    MUST be preserved verbatim and the packet MUST NOT be rejected for
    carrying one. ERROR code 0 is newly reserved/unassigned and falls under
    the same preserve-don't-reject rule. New vectors cover unrecognised
    values for all three fields, including ERROR code 0.
  - S6.6 now states explicitly that PING/PONG's responder_ticks_ms is
    PRESERVED on decode, not scrubbed, even though S6.6 also says it "MUST be
    zero on send" in a PING -- because PING and PONG share one decoder. This
    was genuinely ambiguous under S2's general scrub rule when
    ping_baseline was first written (S2 would have been an equally
    defensible, and wrong, reading); the vector's choice is now confirmed
    correct by name rather than by inference, and its comment says so.
  - S9 also gained "Duplicates" rules (retransmissions MUST be byte-identical
    including sequence; a receiver MUST re-send its original answer verbatim
    for a duplicate request rather than generate a fresh one; every copy of a
    reliable message MUST be ACKed, not just the first). These are session-
    FSM behaviors involving sequences of packets and time, not single-packet
    payload decode -- out of what a Section A-G vector can express. Flagged
    in the report as not covered here, not overlooked.
"""

import hashlib
import hmac
import os
import struct

SPEC = "docs/PROTOCOL.md"

# ---------------------------------------------------------------------------
# Constants, transcribed from docs/PROTOCOL.md. Nothing here is inferred.
# ---------------------------------------------------------------------------

MAGIC = 0x4D43              # PROTOCOL.md S3: bytes on the wire are 43 4D (LE)
HEADER_LEN = 12              # PROTOCOL.md S3
TAG_LEN = 8                  # PROTOCOL.md S3 / S10
MAX_DATAGRAM = 256           # PROTOCOL.md S1, S11
VERSION_1 = 1                # PROTOCOL.md S3 ("version for this specification is 1")

FLAG_AUTHENTICATED = 1 << 0  # PROTOCOL.md S3
FLAG_RELIABLE = 1 << 1       # PROTOCOL.md S3
# PROTOCOL.md S3: "bits 2-15 reserved". Derived here, not copied from any
# implementation header.
FLAG_KNOWN_MASK = FLAG_AUTHENTICATED | FLAG_RELIABLE

TYPE_DISCOVER = 0x01
TYPE_ANNOUNCE = 0x02
TYPE_HELLO = 0x10
TYPE_WELCOME = 0x11
TYPE_BYE = 0x12
TYPE_INPUT_STATE = 0x20
TYPE_PING = 0x30
TYPE_PONG = 0x31
TYPE_RUMBLE = 0x40
TYPE_LED = 0x41
TYPE_STATUS = 0x42
TYPE_ACK = 0x50
TYPE_ERROR = 0x51

KNOWN_TYPES = [
    TYPE_DISCOVER, TYPE_ANNOUNCE, TYPE_HELLO, TYPE_WELCOME, TYPE_BYE,
    TYPE_INPUT_STATE, TYPE_PING, TYPE_PONG, TYPE_RUMBLE, TYPE_LED,
    TYPE_STATUS, TYPE_ACK, TYPE_ERROR,
]

TYPE_SIZE = {  # PROTOCOL.md S4 table, "Payload" column
    TYPE_DISCOVER: 0, TYPE_ANNOUNCE: 40, TYPE_HELLO: 76, TYPE_WELCOME: 60,
    TYPE_BYE: 4, TYPE_INPUT_STATE: 56, TYPE_PING: 8, TYPE_PONG: 8,
    TYPE_RUMBLE: 8, TYPE_LED: 4, TYPE_STATUS: 64, TYPE_ACK: 4, TYPE_ERROR: 64,
}

# A type code deliberately absent from PROTOCOL.md S4's table.
TYPE_UNKNOWN_A = 0x99
TYPE_UNKNOWN_B = 0xAB

INPUT_STATE_LEN = 56          # PROTOCOL.md S5 ("Total 56 bytes")
DISCOVER_LEN = 0              # PROTOCOL.md S6.1
ANNOUNCE_LEN = 40             # PROTOCOL.md S6.2
HELLO_LEN = 76                # PROTOCOL.md S6.3
WELCOME_LEN = 60              # PROTOCOL.md S6.4
BYE_LEN = 4                   # PROTOCOL.md S6.5
PING_LEN = 8                  # PROTOCOL.md S6.6
PONG_LEN = 8                  # PROTOCOL.md S6.6 (same layout as PING)
RUMBLE_LEN = 8                # PROTOCOL.md S6.7
LED_LEN = 4                   # PROTOCOL.md S6.8
STATUS_LEN = 64               # PROTOCOL.md S6.9
ACK_LEN = 4                   # PROTOCOL.md S6.10
ERROR_LEN = 64                # PROTOCOL.md S6.11

# Fixed-width field sizes used by S6 payloads. Derived directly from the S6.x
# tables, not copied from any implementation header.
NAME_LEN = 32          # S6.2 server_name, S6.3 device_name
TEXT_LEN = 60           # S6.9 STATUS.text, S6.11 ERROR.text
CLIENT_ID_LEN = 16      # S6.3 client_id
NONCE_LEN = 16          # S6.3 client_nonce, S6.4 server_nonce
KEY_MATERIAL_LEN = 32   # S6.4 key_material

# S6.3: "bits 14..31 reserved, MUST be zero". Bits 0..13 are the valid caps
# range; derived here as a plain bit computation.
CAPS_VALID_MASK = (1 << 14) - 1

CAP_DPAD = 1 << 0
CAP_FACE4 = 1 << 1
CAP_SHOULDER = 1 << 2
CAP_SHOULDER2 = 1 << 3
CAP_TRIGGERS = 1 << 4
CAP_STICK_L = 1 << 5
CAP_STICK_R = 1 << 6
CAP_TOUCH = 1 << 7
CAP_TOUCH_REAR = 1 << 8
CAP_ACCEL = 1 << 9
CAP_GYRO = 1 << 10
CAP_RUMBLE = 1 << 11
CAP_LED = 1 << 12
CAP_BATTERY = 1 << 13

# PROTOCOL.md S5.1, apad_hat_lut -- copied verbatim, byte for byte.
HAT_LUT = [8, 0, 4, 8, 6, 7, 5, 6, 2, 1, 3, 2, 8, 0, 4, 8]

# Button bit positions, PROTOCOL.md S5.1.
BTN_A = 1 << 0
BTN_B = 1 << 1
BTN_X = 1 << 2
BTN_Y = 1 << 3
BTN_DPAD_UP = 1 << 4
BTN_DPAD_DOWN = 1 << 5
BTN_DPAD_LEFT = 1 << 6
BTN_DPAD_RIGHT = 1 << 7
BTN_L = 1 << 8
BTN_R = 1 << 9
BTN_ZL = 1 << 10
BTN_ZR = 1 << 11
BTN_L3 = 1 << 12
BTN_R3 = 1 << 13
BTN_START = 1 << 14
BTN_SELECT = 1 << 15
BTN_HOME = 1 << 16
BTN_TOUCH_PRESS = 1 << 17
BTN_TOUCH_REAR_PRESS = 1 << 18
BTN_CAPTURE = 1 << 19
# PROTOCOL.md S5.1: "20..31 reserved (MUST be zero)". Bits 0..19 are the
# valid range; derived here as a plain bit computation, not copied from any
# implementation header.
BTN_VALID_MASK = (1 << 20) - 1

# PROTOCOL.md S3.1 check numbers, and the action/ERROR-code each failure maps
# to per the S3.1 table ("Discard" = drop silently, "Reject" = drop and MAY
# send ERROR with the stated code).
CHECK_LENGTH_MIN = 1     # datagram length >= 12           -> discard
CHECK_MAGIC = 2           # magic == 0x4D43                 -> discard
CHECK_VERSION = 3         # version == 1                    -> reject, code 1
CHECK_LENGTH_FORMULA = 4  # actual len == 12+payload_len+tag -> reject, code 6
CHECK_TYPE = 5             # type known                      -> discard
CHECK_PAYLOAD_SIZE = 6     # payload_len == fixed size       -> reject, code 6
CHECK_TAG = 7              # tag verifies (AUTHENTICATED)    -> reject, code 3

ACTION_ACCEPT = 0
ACTION_DISCARD = 1
ACTION_REJECT = 2

ACTION_FOR_CHECK = {
    0: ACTION_ACCEPT,
    CHECK_LENGTH_MIN: ACTION_DISCARD,
    CHECK_MAGIC: ACTION_DISCARD,
    CHECK_VERSION: ACTION_REJECT,
    CHECK_LENGTH_FORMULA: ACTION_REJECT,
    CHECK_TYPE: ACTION_DISCARD,
    CHECK_PAYLOAD_SIZE: ACTION_REJECT,
    CHECK_TAG: ACTION_REJECT,
}

ERROR_CODE_FOR_CHECK = {  # PROTOCOL.md S3.1 "On failure" column, S6.11 codes
    0: 0,
    CHECK_LENGTH_MIN: 0,       # silent discard, no ERROR code
    CHECK_MAGIC: 0,            # silent discard, no ERROR code
    CHECK_VERSION: 1,          # "version mismatch"
    CHECK_LENGTH_FORMULA: 6,   # "malformed packet"
    CHECK_TYPE: 0,             # silent discard, no ERROR code
    CHECK_PAYLOAD_SIZE: 6,     # "malformed packet"
    CHECK_TAG: 3,              # "authentication failed"
}


def u16(v):
    return v & 0xFFFF


def u32(v):
    return v & 0xFFFFFFFF


def s16_to_bytes(v):
    return struct.pack('<h', v)


# ---------------------------------------------------------------------------
# Packet builders
# ---------------------------------------------------------------------------

def build_header(version, type_, session_id, sequence, payload_len, flags):
    """PROTOCOL.md S3, 12-byte header, little-endian, fixed offsets."""
    return struct.pack('<HBBHHHH', MAGIC, version & 0xFF, type_ & 0xFF,
                        u16(session_id), u16(sequence), u16(payload_len),
                        u16(flags))


def build_input_state_payload(buttons, axes, touch_count, reserved0,
                               touches, accel, gyro, battery, reserved1,
                               reserved2, client_ticks_ms):
    """PROTOCOL.md S5, 56-byte INPUT_STATE payload.

    axes: list of 8 signed 16-bit ints (indices 0..7)
    touches: list of 2 tuples (id, pressure, x, y)
    accel, gyro: lists of 3 signed 16-bit ints
    reserved1: list of 3 bytes; reserved2: list of 2 bytes
    """
    assert len(axes) == 8
    assert len(touches) == 2
    assert len(accel) == 3 and len(gyro) == 3
    assert len(reserved1) == 3 and len(reserved2) == 2

    out = b''
    out += struct.pack('<I', u32(buttons))
    for a in axes:
        out += s16_to_bytes(a)
    out += struct.pack('<BB', touch_count & 0xFF, reserved0 & 0xFF)
    for (tid, pressure, x, y) in touches:
        out += struct.pack('<BBhh', tid & 0xFF, pressure & 0xFF, x, y)
    for a in accel:
        out += s16_to_bytes(a)
    for g in gyro:
        out += s16_to_bytes(g)
    out += struct.pack('<B', battery & 0xFF)
    out += bytes([b & 0xFF for b in reserved1])
    out += bytes([b & 0xFF for b in reserved2])
    out += struct.pack('<I', u32(client_ticks_ms))
    assert len(out) == INPUT_STATE_LEN, len(out)
    return out


def clamp_touch_count(raw):
    """PROTOCOL.md S5: "Values > 2 MUST be clamped to 2 on receive"."""
    return 2 if raw > 2 else raw


def clamp_trigger_axis(raw):
    """PROTOCOL.md S5: "Negative values in axes[4]/axes[5] MUST be treated
    as 0 on receive"."""
    return 0 if raw < 0 else raw


def normalize_battery(raw):
    """PROTOCOL.md S5.5 (new section): 0-100 is a percentage, 255 is the
    unknown sentinel, and 101-254 is reserved. "a decoder MUST normalise
    them to 255 in its decoded output" -- a decode-time normalisation, the
    same category as the touch_count clamp, the trigger clamp, and the S2
    reserved-bit scrub."""
    raw &= 0xFF
    return 255 if 101 <= raw <= 254 else raw


def hat_for(buttons):
    """PROTOCOL.md S5.1: (buttons >> 4) & 0xF indexes apad_hat_lut."""
    return HAT_LUT[(buttons >> 4) & 0xF]


def rand_filler(n, start=0):
    """Deterministic non-repeating filler bytes, for payloads whose exact
    content is irrelevant to the vector (framing-only tests)."""
    return bytes((i * 37 + 1 + start) & 0xFF for i in range(n))


def normalize_pairing_required(raw):
    """PROTOCOL.md S6.2 (new rule, commit e467fd8): "pairing_required -- 0 or
    1; any non-zero value MUST be read as 1"."""
    return 0 if (raw & 0xFF) == 0 else 1


def normalize_led_player_index(raw):
    """PROTOCOL.md S6.8: "player_index -- 1-4, 0 = off" and "Values above 4
    are reserved and MUST be treated as 0 (off) on receive." 0 is a valid,
    non-reserved value (it means "off") and MUST NOT be touched by this
    rule; only values > 4 are normalised."""
    raw &= 0xFF
    return 0 if raw > 4 else raw


def text_field_padded(s_bytes, width):
    """PROTOCOL.md S2: "UTF-8, NUL-padded to their fixed width"."""
    assert len(s_bytes) <= width
    return s_bytes + bytes(width - len(s_bytes))


def text_field_full_no_nul(width, seed=0):
    """PROTOCOL.md S2: "need not be NUL-terminated when they fill the
    field." Exactly `width` distinctive printable-ASCII bytes, no NUL
    anywhere -- the field is entirely full."""
    return bytes(65 + ((i * 7 + seed) % 26) for i in range(width))  # 'A'..'Z' cycling


def text_field_split_utf8(width, seed=0):
    """PROTOCOL.md S2: "A sender MUST NOT split a multi-byte UTF-8 sequence
    across the end of the field ... A receiver MUST tolerate a malformed
    trailing sequence rather than reject the packet." This constructs
    exactly the sender-non-conforming case a receiver MUST tolerate: the
    lead byte of a 2-byte UTF-8 sequence (0xC3, e.g. the lead byte of any of
    U+00C0-U+00FF) placed as the very LAST byte of the field, with no room
    left for its continuation byte or for a NUL terminator."""
    filler = bytes(65 + ((i * 5 + seed) % 26) for i in range(width - 1))
    return filler + bytes([0xC3])


# ---------------------------------------------------------------------------
# Section A: header / framing vectors
#
# PROTOCOL.md S3.1 (new in this revision) replaces the previous three
# independent booleans (frame-valid / type-known / version-ok) with a single
# ORDERED decision: seven checks, applied in order, stopping at the first
# failure. Each vector below records exp_failed_check -- 0 if every check
# passes, else the 1-based number of the first check (per the S3.1 table)
# that fails -- from which the expected action (accept/discard/reject) and
# ERROR code follow mechanically (see ACTION_FOR_CHECK / ERROR_CODE_FOR_CHECK
# above).
#
# Header fields (version/type/session_id/sequence/payload_len/flags) are
# only asserted when exp_failed_check == 0 (fully accepted). This is a
# deliberate simplification, not a claim that a real decoder can't read the
# header in other cases -- see the report for why: core/include/atticpad.h
# (which this generator does not treat as authoritative, only docs/PROTOCOL.md
# is) happens to split "parse" (checks 1-6) from "verify" (check 7, needs a
# session key) into two functions, which is one reasonable way to structure
# check 7 given it requires key material a pure framing vector doesn't carry.
# Vectors here therefore only ever set exp_failed_check in 0..6; check 7 (tag
# verification) is exercised in Section F, which does carry real key
# material from Appendix A.
# ---------------------------------------------------------------------------

frame_vectors = []  # list of dicts


def add_frame_vector(name, spec_ref, byte_str, exp_failed_check,
                      exp_version=None, exp_type=None, exp_session_id=None,
                      exp_sequence=None, exp_payload_len=None,
                      exp_flags=None, logical_len=None):
    frame_vectors.append(dict(
        name=name, spec_ref=spec_ref, bytes=byte_str,
        logical_len=logical_len if logical_len is not None else len(byte_str),
        exp_failed_check=exp_failed_check,
        exp_action=ACTION_FOR_CHECK[exp_failed_check],
        exp_error_code=ERROR_CODE_FOR_CHECK[exp_failed_check],
        exp_version=exp_version, exp_type=exp_type,
        exp_session_id=exp_session_id, exp_sequence=exp_sequence,
        exp_payload_len=exp_payload_len, exp_flags=exp_flags,
    ))


# A canonical, fully valid INPUT_STATE datagram reused by several vectors
# below (framing tests and the truncation sweep). Distinctive, non-repeating
# field values everywhere so an offset bug cannot hide behind a coincidence.
_baseline_axes = [1000, -2000, 3000, -4000, 5000, 6000, 0, 0]
_baseline_touches = [(1, 200, 11111, -11111), (2, 201, -22222, 22222)]
_baseline_accel = [111, -222, 333]
_baseline_gyro = [-444, 555, -666]
_baseline_buttons = (BTN_A | BTN_X | BTN_DPAD_UP | BTN_L | BTN_START)

CANONICAL_PAYLOAD = build_input_state_payload(
    buttons=_baseline_buttons,
    axes=_baseline_axes,
    touch_count=2,
    reserved0=0,
    touches=_baseline_touches,
    accel=_baseline_accel,
    gyro=_baseline_gyro,
    battery=42,
    reserved1=[0, 0, 0],
    reserved2=[0, 0],
    client_ticks_ms=0x12345678,
)
CANONICAL_HEADER = build_header(VERSION_1, TYPE_INPUT_STATE,
                                 session_id=1, sequence=100,
                                 payload_len=INPUT_STATE_LEN, flags=0)
CANONICAL_DATAGRAM = CANONICAL_HEADER + CANONICAL_PAYLOAD
assert len(CANONICAL_DATAGRAM) == HEADER_LEN + INPUT_STATE_LEN  # 68

# ---- Accept (exp_failed_check == 0): every check passes ----

add_frame_vector(
    "valid_input_state_frame", "S3, S3.1, S4, S5",
    CANONICAL_DATAGRAM, 0,
    exp_version=VERSION_1, exp_type=TYPE_INPUT_STATE, exp_session_id=1,
    exp_sequence=100, exp_payload_len=INPUT_STATE_LEN, exp_flags=0,
)

# Zero-length payload: DISCOVER is explicitly specified as 0 payload bytes
# (S6.1) and session_id MUST be 0.
discover_header = build_header(VERSION_1, TYPE_DISCOVER, session_id=0,
                                sequence=7, payload_len=0, flags=0)
add_frame_vector(
    "zero_length_payload_discover", "S3.1, S4, S6.1 (S13 zero-length payload)",
    discover_header, 0,
    exp_version=VERSION_1, exp_type=TYPE_DISCOVER, exp_session_id=0,
    exp_sequence=7, exp_payload_len=0, exp_flags=0,
)

# Reserved header flag bits (S3: "bits 2-15 reserved") MUST be scrubbed to
# zero on decode (S2), not rejected. Bit 1 (RELIABLE) is set too, to show the
# scrub is selective: only the reserved bits are cleared, known bits survive.
# (Whether RELIABLE is a sensible flag for INPUT_STATE is a session-layer
# concern (S9), not a framing-layer one; none of the seven S3.1 checks
# inspect flags beyond bit 0, so this is purely a framing/scrub vector.)
_reserved_flags_header = build_header(VERSION_1, TYPE_INPUT_STATE,
                                       session_id=8, sequence=55,
                                       payload_len=INPUT_STATE_LEN,
                                       flags=0xFFFE)
add_frame_vector(
    "reserved_header_flags_bits_scrubbed_accepted",
    "S2 (scrub, not reject), S3 flags bits 2-15 reserved",
    _reserved_flags_header + CANONICAL_PAYLOAD, 0,
    exp_version=VERSION_1, exp_type=TYPE_INPUT_STATE, exp_session_id=8,
    exp_sequence=55, exp_payload_len=INPUT_STATE_LEN,
    exp_flags=(0xFFFE & FLAG_KNOWN_MASK),
)

# Appendix A's authenticated PING datagram, byte for byte (S14). At the
# framing/parse layer (checks 1-6) it is fully accepted: length arithmetic
# includes the +8 tag per S3 with AUTHENTICATED set, type PING is known,
# payload_len (8) matches PING's fixed size. Tag CORRECTNESS (check 7) is
# exercised in Section F, which has the key; see the note above the section.
APPENDIX_A_PIN = b"123456"                     # S14: six ASCII digits, no NUL
APPENDIX_A_SALT = bytes(range(16))             # S14 server_nonce
APPENDIX_A_ITERS = 10000                       # S14
APPENDIX_A_KEY = bytes.fromhex(
    "A9660861D611D46A191971ECCF0CC895EE7CD58091C1973EE6D60A5C4F304219"
)  # S14 derived session key
assert len(APPENDIX_A_KEY) == 32
APPENDIX_A_HEADER = bytes.fromhex("434D0130010002000800 0100".replace(" ", ""))
APPENDIX_A_PAYLOAD = bytes.fromhex("E803000000000000")  # origin_ticks_ms=1000, responder=0
APPENDIX_A_TAG = bytes.fromhex("F84CBAC6CE34B1AE")      # S14 truncated tag
assert len(APPENDIX_A_HEADER) == HEADER_LEN
assert len(APPENDIX_A_PAYLOAD) == PING_LEN
assert len(APPENDIX_A_TAG) == TAG_LEN
APPENDIX_A_DATAGRAM = APPENDIX_A_HEADER + APPENDIX_A_PAYLOAD + APPENDIX_A_TAG
assert len(APPENDIX_A_DATAGRAM) == HEADER_LEN + PING_LEN + TAG_LEN  # 28

add_frame_vector(
    "authenticated_appendix_a_ping_frame_ok",
    "S3, S3.1 checks 1-6, S14 (tag itself verified in Section F)",
    APPENDIX_A_DATAGRAM, 0,
    exp_version=VERSION_1, exp_type=TYPE_PING, exp_session_id=1,
    exp_sequence=2, exp_payload_len=PING_LEN, exp_flags=FLAG_AUTHENTICATED,
)

# ---- Check 5 (type known) fails, after check 4 (length formula) passes ----
#
# The framing ceiling: the largest payload_len for which the total datagram
# still fits the 256-byte cap (S1, S11) with no AUTHENTICATED tag:
# 256 - 12 = 244. No defined v1 type documents a payload this large (HELLO at
# 76 bytes is the largest named type, S4). Under the new ordered validation,
# an unknown type is caught by check 5 -- this vector demonstrates check 4's
# length arithmetic is satisfied exactly at the 244-byte ceiling before
# check 5 discards it for the unknown type. It intentionally does NOT reach
# "accepted", because no v1 type is 244 bytes; see the next vector for a
# variant that reaches check 6 instead.
_max_payload_len = MAX_DATAGRAM - HEADER_LEN
assert _max_payload_len == 244
max_len_header = build_header(VERSION_1, TYPE_UNKNOWN_A, session_id=5,
                               sequence=9, payload_len=_max_payload_len,
                               flags=0)
max_len_payload = rand_filler(_max_payload_len)
max_len_datagram = max_len_header + max_len_payload
assert len(max_len_datagram) == MAX_DATAGRAM
add_frame_vector(
    "max_length_payload_244_unknown_type",
    "S1, S3.1 check 4 passes then check 5 fails, S11 (S13 maximum-length payload)",
    max_len_datagram, CHECK_TYPE,
)

# ---- Check 6 (payload_len == fixed size for type) fails ----
#
# Same 244-byte framing ceiling, but with a KNOWN type (INPUT_STATE) whose
# declared payload_len (244) disagrees with its fixed size (56). Check 4
# passes (the datagram is internally length-consistent), check 5 passes
# (type is known), check 6 fails. This is the "check 6 is load-bearing, not
# pedantry" scenario from S3.1 at the framing ceiling.
max_len_known_type_header = build_header(VERSION_1, TYPE_INPUT_STATE,
                                          session_id=5, sequence=9,
                                          payload_len=_max_payload_len,
                                          flags=0)
max_len_known_type_datagram = max_len_known_type_header + rand_filler(_max_payload_len, start=3)
assert len(max_len_known_type_datagram) == MAX_DATAGRAM
add_frame_vector(
    "max_length_payload_244_known_type_size_mismatch",
    "S3.1 check 6 (S13 maximum-length payload + payload_len-vs-type-size)",
    max_len_known_type_datagram, CHECK_PAYLOAD_SIZE,
)

# Check 6 alone, at an ordinary (non-ceiling) size: INPUT_STATE declares
# payload_len=55 (one less than its fixed 56), and the datagram is exactly
# 12+55=67 bytes -- internally consistent under check 4, wrong for the type
# under check 6.
_short_declared_len = INPUT_STATE_LEN - 1
payload_len_mismatch_header = build_header(VERSION_1, TYPE_INPUT_STATE,
                                            session_id=1, sequence=100,
                                            payload_len=_short_declared_len,
                                            flags=0)
payload_len_mismatch_datagram = payload_len_mismatch_header + CANONICAL_PAYLOAD[:_short_declared_len]
assert len(payload_len_mismatch_datagram) == HEADER_LEN + _short_declared_len
add_frame_vector(
    "payload_len_disagrees_with_type_size",
    "S3.1 check 6 (S13 payload_len disagreeing with the type's fixed size)",
    payload_len_mismatch_datagram, CHECK_PAYLOAD_SIZE,
)

# ---- Check 4 (length formula) fails ----

add_frame_vector(
    "length_mismatch_too_short_by_one", "S3.1 check 4 (S13 length mismatch)",
    CANONICAL_DATAGRAM[:-1], CHECK_LENGTH_FORMULA,
)
add_frame_vector(
    "length_mismatch_too_long_by_one", "S3.1 check 4 (S13 length mismatch)",
    CANONICAL_DATAGRAM + b'\x00', CHECK_LENGTH_FORMULA,
)
# The AUTHENTICATED ("+8") arm of check 4: Appendix A's datagram, one byte
# short of the full 28 (12 header + 8 payload + 8 tag).
add_frame_vector(
    "authenticated_flag_length_short_by_one",
    "S3.1 check 4, AUTHENTICATED arm (S13 length mismatch)",
    APPENDIX_A_DATAGRAM[:-1], CHECK_LENGTH_FORMULA,
)

# ---- Check 5 (type known) fails, ordinary size ----

unknown_type_payload = build_input_state_payload(
    buttons=0, axes=[0] * 8, touch_count=0, reserved0=0,
    touches=[(0, 0, 0, 0), (0, 0, 0, 0)], accel=[0, 0, 0], gyro=[0, 0, 0],
    battery=0, reserved1=[0, 0, 0], reserved2=[0, 0], client_ticks_ms=0,
)
unknown_type_header = build_header(VERSION_1, TYPE_UNKNOWN_B, session_id=0,
                                    sequence=1,
                                    payload_len=len(unknown_type_payload),
                                    flags=0)
add_frame_vector(
    "unknown_type_discarded", "S3.1 check 5, S4",
    unknown_type_header + unknown_type_payload, CHECK_TYPE,
)

# ---- Check 3 (version) fails ----

for bad_version, tag in ((0, "zero"), (2, "two")):
    hdr = build_header(bad_version, TYPE_INPUT_STATE, session_id=1,
                        sequence=100, payload_len=INPUT_STATE_LEN, flags=0)
    add_frame_vector(
        "version_mismatch_%s" % tag, "S3.1 check 3, S12",
        hdr + CANONICAL_PAYLOAD, CHECK_VERSION,
    )

# ---- Check 2 (magic) fails ----

_bad_magic_datagram = bytearray(CANONICAL_DATAGRAM)
_bad_magic_datagram[0:2] = b'\xFF\xFF'
add_frame_vector(
    "magic_mismatch", "S3.1 check 2 (S13 magic mismatch)",
    bytes(_bad_magic_datagram), CHECK_MAGIC,
)

# ---- Check 1 (datagram length >= 12) fails ----

add_frame_vector(
    "datagram_shorter_than_header_5_bytes",
    "S3.1 check 1 (S13 datagram shorter than 12 bytes)",
    bytes([0x43, 0x4D, 0x01, 0x20, 0x00]), CHECK_LENGTH_MIN,
)
# The empty UDP datagram: recvfrom() can legally return 0 bytes. The single
# placeholder byte below is never read (logical_len=0) -- it exists only
# because a zero-length array initializer is not portable C99.
add_frame_vector(
    "datagram_zero_length",
    "S3.1 check 1 (S13 datagram shorter than 12 bytes, the 0-byte case)",
    bytes([0x00]), CHECK_LENGTH_MIN, logical_len=0,
)

# ---------------------------------------------------------------------------
# Section B: truncation sweep, "at every byte offset" (S13)
#
# The canonical 68-byte INPUT_STATE datagram, truncated to every length from
# 0 to 67. For any trunc_len < 12, check 1 fails (header not even readable).
# For any trunc_len in [12, 67], the first 12 bytes (magic, version, ...) are
# untouched -- only the tail is missing -- so checks 1-3 pass and check 4
# (actual length == 12 + payload_len + tag) fails, since the actual length
# never equals the required 68 for any length in this range.
# ---------------------------------------------------------------------------

def failed_check_for_truncation(trunc_len, full_len, payload_len, tag_present):
    if trunc_len < HEADER_LEN:
        return CHECK_LENGTH_MIN
    required = HEADER_LEN + payload_len + (TAG_LEN if tag_present else 0)
    if trunc_len != required:
        return CHECK_LENGTH_FORMULA
    return 0  # trunc_len == full_len: this is the untruncated datagram


truncation_vectors = []
for trunc_len in range(0, len(CANONICAL_DATAGRAM)):
    truncation_vectors.append(dict(
        name="trunc_%02d" % trunc_len,
        trunc_len=trunc_len,
        exp_failed_check=failed_check_for_truncation(
            trunc_len, len(CANONICAL_DATAGRAM), INPUT_STATE_LEN, False),
    ))

# Second sweep: Appendix A's 28-byte AUTHENTICATED datagram, truncated at
# every offset. Exercises the "+8" arm of check 4 at every byte boundary,
# including inside the tag itself.
auth_truncation_vectors = []
for trunc_len in range(0, len(APPENDIX_A_DATAGRAM)):
    auth_truncation_vectors.append(dict(
        name="auth_trunc_%02d" % trunc_len,
        trunc_len=trunc_len,
        exp_failed_check=failed_check_for_truncation(
            trunc_len, len(APPENDIX_A_DATAGRAM), PING_LEN, True),
    ))

# ---------------------------------------------------------------------------
# Section C: INPUT_STATE payload decode vectors (S5)
#
# Decoding rule adopted throughout, per the revised S2: every field decodes
# as the literal bytes sent EXCEPT for fields the spec marks reserved or
# out-of-band, which a receiver MUST SCRUB to zero on decode:
#   - buttons bits 20..31 (S5.1: reserved, MUST be zero)              -> 0
#   - axes[6] / axes[7]   (S5: reserved, MUST be zero)                -> 0
#   - reserved0           (S5 offset 21)                              -> 0
#   - touch entries at index >= touch_count (post-clamp) (S5.2)       -> zeroed
# plus the explicit clamps/normalisations stated in S5 and S5.5 (not scrubs,
# transformations of an in-range-but-not-canonical value):
#   - touch_count > 2 clamped to 2
#   - axes[4] / axes[5] negative clamped to 0
#   - battery 101-254 normalised to 255 (S5.5, new section)
# reserved1[3] and reserved2[2] are also reserved bytes that MUST scrub to
# zero on decode, but they are pure padding with no meaning to assert beyond
# "the codec must zero them"; not modelled as separate exp_ fields here since
# nothing consumes them, matching how a real decoded apad_input_state would
# still show them as zeroed bytes but this vector format doesn't need to
# name that separately from the buttons/axes/touch scrubs it does assert.
# ---------------------------------------------------------------------------

input_state_vectors = []


def add_input_state_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_INPUT_STATE,
                            session_id=1, sequence=100,
                            payload_len=INPUT_STATE_LEN, flags=0)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(
        buttons=_baseline_buttons, axes=list(_baseline_axes),
        touch_count=2, reserved0=0, touches=list(_baseline_touches),
        accel=list(_baseline_accel), gyro=list(_baseline_gyro),
        battery=42, reserved1=[0, 0, 0], reserved2=[0, 0],
        client_ticks_ms=0x12345678,
    )
    payload_defaults.update(payload_kwargs)

    packet = build_header(**header_defaults) + build_input_state_payload(**payload_defaults)

    exp_touch_count = clamp_touch_count(payload_defaults['touch_count'])
    exp_axes = list(payload_defaults['axes'])
    exp_axes[4] = clamp_trigger_axis(exp_axes[4])
    exp_axes[5] = clamp_trigger_axis(exp_axes[5])
    exp_axes[6] = 0  # S5: reserved, scrubbed on decode (S2)
    exp_axes[7] = 0  # S5: reserved, scrubbed on decode (S2)
    exp_buttons = u32(payload_defaults['buttons']) & BTN_VALID_MASK  # S5.1 bits 20..31 scrubbed

    raw_touches = payload_defaults['touches']
    exp_touches = []
    for i, t in enumerate(raw_touches):
        if i < exp_touch_count:
            exp_touches.append((t[0] & 0xFF, t[1] & 0xFF, t[2], t[3]))
        else:
            exp_touches.append((0, 0, 0, 0))  # S5.2 + S2: scrubbed, index >= touch_count

    input_state_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_header_sequence=u16(header_defaults['sequence']),
        exp_buttons=exp_buttons,
        exp_axes=exp_axes,
        exp_touch_count=exp_touch_count,
        exp_reserved0=0,  # S5 offset 21: reserved, scrubbed on decode (S2)
        exp_touches=exp_touches,
        exp_accel=list(payload_defaults['accel']),
        exp_gyro=list(payload_defaults['gyro']),
        exp_battery=normalize_battery(payload_defaults['battery']),  # S5.5
        exp_client_ticks_ms=u32(payload_defaults['client_ticks_ms']),
        exp_hat=hat_for(exp_buttons),
    ))


# 1. Baseline: sensible, fully distinctive values in every field.
add_input_state_vector("baseline_valid", "S5", {}, {})

# 2. All 16 hat-LUT indices (S5.1), including up+down (0b0011) and
#    left+right (0b1100). Isolate the D-pad nibble; leave everything else
#    at the baseline payload to also confirm hat computation doesn't
#    interact with unrelated fields.
DPAD_BITS = [BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT]
for nibble in range(16):
    buttons = 0
    for bit_i in range(4):
        if nibble & (1 << bit_i):
            buttons |= DPAD_BITS[bit_i]
    add_input_state_vector(
        "hat_lut_index_%02d" % nibble, "S5.1 (S13 all 16 hat-LUT indices)",
        {}, dict(buttons=buttons),
    )

# 3. touch_count > 2 clamped to 2 (S5). Both touch slots carry real,
#    non-zero data so the clamp is visible against a populated struct.
add_input_state_vector(
    "touch_count_clamped_above_2", "S5 (S13 touch_count > 2 clamped)",
    {}, dict(touch_count=250),
)

# 4. Touch entries at index >= touch_count: sender obligation is to zero
#    them (S5.2), but this vector sends deliberate non-zero garbage in the
#    unused slot from a non-conforming sender, to pin down receive-side
#    decode behaviour. Per the revised S2, the receiver MUST scrub this to
#    zero -- this is the vector that FLIPPED in this revision (previously
#    asserted verbatim passthrough of the garbage).
add_input_state_vector(
    "touch_ignored_slot_nonzero_on_wire", "S5.2, S2 (scrub, not passthrough)",
    {}, dict(touch_count=1, touches=[(9, 250, 30000, -30000), (77, 88, -12345, 6789)]),
)

# 5. battery = 255 (unknown sentinel), S5. Also the top boundary of S5.5's
#    normalisation range: 255 is already the sentinel, so it MUST stay 255,
#    not an accidental no-op of the 101-254 rule.
add_input_state_vector(
    "battery_unknown_255", "S5, S5.5 (S13 battery=255)",
    {}, dict(battery=255),
)

# 5a-5e. S5.5 battery normalisation boundaries (new spec section). A decoder
# MUST normalise 101-254 to 255 in its DECODED output -- this is a
# decode-time normalisation, the same category as the touch_count clamp,
# the trigger clamp, and the S2 reserved-bit scrub. The boundaries are what
# matter: 100 is the top of the real percentage range and MUST pass through
# unchanged; 101 is the first reserved value and MUST become 255; 254 is the
# last reserved value and MUST become 255; 255 (covered above) is already
# the sentinel and MUST stay 255. One midrange value (173) is added for
# coverage of the interior of the reserved range, not just its edges.
add_input_state_vector(
    "battery_100_top_of_real_range_unchanged", "S5.5 (S13 battery normalisation boundary)",
    {}, dict(battery=100),
)
add_input_state_vector(
    "battery_101_first_reserved_normalized", "S5.5 (S13 battery normalisation boundary)",
    {}, dict(battery=101),
)
add_input_state_vector(
    "battery_173_midrange_reserved_normalized", "S5.5 (S13 battery normalisation boundary)",
    {}, dict(battery=173),
)
add_input_state_vector(
    "battery_254_last_reserved_normalized", "S5.5 (S13 battery normalisation boundary)",
    {}, dict(battery=254),
)

# 6. axes[4]/axes[5] negative clamped to 0 on receive (S5). axes[1] is also
#    driven very negative here to show the clamp is specific to the trigger
#    axes and NOT applied to the stick axes.
add_input_state_vector(
    "axes_triggers_negative_clamped", "S5 (S13 axes[4]/axes[5] negative clamp)",
    {}, dict(axes=[100, -32768, -200, 32767, -1, -32768, 0, 0]),
)

# 7. Reserved payload bits/bytes set by a non-conforming sender: buttons bits
#    20-31, reserved0, axes[6]/axes[7], reserved1[3], reserved2[2] all set to
#    nonzero garbage on the wire. Per the revised S2, ALL of these MUST decode
#    as zero -- this is the other vector that FLIPPED in this revision
#    (previously asserted verbatim passthrough). Header-level reserved-flag
#    scrubbing is covered separately by the Section A
#    "reserved_header_flags_bits_scrubbed_accepted" vector, to keep the two
#    layers (header vs. payload) independently testable.
_reserved_buttons = _baseline_buttons | 0xFFF00000  # set all of bits 20..31
add_input_state_vector(
    "reserved_bits_and_bytes_set_ignored",
    "S2 (scrub on decode, not reject and not passthrough)",
    {},
    dict(buttons=_reserved_buttons, reserved0=0xFF,
         axes=[100, -200, 300, -400, 500, 600, 0x7AAA, -0x3BBB],
         reserved1=[0xFF, 0xEE, 0xDD], reserved2=[0xCC, 0xBB]),
)

# 8/9. Raw sequence field decode at the wrap boundary values themselves
# (0xFFFF and 0x0000). Decoding is a flat field read -- no wrap arithmetic
# is applied at decode time (S9's wrap-safe comparisons are a separate,
# pure-function concern, covered in Section D/E below).
add_input_state_vector(
    "header_sequence_raw_0xFFFF", "S3, S9 (S13 sequence wrap across 0xFFFF)",
    dict(sequence=0xFFFF), {},
)
add_input_state_vector(
    "header_sequence_raw_0x0000", "S3, S9 (S13 sequence wrap across 0xFFFF)",
    dict(sequence=0x0000), {},
)

# 10/11. client_ticks_ms at the tick-wrap boundary values (S13 "tick wrap
# across 2^32"). As with sequence, decode is a flat u32 read; the wrap-safe
# comparison itself is exercised by the apad_time_after vectors below.
add_input_state_vector(
    "client_ticks_ms_min_0", "S5, S9 (S13 tick wrap across 2^32)",
    {}, dict(client_ticks_ms=0x00000000),
)
add_input_state_vector(
    "client_ticks_ms_max_0xFFFFFFFF", "S5, S9 (S13 tick wrap across 2^32)",
    {}, dict(client_ticks_ms=0xFFFFFFFF),
)

# ---------------------------------------------------------------------------
# Section D/E: wrap-safe arithmetic (S9). Both functions are given as
# normative C in PROTOCOL.md S9 -- these vectors are literal evaluations of
# that normative code at chosen (a, b) pairs, not an independent derivation.
# ---------------------------------------------------------------------------

def apad_seq_newer_ref(a, b):
    d = (u16(a) - u16(b)) & 0xFFFF
    return 1 if (d != 0 and d < 0x8000) else 0


def apad_time_after_ref(a, b):
    d = (u32(a) - u32(b)) & 0xFFFFFFFF
    return 1 if (d != 0 and d < 0x80000000) else 0


seq_newer_vectors = [
    dict(name="wrap_forward_across_0xFFFF", a=1, b=0xFFFE,
         spec_ref="S9 apad_seq_newer (S13 sequence wrap, forward direction)"),
    dict(name="wrap_backward_across_0xFFFF", a=0xFFFE, b=1,
         spec_ref="S9 apad_seq_newer (S13 sequence wrap, reverse direction)"),
    dict(name="wrap_boundary_0x0000_after_0xFFFF", a=0, b=0xFFFF,
         spec_ref="S9 apad_seq_newer (exact wrap point, forward)"),
    dict(name="wrap_boundary_0xFFFF_before_0x0000", a=0xFFFF, b=0,
         spec_ref="S9 apad_seq_newer (exact wrap point, reverse)"),
    dict(name="equal_not_newer", a=100, b=100,
         spec_ref="S9 apad_seq_newer (a==b is not strictly newer)"),
    dict(name="trivial_adjacent_no_wrap", a=101, b=100,
         spec_ref="S9 apad_seq_newer (sanity, no wrap involved)"),
    dict(name="half_range_degenerate", a=0x8000, b=0x0000,
         spec_ref="S9 apad_seq_newer (exact half-range: formula defined, "
                   "direction-independent result -- see report)"),
]
for v in seq_newer_vectors:
    v['expect'] = apad_seq_newer_ref(v['a'], v['b'])

time_after_vectors = [
    dict(name="wrap_forward_across_2_32", a=1, b=0xFFFFFFFF,
         spec_ref="S9 apad_time_after (S13 tick wrap, forward direction)"),
    dict(name="wrap_backward_across_2_32", a=0xFFFFFFFF, b=1,
         spec_ref="S9 apad_time_after (S13 tick wrap, reverse direction)"),
    dict(name="wrap_boundary_0_after_max", a=0, b=0xFFFFFFFF,
         spec_ref="S9 apad_time_after (exact wrap point, forward)"),
    dict(name="wrap_boundary_max_before_0", a=0xFFFFFFFF, b=0,
         spec_ref="S9 apad_time_after (exact wrap point, reverse)"),
    dict(name="equal_not_after", a=1000, b=1000,
         spec_ref="S9 apad_time_after (a==b is not strictly after)"),
    dict(name="trivial_adjacent_no_wrap", a=1001, b=1000,
         spec_ref="S9 apad_time_after (sanity, no wrap involved)"),
    dict(name="half_range_degenerate", a=0x80000000, b=0,
         spec_ref="S9 apad_time_after (exact half-range: formula defined, "
                   "direction-independent result -- see report)"),
]
for v in time_after_vectors:
    v['expect'] = apad_time_after_ref(v['a'], v['b'])

# ---------------------------------------------------------------------------
# Section F: Appendix A authentication vectors (S10, S14 -- new in this
# revision). S14 fixes a PIN, salt, iteration count, derived key and tag so
# a byte-exact result can be checked without access to any implementation.
#
# Sanity cross-check (NOT a derivation): this script uses Python's standard
# `hashlib`/`hmac` to recompute the PBKDF2 key and the HMAC tag from the S14
# inputs and asserts they equal the fixed S14 outputs. This catches a
# transcription error in this script; it is not a dependency on core/src, and
# it does not resolve any ambiguity -- every byte emitted into vectors.h below
# is the literal S14 value, not the hashlib-computed one (they are asserted
# equal, and the S14 literal is what gets written out).
# ---------------------------------------------------------------------------

_check_key = hashlib.pbkdf2_hmac('sha256', APPENDIX_A_PIN, APPENDIX_A_SALT,
                                  APPENDIX_A_ITERS, dklen=32)
assert _check_key == APPENDIX_A_KEY, "S14 PBKDF2 transcription mismatch"

_check_tag_msg = APPENDIX_A_HEADER + APPENDIX_A_PAYLOAD + bytes(TAG_LEN)
_check_tag = hmac.new(APPENDIX_A_KEY, _check_tag_msg, hashlib.sha256).digest()[:TAG_LEN]
assert _check_tag == APPENDIX_A_TAG, "S14 HMAC transcription mismatch"

pbkdf2_vectors = [
    dict(name="appendix_a_pbkdf2_derived_key", spec_ref="S10, S14",
         pin=APPENDIX_A_PIN, salt=APPENDIX_A_SALT, iterations=APPENDIX_A_ITERS,
         expected_key=APPENDIX_A_KEY),
]

auth_tag_vectors = []


def add_auth_tag_vector(name, spec_ref, key, datagram, exp_verify_ok):
    auth_tag_vectors.append(dict(
        name=name, spec_ref=spec_ref, key=key, datagram=datagram,
        exp_verify_ok=exp_verify_ok,
    ))


add_auth_tag_vector(
    "appendix_a_tag_valid", "S10, S14 (exact Appendix A datagram and tag)",
    APPENDIX_A_KEY, APPENDIX_A_DATAGRAM, 1,
)

_flip_tag = bytearray(APPENDIX_A_DATAGRAM)
_flip_tag[-TAG_LEN] ^= 0x01  # flip the low bit of the tag's first byte
add_auth_tag_vector(
    "appendix_a_tag_bit_flip_in_tag",
    "S10 (S13: a single-bit flip MUST fail verification)",
    APPENDIX_A_KEY, bytes(_flip_tag), 0,
)

_flip_payload = bytearray(APPENDIX_A_DATAGRAM)
_flip_payload[HEADER_LEN] ^= 0x01  # flip the low bit of origin_ticks_ms's first byte
add_auth_tag_vector(
    "appendix_a_tag_bit_flip_in_payload",
    "S10 (tag covers the whole datagram: a payload bit flip MUST also fail)",
    APPENDIX_A_KEY, bytes(_flip_payload), 0,
)

_flip_header = bytearray(APPENDIX_A_DATAGRAM)
_flip_header[6] ^= 0x01  # flip the low bit of the header's sequence field
add_auth_tag_vector(
    "appendix_a_tag_bit_flip_in_header",
    "S10 (\"the entire datagram, header included\": a header bit flip MUST also fail)",
    APPENDIX_A_KEY, bytes(_flip_header), 0,
)

# ---------------------------------------------------------------------------
# Section I: S10.1 secret-length boundary vectors (docs/PROTOCOL.md S10.1 is
# NEW as of this pass). S10.1: the secret's length depends on the pairing
# channel -- 6 decimal digits when typed, or "a random token of at least 16
# characters" when scanned -- and "A conforming implementation MUST accept a
# secret of any length from 6 to 64 bytes and MUST NOT assume six digits."
# Both channels "derive the session key identically: PBKDF2-HMAC-SHA256
# (secret, server_nonce, 10000, 32)".
#
# Unlike Section F, there is no Appendix A literal to transcribe for any
# length other than 6 (the existing appendix_a_pbkdf2_derived_key vector IS
# the 6-byte case; I.a below reuses the identical PIN/salt/key rather than
# duplicating a second independent 6-byte vector). For every other length
# this section computes expected_key directly with Python's stdlib
# hashlib.pbkdf2_hmac -- independent, general-purpose crypto, the same
# category this file's own Section F header comment and the task
# instructions already treat as in scope. It is not a dependency on
# core/src, and core/src/hmac_sha256.c was NOT read while writing this file.
#
# AMBIGUITY -- reported, not resolved by guessing, and NOT "fixed" by
# adjusting these vectors to whatever core/src happens to do: S10.1 nowhere
# specifies a character set/alphabet for the scanned "random token" (grepped
# docs/PROTOCOL.md for "alphabet", "charset", "character set", "hex",
# "base32", "base64", "A-Z", "a-z", "0-9" -- zero hits). S10.1 also says "The
# secret is an opaque byte string to everything below this section", which
# argues the alphabet cannot matter to the derivation itself (PBKDF2 treats
# its input as raw bytes regardless of content) -- but that only means these
# vectors' LENGTH is normative, not their CONTENT. The byte content chosen
# below is deterministic printable-ASCII filler from the same
# text_field_full_no_nul() helper Section H already uses (seeded so every
# vector's bytes are distinguishable), and is explicitly NOT derived from
# any S10.1 statement about what a scanned token looks like.
#
# Also observed, not resolved here: core/include/atticpad/atticpad.h (header
# only, in scope per the task) declares `void apad_derive_session_key(const
# char *pin, const uint8_t server_nonce[16], uint8_t out[32])` -- no
# explicit length parameter, so whatever calls it must be relying on a
# NUL-terminated C string. That means the "opaque byte string" S10.1
# describes provably cannot contain an embedded NUL (0x00) byte through this
# entry point, which is a narrower contract than S10.1's prose states. None
# of the content below contains a NUL, so it does not affect these vectors,
# but it is worth flagging in the report.
# ---------------------------------------------------------------------------

secret_length_vectors = []


def add_secret_length_vector(name, spec_ref, pin, salt):
    assert 0x00 not in pin, "pin is consumed as a NUL-terminated C string"
    assert 6 <= len(pin) <= 64, "S10.1: valid secrets are 6..64 bytes"
    expected_key = hashlib.pbkdf2_hmac('sha256', pin, salt, 10000, dklen=32)
    secret_length_vectors.append(dict(
        name=name, spec_ref=spec_ref, pin=pin, salt=salt,
        iterations=10000, expected_key=expected_key,
    ))


# I.a -- 6 bytes, the typed-PIN minimum (S10.1 table row 1, "6 decimal
# digits"). Byte-identical to Appendix A's PIN/salt/key -- asserted below --
# kept as its own named vector so a driver can assert the S10.1 boundary by
# name without relying on Section F's vector meaning the same thing.
add_secret_length_vector(
    "secret_length_6_typed_pin_minimum",
    "S10.1 (\"6 decimal digits\", the typed-channel minimum; S14 salt)",
    APPENDIX_A_PIN, APPENDIX_A_SALT,
)
assert secret_length_vectors[-1]['expected_key'] == APPENDIX_A_KEY, \
    "S10.1's 6-byte typed case must equal Appendix A's PIN/key exactly"

# I.b -- 16 bytes, the scanned-token minimum (S10.1 table row 2, "a random
# token of at least 16 characters").
add_secret_length_vector(
    "secret_length_16_scanned_token_minimum",
    "S10.1 (\"a random token of at least 16 characters\", the scanned-"
    "channel minimum; S14 salt; token alphabet unspecified by S10.1 -- see "
    "this section's header comment)",
    text_field_full_no_nul(16, seed=101), APPENDIX_A_SALT,
)

# I.c -- 32 bytes: exactly one SHA-256 / HMAC-SHA256 digest length, and
# roughly the midpoint of the 6..64 span S10.1 defines. Not itself named as
# a boundary by S10.1, but the likeliest place an under-specified HMAC
# key-handling path confuses "digest length" (32) with "block length" (64,
# the actual point at which HMAC must pre-hash an oversized key).
add_secret_length_vector(
    "secret_length_32_mid_range",
    "S10.1 (\"any length from 6 to 64 bytes\" -- mid-range point at exactly "
    "one SHA-256 digest length; S14 salt)",
    text_field_full_no_nul(32, seed=103), APPENDIX_A_SALT,
)

# I.d -- 33 bytes: one past I.c, in case a boundary is coded as "<= 32"
# rather than "< 32" against that digest-length confusion.
add_secret_length_vector(
    "secret_length_33_one_past_digest_length",
    "S10.1 (one byte past I.c's digest-length boundary; S14 salt)",
    text_field_full_no_nul(33, seed=107), APPENDIX_A_SALT,
)

# I.e -- 64 bytes, the S10.1 maximum, and also exactly the HMAC-SHA256
# BLOCK length (64 bytes) -- the real internal threshold at which a correct
# HMAC implementation must pre-hash an oversized key rather than use it
# directly zero-padded. The single most likely off-by-one boundary in this
# whole section: a secret of exactly 64 bytes MUST still be accepted per
# S10.1, whether or not the implementation's HMAC key buffer is sized 64.
add_secret_length_vector(
    "secret_length_64_maximum",
    "S10.1 (\"any length from 6 to 64 bytes\" -- the maximum, and exactly "
    "the HMAC-SHA256 block length; S14 salt)",
    text_field_full_no_nul(64, seed=109), APPENDIX_A_SALT,
)

# ---------------------------------------------------------------------------
# Section G: the eleven S6 payloads (ANNOUNCE, HELLO, WELCOME, BYE, PING,
# PONG, RUMBLE, LED, STATUS, ACK, ERROR).
#
# S15 item 1 flagged these as "the least-reviewed part of the wire format...
# implemented once, against themselves" -- unlike INPUT_STATE (S5), no
# independently-authored vector had ever exercised them. Every field below
# is set to a distinctive, non-zero, non-repeating value (never all-zero,
# which would pass against a decoder reading the wrong offset), every
# reserved byte is sent non-zero on the wire to assert the S2 scrub applies
# here too, and the type-specific receive-side rules S6 states are each
# exercised at their boundary: S6.2 pairing_required normalisation (any
# non-zero -> 1), S6.8 LED.player_index normalisation (>4 -> 0, but the codec
# was found NOT to apply this rule -- see the report), and S6.0's mirror-image
# rule for BYE.reason / STATUS.code / ERROR.code (including ERROR code 0,
# unassigned and reserved): these are diagnostic labels, NOT clamped, and an
# unrecognised value MUST be preserved verbatim with the packet accepted, not
# rejected. S6.0 is new (added after this section's first pass) precisely
# because a port author who just implemented the two clamps above is likely
# to assume the pattern continues into these three fields; it doesn't.
# ---------------------------------------------------------------------------

def build_announce_payload(server_name, pads_total, pads_free, pairing_required,
                            reserved0, server_port, reserved1):
    """PROTOCOL.md S6.2, 40-byte ANNOUNCE payload."""
    assert len(server_name) == NAME_LEN
    out = server_name
    out += struct.pack('<BBBB', pads_total & 0xFF, pads_free & 0xFF,
                        pairing_required & 0xFF, reserved0 & 0xFF)
    out += struct.pack('<H', u16(server_port))
    out += struct.pack('<H', u16(reserved1))
    assert len(out) == ANNOUNCE_LEN
    return out


def build_hello_payload(client_id, caps, device_name, client_nonce,
                         desired_rate_hz, proto_major, reserved0, client_ticks_ms):
    """PROTOCOL.md S6.3, 76-byte HELLO payload."""
    assert len(client_id) == CLIENT_ID_LEN
    assert len(device_name) == NAME_LEN
    assert len(client_nonce) == NONCE_LEN
    out = client_id
    out += struct.pack('<I', u32(caps))
    out += device_name
    out += client_nonce
    out += struct.pack('<H', u16(desired_rate_hz))
    out += struct.pack('<BB', proto_major & 0xFF, reserved0 & 0xFF)
    out += struct.pack('<I', u32(client_ticks_ms))
    assert len(out) == HELLO_LEN
    return out


def build_welcome_payload(session_id, pad_slot, flags, input_rate_hz, reserved0,
                           server_nonce, key_material, server_ticks_ms):
    """PROTOCOL.md S6.4, 60-byte WELCOME payload."""
    assert len(server_nonce) == NONCE_LEN
    assert len(key_material) == KEY_MATERIAL_LEN
    out = struct.pack('<H', u16(session_id))
    out += struct.pack('<BB', pad_slot & 0xFF, flags & 0xFF)
    out += struct.pack('<H', u16(input_rate_hz))
    out += struct.pack('<H', u16(reserved0))
    out += server_nonce
    out += key_material
    out += struct.pack('<I', u32(server_ticks_ms))
    assert len(out) == WELCOME_LEN
    return out


def build_bye_payload(reason, reserved0):
    """PROTOCOL.md S6.5, 4-byte BYE payload."""
    assert len(reserved0) == 3
    out = struct.pack('<B', reason & 0xFF) + bytes(b & 0xFF for b in reserved0)
    assert len(out) == BYE_LEN
    return out


def build_ping_payload(origin_ticks_ms, responder_ticks_ms):
    """PROTOCOL.md S6.6, 8-byte PING/PONG payload (shared layout)."""
    out = struct.pack('<II', u32(origin_ticks_ms), u32(responder_ticks_ms))
    assert len(out) == PING_LEN
    return out


def build_rumble_payload(low_freq, high_freq, duration_ms, reserved0):
    """PROTOCOL.md S6.7, 8-byte RUMBLE payload."""
    out = struct.pack('<HHHH', u16(low_freq), u16(high_freq), u16(duration_ms), u16(reserved0))
    assert len(out) == RUMBLE_LEN
    return out


def build_led_payload(player_index, r, g, b):
    """PROTOCOL.md S6.8, 4-byte LED payload."""
    out = struct.pack('<BBBB', player_index & 0xFF, r & 0xFF, g & 0xFF, b & 0xFF)
    assert len(out) == LED_LEN
    return out


def build_status_payload(code, reserved0, text):
    """PROTOCOL.md S6.9, 64-byte STATUS payload."""
    assert len(reserved0) == 3
    assert len(text) == TEXT_LEN
    out = struct.pack('<B', code & 0xFF) + bytes(b & 0xFF for b in reserved0) + text
    assert len(out) == STATUS_LEN
    return out


def build_ack_payload(sequence, reserved0):
    """PROTOCOL.md S6.10, 4-byte ACK payload."""
    out = struct.pack('<HH', u16(sequence), u16(reserved0))
    assert len(out) == ACK_LEN
    return out


def build_error_payload(code, reserved0, text):
    """PROTOCOL.md S6.11, 64-byte ERROR payload."""
    assert len(text) == TEXT_LEN
    out = struct.pack('<HH', u16(code), u16(reserved0)) + text
    assert len(out) == ERROR_LEN
    return out


# ---- G.1 ANNOUNCE (S6.2) ---------------------------------------------------

announce_vectors = []


def add_announce_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_ANNOUNCE, session_id=0,
                            sequence=3, payload_len=ANNOUNCE_LEN, flags=0)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(
        server_name=text_field_padded(b"Living Room PC", NAME_LEN),
        pads_total=8, pads_free=3, pairing_required=1, reserved0=0xFE,
        server_port=21100, reserved1=0xBEEF,
    )
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_announce_payload(**payload_defaults)
    announce_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_server_name=payload_defaults['server_name'],
        exp_pads_total=payload_defaults['pads_total'] & 0xFF,
        exp_pads_free=payload_defaults['pads_free'] & 0xFF,
        exp_pairing_required=normalize_pairing_required(payload_defaults['pairing_required']),
        exp_server_port=u16(payload_defaults['server_port']),
    ))


add_announce_vector("announce_baseline", "S6.2 (S2 reserved scrub)", {}, {})
add_announce_vector(
    "announce_pairing_required_nonzero_normalizes_to_1",
    "S6.2 (new rule, commit e467fd8: any non-zero MUST read as 1)",
    {}, dict(pairing_required=200),
)
add_announce_vector(
    "announce_pairing_required_zero_stays_zero",
    "S6.2 (boundary: 0 MUST NOT be touched by the non-zero-to-1 rule)",
    {}, dict(pairing_required=0),
)
add_announce_vector(
    "announce_server_name_full_width_no_nul",
    "S6.2, S2 (text field exactly full width, no NUL terminator)",
    {}, dict(server_name=text_field_full_no_nul(NAME_LEN), pads_total=1, pads_free=1),
)

# ---- G.2 HELLO (S6.3) -------------------------------------------------------

hello_vectors = []


def add_hello_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_HELLO, session_id=0,
                            sequence=0, payload_len=HELLO_LEN, flags=FLAG_RELIABLE)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(
        client_id=bytes(range(0x00, 0x10)),
        caps=(CAP_DPAD | CAP_FACE4 | CAP_STICK_L | CAP_TRIGGERS | CAP_TOUCH
              | CAP_GYRO | CAP_ACCEL | CAP_RUMBLE | CAP_LED | CAP_BATTERY),
        device_name=text_field_padded(b"3DS Test Client", NAME_LEN),
        client_nonce=bytes(range(0x10, 0x20)),
        desired_rate_hz=125,
        proto_major=VERSION_1,
        reserved0=0xEE,
        client_ticks_ms=0xCAFEBABE,
    )
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_hello_payload(**payload_defaults)
    hello_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_client_id=payload_defaults['client_id'],
        exp_caps=u32(payload_defaults['caps']) & CAPS_VALID_MASK,
        exp_device_name=payload_defaults['device_name'],
        exp_client_nonce=payload_defaults['client_nonce'],
        exp_desired_rate_hz=u16(payload_defaults['desired_rate_hz']),
        exp_proto_major=payload_defaults['proto_major'] & 0xFF,
        exp_client_ticks_ms=u32(payload_defaults['client_ticks_ms']),
    ))


add_hello_vector("hello_baseline", "S6.3 (S2 reserved scrub)", {}, {})
add_hello_vector(
    "hello_caps_reserved_bits_14_31_masked",
    "S6.3 (\"bits 14..31 reserved, MUST be zero\")",
    {}, dict(caps=(CAP_DPAD | CAP_FACE4 | 0xFFFFC000)),
)
add_hello_vector(
    "hello_device_name_split_utf8_boundary",
    "S6.3, S2 (text field split multi-byte UTF-8 at the boundary)",
    {}, dict(device_name=text_field_split_utf8(NAME_LEN)),
)

# ---- G.3 WELCOME (S6.4) -----------------------------------------------------
#
# key_material "MUST be zero in v1 ... MUST be ignored on receive" (S6.4) is
# asserted in BOTH directions per the task: welcome_baseline below covers
# decode (wire carries non-zero garbage, decoded output MUST be all-zero);
# welcome_encode_key_material_forced_zero (a distinct vector shape, since
# this is the one S6 field this file needs to check on the ENCODE side)
# covers send (a caller supplying non-zero key_material MUST see it forced
# to zero in the encoded bytes -- S15 item 4: "a caller who sets it will see
# it silently dropped").

welcome_vectors = []


def add_welcome_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_WELCOME, session_id=1,
                            sequence=1, payload_len=WELCOME_LEN, flags=FLAG_RELIABLE)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(
        session_id=0xBEEF, pad_slot=3, flags=0xFF,  # bit0=1 + reserved bits1-7 garbage
        input_rate_hz=60, reserved0=0xABCD,
        server_nonce=bytes(range(0x20, 0x30)),
        key_material=bytes([0xFF] * KEY_MATERIAL_LEN),  # non-conforming sender
        server_ticks_ms=0x11223344,
    )
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_welcome_payload(**payload_defaults)
    welcome_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_session_id=u16(payload_defaults['session_id']),
        exp_pad_slot=payload_defaults['pad_slot'] & 0xFF,
        exp_flags=payload_defaults['flags'] & 0x01,  # S6.4: bit0 AUTH_REQUIRED only
        exp_input_rate_hz=u16(payload_defaults['input_rate_hz']),
        exp_server_nonce=payload_defaults['server_nonce'],
        exp_key_material=bytes(KEY_MATERIAL_LEN),  # S6.4: MUST be ignored/scrubbed on receive
        exp_server_ticks_ms=u32(payload_defaults['server_ticks_ms']),
    ))


add_welcome_vector(
    "welcome_baseline",
    "S6.4 (S2 reserved scrub; flags bits1-7 masked; key_material decode-scrub)",
    {}, {},
)

welcome_encode_vectors = []


def add_welcome_encode_vector(name, spec_ref, in_kwargs):
    in_defaults = dict(
        session_id=0x4321, pad_slot=2, flags=0x01, input_rate_hz=90,
        server_nonce=bytes(range(0x40, 0x50)),
        key_material=bytes([0xAA] * KEY_MATERIAL_LEN),
        server_ticks_ms=0x99AABBCC,
    )
    in_defaults.update(in_kwargs)
    exp_payload = build_welcome_payload(
        session_id=in_defaults['session_id'], pad_slot=in_defaults['pad_slot'],
        flags=in_defaults['flags'], input_rate_hz=in_defaults['input_rate_hz'],
        reserved0=0, server_nonce=in_defaults['server_nonce'],
        key_material=bytes(KEY_MATERIAL_LEN),  # S6.4: MUST be zero regardless of input
        server_ticks_ms=in_defaults['server_ticks_ms'],
    )
    welcome_encode_vectors.append(dict(
        name=name, spec_ref=spec_ref,
        in_session_id=u16(in_defaults['session_id']),
        in_pad_slot=in_defaults['pad_slot'] & 0xFF,
        in_flags=in_defaults['flags'] & 0xFF,
        in_input_rate_hz=u16(in_defaults['input_rate_hz']),
        in_server_nonce=in_defaults['server_nonce'],
        in_key_material=in_defaults['key_material'],
        in_server_ticks_ms=u32(in_defaults['server_ticks_ms']),
        exp_payload=exp_payload,
    ))


add_welcome_encode_vector(
    "welcome_encode_key_material_forced_zero",
    "S6.4 (\"MUST be zero in v1\", send side -- decode side is welcome_baseline)",
    {},
)

# ---- G.4 BYE (S6.5) ---------------------------------------------------------

bye_vectors = []


def add_bye_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_BYE, session_id=1,
                            sequence=9, payload_len=BYE_LEN, flags=FLAG_RELIABLE)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(reason=2, reserved0=[0xAA, 0xBB, 0xCC])
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_bye_payload(**payload_defaults)
    bye_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_reason=payload_defaults['reason'] & 0xFF,
    ))


add_bye_vector("bye_baseline", "S6.5 (S2 reserved scrub)", {}, {})
add_bye_vector(
    "bye_reason_4_unrecognized_preserved",
    "S6.0 (\"a receiver MUST preserve an unrecognised value verbatim and MUST "
    "NOT reject the packet\" -- reason only defines 0-3; NOT clamped)",
    {}, dict(reason=4),
)
add_bye_vector(
    "bye_reason_255_unrecognized_preserved",
    "S6.0 (unrecognised value preserved verbatim, packet not rejected; max byte value)",
    {}, dict(reason=255),
)

# ---- G.5/G.6 PING / PONG (S6.6) ---------------------------------------------

ping_vectors = []
pong_vectors = []


def add_ping_pong_vector(dest_list, name, spec_ref, msg_type, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=msg_type, session_id=1,
                            sequence=4, payload_len=PING_LEN, flags=0)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(origin_ticks_ms=0x11223344, responder_ticks_ms=0xAABBCCDD)
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_ping_payload(**payload_defaults)
    dest_list.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_origin_ticks_ms=u32(payload_defaults['origin_ticks_ms']),
        exp_responder_ticks_ms=u32(payload_defaults['responder_ticks_ms']),
    ))


add_ping_pong_vector(
    ping_vectors, "ping_baseline",
    "S6.6 (offset check: distinct values in both 4-byte halves. "
    "responder_ticks_ms is PRESERVED, not scrubbed, even inside a PING where "
    "S6.6 also says it 'MUST be zero on send' -- PING and PONG share one "
    "decoder, so this is now stated explicitly rather than left to S2's scrub "
    "rule, which would have been an equally defensible but wrong reading. "
    "This vector's 0xAABBCCDD in responder_ticks_ms decided that question "
    "before it was made explicit; it was right, and now cites the rule that "
    "confirms it instead of an inference from S2.)",
    TYPE_PING, {}, {},
)
add_ping_pong_vector(
    pong_vectors, "pong_baseline",
    "S6.6 (offset check: distinct values in both 4-byte halves; correlation "
    "is by origin_ticks_ms alone, per the S9 direct-answer rule)",
    TYPE_PONG, dict(sequence=5),
    dict(origin_ticks_ms=0x99887766, responder_ticks_ms=0x55443322),
)

# ---- G.7 RUMBLE (S6.7) -------------------------------------------------------

rumble_vectors = []


def add_rumble_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_RUMBLE, session_id=1,
                            sequence=6, payload_len=RUMBLE_LEN, flags=FLAG_RELIABLE)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(low_freq=0x1234, high_freq=0x5678, duration_ms=500, reserved0=0xDEAD)
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_rumble_payload(**payload_defaults)
    rumble_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_low_freq=u16(payload_defaults['low_freq']),
        exp_high_freq=u16(payload_defaults['high_freq']),
        exp_duration_ms=u16(payload_defaults['duration_ms']),
    ))


add_rumble_vector("rumble_baseline", "S6.7 (S2 reserved scrub)", {}, {})

# ---- G.8 LED (S6.8) ----------------------------------------------------------
#
# The audit's own example of why this section exists: "the audit found a
# real S6 semantic bug that slipped through exactly this gap: apad_decode_led
# never applies S6.8's 'values above 4 MUST be treated as 0 on receive', and
# there was no vector to catch it." Both boundaries of the reserved range are
# covered, plus the 0 = off sentinel (a valid value the rule must NOT touch).

led_vectors = []


def add_led_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_LED, session_id=1,
                            sequence=7, payload_len=LED_LEN, flags=FLAG_RELIABLE)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(player_index=3, r=0x11, g=0x22, b=0x33)
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_led_payload(**payload_defaults)
    led_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_player_index=normalize_led_player_index(payload_defaults['player_index']),
        exp_r=payload_defaults['r'] & 0xFF,
        exp_g=payload_defaults['g'] & 0xFF,
        exp_b=payload_defaults['b'] & 0xFF,
    ))


add_led_vector("led_baseline_valid_index", "S6.8", {}, {})
add_led_vector(
    "led_player_index_0_off", "S6.8 (0 = off, a valid value; MUST NOT be touched)",
    {}, dict(player_index=0),
)
add_led_vector(
    "led_player_index_4_last_valid",
    "S6.8 (4 is the last VALID value, i.e. NOT reserved -- the off-by-one an "
    "eyeballed \"> 4\" clamp gets wrong; must decode unchanged, not to 0)",
    {}, dict(player_index=4),
)
add_led_vector(
    "led_player_index_5_first_reserved_normalized",
    "S6.8 (\"values above 4 MUST be treated as 0\" -- first reserved value)",
    {}, dict(player_index=5),
)
add_led_vector(
    "led_player_index_255_max_reserved_normalized",
    "S6.8 (\"values above 4 MUST be treated as 0\" -- last/max reserved value)",
    {}, dict(player_index=255),
)

# ---- G.9 STATUS (S6.9) -------------------------------------------------------

status_vectors = []


def add_status_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_STATUS, session_id=1,
                            sequence=8, payload_len=STATUS_LEN, flags=FLAG_RELIABLE)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(
        code=2, reserved0=[0xDE, 0xAD, 0xBE],
        text=text_field_padded(b"Controller battery critical", TEXT_LEN),
    )
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_status_payload(**payload_defaults)
    status_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_code=payload_defaults['code'] & 0xFF,
        exp_text=payload_defaults['text'],
    ))


add_status_vector("status_baseline", "S6.9 (S2 reserved scrub)", {}, {})
add_status_vector(
    "status_text_full_width_no_nul", "S6.9, S2 (text field exactly full width, no NUL)",
    {}, dict(text=text_field_full_no_nul(TEXT_LEN)),
)
add_status_vector(
    "status_code_3_unrecognized_preserved",
    "S6.0 (\"a receiver MUST preserve an unrecognised value verbatim and MUST "
    "NOT reject the packet\" -- code only defines 0-2; NOT clamped)",
    {}, dict(code=3),
)
add_status_vector(
    "status_code_255_unrecognized_preserved",
    "S6.0 (unrecognised value preserved verbatim, packet not rejected; max byte value)",
    {}, dict(code=255),
)

# ---- G.10 ACK (S6.10) --------------------------------------------------------
#
# S9's new "what discharges a reliable message" paragraph makes ACK.sequence
# (the sequence being acknowledged) directly load-bearing: a HELLO or a
# WELCOME retransmit could plausibly straddle the 0xFFFF wrap over a long
# session, so the wrap-boundary values get their own vectors here too, not
# just the header's own sequence field (Section C).

ack_vectors = []


def add_ack_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_ACK, session_id=1,
                            sequence=2, payload_len=ACK_LEN, flags=0)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(sequence=0xBEEF, reserved0=0xF00D)
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_ack_payload(**payload_defaults)
    ack_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_sequence=u16(payload_defaults['sequence']),
    ))


add_ack_vector("ack_baseline", "S6.10 (S2 reserved scrub)", {}, {})
add_ack_vector(
    "ack_sequence_raw_0xFFFF",
    "S6.10, S9 (S13 sequence wrap boundary, in the acknowledged-sequence field)",
    {}, dict(sequence=0xFFFF),
)
add_ack_vector(
    "ack_sequence_raw_0x0000",
    "S6.10, S9 (S13 sequence wrap boundary, in the acknowledged-sequence field)",
    {}, dict(sequence=0x0000),
)

# ---- G.11 ERROR (S6.11) ------------------------------------------------------

error_vectors = []


def add_error_vector(name, spec_ref, header_kwargs, payload_kwargs):
    header_defaults = dict(version=VERSION_1, type_=TYPE_ERROR, session_id=1,
                            sequence=10, payload_len=ERROR_LEN, flags=0)
    header_defaults.update(header_kwargs)
    payload_defaults = dict(
        code=6, reserved0=0xC0DE,
        text=text_field_padded(b"malformed packet: bad payload_len", TEXT_LEN),
    )
    payload_defaults.update(payload_kwargs)
    packet = build_header(**header_defaults) + build_error_payload(**payload_defaults)
    error_vectors.append(dict(
        name=name, spec_ref=spec_ref, packet=packet,
        exp_code=u16(payload_defaults['code']),
        exp_text=payload_defaults['text'],
    ))


add_error_vector("error_baseline", "S6.11 (S2 reserved scrub)", {}, {})
add_error_vector(
    "error_text_split_utf8_boundary", "S6.11, S2 (text field split multi-byte UTF-8 at the boundary)",
    {}, dict(text=text_field_split_utf8(TEXT_LEN)),
)
add_error_vector(
    "error_code_0_unassigned_preserved",
    "S6.0 (\"ERROR code 0 is unassigned and reserved. A receiver treats it as "
    "an unrecognised code under the rule above\" -- preserved verbatim, packet "
    "not rejected, NOT normalised to some other value)",
    {}, dict(code=0),
)
add_error_vector(
    "error_code_8_unrecognized_preserved",
    "S6.0 (\"a receiver MUST preserve an unrecognised value verbatim and MUST "
    "NOT reject the packet\" -- codes 1-7 are defined; 8 is the first "
    "unrecognised value above the defined table)",
    {}, dict(code=8),
)
add_error_vector(
    "error_code_255_unrecognized_preserved",
    "S6.0 (unrecognised value preserved verbatim, packet not rejected; code is "
    "u16-wide -- 255 exercises the low byte at its max while the high byte is 0)",
    {}, dict(code=255),
)

# ---------------------------------------------------------------------------
# Section H: apad_text_set / apad_text_len / apad_text_get -- the S2
# sender-truncation rule, exercised directly against the codec's text-field
# helpers (core/include/atticpad/atticpad.h) rather than only indirectly
# through a full HELLO/STATUS/ERROR payload decode.
#
# S2's text-field paragraph states four obligations. This section's two
# subsections map onto them:
#
#   1. "NUL-padded to their fixed width ... need not be NUL-terminated when
#      [it fills] the field."                        -> H.1 SET vectors
#   2. "A receiver MUST treat a text field as bounded by its fixed width."
#                                                       -> H.2 GET/LEN vectors
#   3. "A sender MUST NOT split a multi-byte UTF-8 sequence across the end
#      of the field -- truncate at a character boundary and NUL-pad the
#      remainder."  THE RULE THIS SECTION WAS ADDED TO COVER; had zero
#      vectors before this pass.                       -> H.1 SET vectors
#   4. "A receiver MUST tolerate a malformed trailing sequence rather than
#      reject the packet."                             -> H.2 GET/LEN vectors
#      (also already exercised at the full-payload level in Section G --
#      hello_device_name_split_utf8_boundary, status_text_split_utf8_boundary,
#      error_text_split_utf8_boundary -- which check that a whole HELLO/
#      STATUS/ERROR datagram carrying a malformed trailing field is still
#      accepted. H.2 below instead calls apad_text_len/apad_text_get
#      directly, so a bug local to those two functions -- as opposed to the
#      payload decoders that call them -- has somewhere to show up.)
#
# apad_text_set can't be driven by a decoder-only vectors.h any more than
# apad_encode_welcome could (see apad_vec_welcome_encode above for that
# precedent): H.1 gives the self-test harness a raw `in_src` C string and
# `width`, and the exact `width`-byte field apad_text_set(field, width,
# in_src) MUST produce. exp_field is computed here by a reference
# truncation walk written directly from the S2 rule (see
# _text_set_reference below), independent of and never reading
# core/src/codec.c.
# ---------------------------------------------------------------------------

# UTF-8 encodings of one character at each of the three multi-byte lengths
# S2's rule can split. Plain Python str.encode('utf-8'): standard, universal
# UTF-8 encoding, not this project's codec -- the same category of
# independent reference as Section F's hashlib/hmac cross-check.
UTF8_2BYTE = "é".encode("utf-8")        # e-acute, U+00E9 -> C3 A9
UTF8_3BYTE = "中".encode("utf-8")        # CJK U+4E2D "middle" -> E4 B8 AD
UTF8_4BYTE = "\U0001f600".encode("utf-8")    # U+1F600 grinning face -> F0 9F 98 80
assert UTF8_2BYTE == bytes.fromhex("C3A9")
assert UTF8_3BYTE == bytes.fromhex("E4B8AD")
assert UTF8_4BYTE == bytes.fromhex("F09F9880")
assert len(UTF8_2BYTE) == 2 and len(UTF8_3BYTE) == 3 and len(UTF8_4BYTE) == 4


def _utf8_lead_char_len(lead_byte):
    """How many bytes the UTF-8 character starting at this lead byte
    occupies, per the standard UTF-8 lead-byte pattern (RFC 3629 S3): used
    here only to find character boundaries in WELL-FORMED input, exactly
    what S2's rule assumes ("MUST NOT split a multi-byte UTF-8 sequence").
    A continuation byte or an invalid lead byte falls through to 1 -- S2
    does not say what "truncate at a character boundary" means for a
    SOURCE that is already malformed UTF-8 before truncation even begins;
    see the report for this ambiguity. It is never reached by any vector
    below: every in_src here is well-formed UTF-8 by construction."""
    if lead_byte & 0x80 == 0x00:
        return 1
    if lead_byte & 0xE0 == 0xC0:
        return 2
    if lead_byte & 0xF0 == 0xE0:
        return 3
    if lead_byte & 0xF8 == 0xF0:
        return 4
    return 1


def _text_set_reference(src_bytes, width):
    """Reference implementation of PROTOCOL.md S2's sender-side rule,
    written directly from the spec text and independent of apad_text_set:
    walk src_bytes one whole UTF-8 character at a time; stop BEFORE any
    character that would not fit entirely within the remaining `width`
    budget (this is what "truncate at a character boundary" means -- a
    partially-fitting character is dropped in full, not split); NUL-pad
    whatever budget is left over ("NUL-pad the remainder")."""
    out = bytearray()
    i = 0
    n = len(src_bytes)
    while i < n:
        clen = _utf8_lead_char_len(src_bytes[i])
        if len(out) + clen > width:
            break
        out.extend(src_bytes[i:i + clen])
        i += clen
    out.extend(bytes(width - len(out)))
    assert len(out) == width
    return bytes(out)


text_set_vectors = []


def add_text_set_vector(name, spec_ref, src_bytes, width):
    assert 0x00 not in src_bytes, "in_src is a NUL-terminated C string; no embedded NUL"
    assert len(src_bytes) <= 0xFFFF
    text_set_vectors.append(dict(
        name=name, spec_ref=spec_ref, in_src=bytes(src_bytes), width=width,
        exp_field=_text_set_reference(src_bytes, width),
    ))


# H.1.a -- obligation 1: fits with room to spare -> NUL-padded remainder.
add_text_set_vector(
    "text_set_fits_with_room_to_spare",
    "S2 (\"NUL-padded to their fixed width\")",
    b"Living Room PC", NAME_LEN,
)

# H.1.b -- obligation 1: exactly fills the field -> no NUL terminator
# required (and none fits: the field is exactly `width` bytes).
add_text_set_vector(
    "text_set_exact_fill_no_nul_needed",
    "S2 (\"need not be NUL-terminated when [it fills] the field\")",
    text_field_full_no_nul(NAME_LEN), NAME_LEN,
)

# H.1.c -- obligation 3, the simple case: plain ASCII (every character is
# 1 byte, so truncation can never split one) longer than the field. Also
# exercises "exactly fills the field" again, this time as a truncation
# OUTCOME rather than an exact-length input.
add_text_set_vector(
    "text_set_ascii_truncation_at_boundary",
    "S2 (sender truncation, plain-ASCII case: never splits, since every "
    "character is 1 byte)",
    text_field_full_no_nul(NAME_LEN + 8, seed=3), NAME_LEN,
)

# H.1.d/e/f -- obligation 3, the point of this section: a field width that
# lands exactly mid-sequence for each of the three multi-byte UTF-8 lengths
# S2's rule can split. Filler is (width - (charlen - 1)) bytes of plain
# ASCII, so the multi-byte character's FIRST byte is the very last byte
# that would otherwise fit -- the character needs one more byte than the
# field has left, so the whole character (not a partial one) MUST be
# dropped and every byte from the filler's end to the field's end MUST be
# NUL (exp_field, built by _text_set_reference, asserts the full padded
# remainder, not just the surviving prefix).
add_text_set_vector(
    "text_set_split_2byte_utf8_dropped_and_padded",
    "S2 (\"MUST NOT split a multi-byte UTF-8 sequence across the end of "
    "the field -- truncate at a character boundary and NUL-pad the "
    "remainder\" -- 2-byte case, e.g. e-acute)",
    text_field_full_no_nul(NAME_LEN - 1, seed=5) + UTF8_2BYTE, NAME_LEN,
)
add_text_set_vector(
    "text_set_split_3byte_utf8_dropped_and_padded",
    "S2 (as above, 3-byte case, e.g. CJK)",
    text_field_full_no_nul(NAME_LEN - 2, seed=7) + UTF8_3BYTE, NAME_LEN,
)
add_text_set_vector(
    "text_set_split_4byte_utf8_dropped_and_padded",
    "S2 (as above, 4-byte case, e.g. an emoji outside the BMP)",
    text_field_full_no_nul(NAME_LEN - 3, seed=11) + UTF8_4BYTE, NAME_LEN,
)

# H.1.g -- the 3-byte split case again, but at APAD_TEXT_LEN (60) instead of
# APAD_NAME_LEN (32), so this section covers both real wire widths (task
# instruction: "use real ones rather than invented widths") rather than
# only the 32-byte one.
add_text_set_vector(
    "text_set_split_3byte_utf8_dropped_and_padded_at_text_len",
    "S2 (as above, 3-byte case, width=APAD_TEXT_LEN=60)",
    text_field_full_no_nul(TEXT_LEN - 2, seed=13) + UTF8_3BYTE, TEXT_LEN,
)

# H.1.h/i -- the empty string, at both real widths: the whole field MUST be
# NUL.
add_text_set_vector(
    "text_set_empty_string_name_len",
    "S2 (empty string, width=APAD_NAME_LEN=32)",
    b"", NAME_LEN,
)
add_text_set_vector(
    "text_set_empty_string_text_len",
    "S2 (empty string, width=APAD_TEXT_LEN=60)",
    b"", TEXT_LEN,
)

# ---- H.2: apad_text_len / apad_text_get (obligations 2 and 4) -------------
#
# exp_len follows the bounded-strnlen contract given by the DECLARED
# signature's own doc comment in atticpad.h ("Length of a fixed-width text
# field, bounded by the width") -- the header is explicitly in scope per
# the task (the .c file is not): scan for a NUL up to `width` bytes; if
# none is found, the length IS `width` ("bounded by its fixed width", S2
# obligation 2), not an error and not an unbounded scan past the field.
# exp_cstr is the NUL-terminated copy apad_text_get(dst, dst_cap, field,
# width) MUST produce; dst_cap is generous (width + 1) throughout so this
# section tests only the S2 text-field rule, not apad_text_get's separate
# dst_cap-clamping contract.

text_get_vectors = []


def add_text_get_vector(name, spec_ref, in_field, width, dst_cap=None):
    assert len(in_field) == width
    if dst_cap is None:
        dst_cap = width + 1
    nul_at = in_field.find(0)
    exp_len = nul_at if nul_at != -1 else width
    copy_len = min(exp_len, dst_cap - 1)
    exp_cstr = bytes(in_field[:copy_len]) + bytes(1)  # NUL-terminated
    text_get_vectors.append(dict(
        name=name, spec_ref=spec_ref, in_field=bytes(in_field), width=width,
        dst_cap=dst_cap, exp_len=exp_len, exp_cstr=exp_cstr,
    ))


# H.2.a -- ordinary padded field (obligation 1's receive-side mirror, for
# contrast with the "exact fill" and "malformed trailing" cases below).
add_text_get_vector(
    "text_get_fits_with_nul_padding", "S2 (ordinary NUL-padded field)",
    text_field_padded(b"Living Room PC", NAME_LEN), NAME_LEN,
)

# H.2.b -- obligation 2: a field with NO NUL anywhere (it exactly fills the
# width). apad_text_len MUST return `width`, not scan past it looking for a
# terminator that isn't there; apad_text_get MUST copy exactly `width`
# bytes and terminate at dst[width].
add_text_get_vector(
    "text_get_exact_fill_no_nul_bounded_by_width",
    "S2 (\"A receiver MUST treat a text field as bounded by its fixed "
    "width\" -- no NUL present anywhere in the field)",
    text_field_full_no_nul(NAME_LEN), NAME_LEN,
)

# H.2.c -- obligation 4: a malformed trailing sequence (here, a lone 2-byte
# lead byte with no continuation byte and no NUL -- exactly the case a
# non-conforming sender, or corruption, could put on the wire). MUST NOT be
# rejected, MUST NOT overrun: apad_text_len is still bounded by `width`,
# and apad_text_get tolerates and copies the malformed byte through
# verbatim rather than repairing or rejecting it -- consistent with how
# Section G's *_split_utf8_boundary payload vectors already treat this at
# the full-payload level.
add_text_get_vector(
    "text_get_malformed_trailing_2byte_lead_only",
    "S2 (\"A receiver MUST tolerate a malformed trailing sequence rather "
    "than reject the packet\" -- lone lead byte, no continuation, no NUL)",
    text_field_split_utf8(NAME_LEN), NAME_LEN,
)

# H.2.d -- obligation 4 again, a different malformed shape: two bytes of a
# three-byte sequence (lead + one continuation byte) with the final
# continuation byte missing, at APAD_TEXT_LEN this time.
_malformed_3byte_partial_tail = (
    text_field_full_no_nul(TEXT_LEN - 2, seed=17) + UTF8_3BYTE[:2]
)
assert len(_malformed_3byte_partial_tail) == TEXT_LEN
add_text_get_vector(
    "text_get_malformed_trailing_3byte_partial",
    "S2 (as above, a different malformed shape -- 2 of 3 bytes of a CJK "
    "sequence present, final continuation byte missing -- width="
    "APAD_TEXT_LEN=60)",
    _malformed_3byte_partial_tail, TEXT_LEN,
)

# H.2.e -- a field of all NULs: the empty-string boundary on the receive
# side, matching text_set_empty_string_* on the send side.
add_text_get_vector(
    "text_get_all_nul_field", "S2 (a field of all NULs)",
    bytes(NAME_LEN), NAME_LEN,
)


# ---------------------------------------------------------------------------
# Section J: PROTOCOL.md S10.3 pairing URI (written concurrently with
# core/src/pair_uri.c by a separate author -- see this file's module
# docstring: this generator does not read
# core/src/pair_uri.c or anything else under core/src/, only docs/PROTOCOL.md
# S10.3 (the grammar and parser requirements) and S10.1 (the secret `s`
# carries).
#
# J.1 covers apad_pair_uri_parse: every "MUST parse, with the exact expected
# address/port/secret" case and every "MUST be REJECTED" case the task
# lists, plus the boundary pairs S10.3/S10.1 name explicitly (128 vs 129
# byte URI ceiling; 6 vs 5 and 64 vs 65 byte secret).
#
# J.2 covers apad_pair_uri_build round-tripping. exp_uri pins the EXACT
# bytes build() MUST produce -- taken directly from S10.3's own literal
# grammar box (`atticpad://<ipv4>:<port>/?v=1&s=<secret>`, scheme/host/port/
# path/v/s in that literal order), which is presented as the normative
# encoding, not one of several accepted forms. This is a stricter check than
# "build then parse recovers the same values" alone; both are given the
# task's own words ("build then parse must recover the same address and
# secret") only requires the round trip, but S10.3 gives a literal, single
# encoding to build against, so this file holds build() to it exactly.
#
# AMBIGUITY -- reported, not resolved by guessing (see the report handed
# back with this pass): S10.3 states a parser "MUST ignore query keys it
# does not recognise" but says nothing about a RECOGNISED key repeated (a
# second `s=` or `v=` in one URI). No vector below exercises a duplicate
# key; do not add one by inferring a rule S10.3 does not state.
# ---------------------------------------------------------------------------

SECRET_MIN_LEN = 6      # PROTOCOL.md S10.1 ("6 decimal digits", typed minimum)
SECRET_MAX_LEN = 64     # PROTOCOL.md S10.1 ("MUST accept ... any length from 6 to 64 bytes")
PAIR_URI_MAX_LEN = 128  # PROTOCOL.md S10.3 ("The whole URI MUST be <= 128 bytes")

# PROTOCOL.md S10.1 "Token alphabet", copied verbatim.
TOKEN_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
assert len(TOKEN_ALPHABET) == 32


def token_of_length(n):
    """A deterministic string of exactly n characters, entirely from S10.1's
    32-character token alphabet (cycled if n > 32). Only membership in the
    alphabet and the length are normative here -- the specific characters
    chosen are not."""
    return "".join(TOKEN_ALPHABET[i % 32] for i in range(n))


assert token_of_length(20) == "23456789ABCDEFGHJKLM"
assert len(token_of_length(64)) == 64
assert len(token_of_length(65)) == 65


def c_string_literal(s):
    """A double-quoted C string literal. Every string this section emits is
    plain ASCII (dotted-quad IPv4, decimal ports, the fixed atticpad/v/s
    tokens, and S10.1-range secrets) with one deliberate exception (a literal
    space in a REJECT vector's secret) -- handled here, not escaped away."""
    out = []
    for ch in s:
        if ch == '\\' or ch == '"':
            out.append('\\' + ch)
        elif 0x20 <= ord(ch) <= 0x7E:
            out.append(ch)
        else:
            out.append('\\x%02x' % ord(ch))
    return '"' + "".join(out) + '"'


# ---- J.1 apad_pair_uri_parse -----------------------------------------------

pair_uri_parse_vectors = []


def add_pair_uri_parse_vector(name, spec_ref, uri, exp_ok,
                               exp_ip=None, exp_port=None, exp_secret=None):
    assert '\x00' not in uri, "uri is a NUL-terminated C string"
    if exp_ok:
        assert exp_ip is not None and exp_port is not None and exp_secret is not None
    pair_uri_parse_vectors.append(dict(
        name=name, spec_ref=spec_ref, uri=uri, exp_ok=1 if exp_ok else 0,
        exp_ip=list(exp_ip) if exp_ip is not None else [0, 0, 0, 0],
        exp_port=exp_port if exp_port is not None else 0,
        exp_secret=exp_secret if exp_secret is not None else "",
    ))


# -- MUST parse ---------------------------------------------------------

_pin6 = "482913"
add_pair_uri_parse_vector(
    "pair_uri_canonical_6_digit_pin",
    "S10.3 grammar box + Parts table; S10.1 typed 6-digit PIN",
    "atticpad://192.168.1.42:21100/?v=1&s=%s" % _pin6, True,
    exp_ip=[192, 168, 1, 42], exp_port=21100, exp_secret=_pin6,
)

_token20 = token_of_length(20)
add_pair_uri_parse_vector(
    "pair_uri_canonical_20_char_token",
    "S10.3 grammar box; S10.1 scanned-token case (\"at least 16 characters\", "
    "20 used here)",
    "atticpad://10.1.2.3:21100/?v=1&s=%s" % _token20, True,
    exp_ip=[10, 1, 2, 3], exp_port=21100, exp_secret=_token20,
)

_secret64 = token_of_length(64)
add_pair_uri_parse_vector(
    "pair_uri_secret_64_byte_maximum",
    "S10.3 `s` = the S10.1 secret; S10.1 maximum length (64 bytes) -- "
    "boundary pair with pair_uri_secret_65_bytes_too_long below",
    "atticpad://10.1.2.3:21100/?v=1&s=%s" % _secret64, True,
    exp_ip=[10, 1, 2, 3], exp_port=21100, exp_secret=_secret64,
)

_secret6b = "778899"
add_pair_uri_parse_vector(
    "pair_uri_query_keys_opposite_order",
    "S10.3 (\"A parser MUST NOT require the query keys in any particular "
    "order\")",
    "atticpad://10.20.30.40:5000/?s=%s&v=1" % _secret6b, True,
    exp_ip=[10, 20, 30, 40], exp_port=5000, exp_secret=_secret6b,
)

add_pair_uri_parse_vector(
    "pair_uri_unknown_query_key_ignored",
    "S10.3 (\"A parser MUST ignore query keys it does not recognise\")",
    "atticpad://10.20.30.40:5000/?v=1&s=%s&x=whatever" % _secret6b, True,
    exp_ip=[10, 20, 30, 40], exp_port=5000, exp_secret=_secret6b,
)

add_pair_uri_parse_vector(
    "pair_uri_port_1_minimum",
    "S10.3 (\"port -- decimal, 1-65535, REQUIRED\", minimum)",
    "atticpad://1.2.3.4:1/?v=1&s=%s" % _secret6b, True,
    exp_ip=[1, 2, 3, 4], exp_port=1, exp_secret=_secret6b,
)

add_pair_uri_parse_vector(
    "pair_uri_port_65535_maximum",
    "S10.3 (\"port -- decimal, 1-65535, REQUIRED\", maximum)",
    "atticpad://1.2.3.4:65535/?v=1&s=%s" % _secret6b, True,
    exp_ip=[1, 2, 3, 4], exp_port=65535, exp_secret=_secret6b,
)


def _pad_with_ignored_key(base_uri, target_len):
    """Pad base_uri to exactly target_len bytes with an unknown, MUST-be-
    ignored query key (S10.3), isolating the S10.3 128-byte length ceiling
    from every other rule -- the padded URI is otherwise fully conforming."""
    pad_needed = target_len - len(base_uri) - len("&x=")
    assert pad_needed >= 0
    return base_uri + "&x=" + ("p" * pad_needed)


_base_for_padding = "atticpad://10.20.30.40:5000/?v=1&s=%s" % _secret6b
_uri_128 = _pad_with_ignored_key(_base_for_padding, PAIR_URI_MAX_LEN)
assert len(_uri_128) == PAIR_URI_MAX_LEN
add_pair_uri_parse_vector(
    "pair_uri_length_exactly_128_parses",
    "S10.3 (\"The whole URI MUST be <= 128 bytes\", exact ceiling; padded "
    "via an ignored unknown key so only the length rule is exercised) -- "
    "boundary pair with pair_uri_length_exactly_129_rejected below",
    _uri_128, True,
    exp_ip=[10, 20, 30, 40], exp_port=5000, exp_secret=_secret6b,
)

# -- MUST be REJECTED -----------------------------------------------------

add_pair_uri_parse_vector(
    "pair_uri_v_unrecognised_2",
    "S10.3 (\"A parser MUST reject a `v` it does not recognise, and MUST "
    "NOT attempt a partial interpretation\")",
    "atticpad://1.2.3.4:21100/?v=2&s=123456", False,
)
add_pair_uri_parse_vector(
    "pair_uri_v_unrecognised_99",
    "S10.3 (as above, a far-future version number)",
    "atticpad://1.2.3.4:21100/?v=99&s=123456", False,
)
add_pair_uri_parse_vector(
    "pair_uri_v_missing",
    "S10.3 (`v` is REQUIRED per the Parts table)",
    "atticpad://1.2.3.4:21100/?s=123456", False,
)
add_pair_uri_parse_vector(
    "pair_uri_s_missing",
    "S10.3 (`s` is REQUIRED per the Parts table)",
    "atticpad://1.2.3.4:21100/?v=1", False,
)
add_pair_uri_parse_vector(
    "pair_uri_hostname_instead_of_dotted_quad",
    "S10.3 (\"host -- an IPv4 dotted-quad literal. No hostnames\")",
    "atticpad://example.local:21100/?v=1&s=123456", False,
)
add_pair_uri_parse_vector(
    "pair_uri_no_port",
    "S10.3 (\"port ... REQUIRED\")",
    "atticpad://1.2.3.4/?v=1&s=123456", False,
)
add_pair_uri_parse_vector(
    "pair_uri_port_0",
    "S10.3 (\"port -- decimal, 1-65535\"; 0 is one below the minimum)",
    "atticpad://1.2.3.4:0/?v=1&s=123456", False,
)
add_pair_uri_parse_vector(
    "pair_uri_port_65536",
    "S10.3 (\"port -- decimal, 1-65535\"; 65536 is one past the maximum)",
    "atticpad://1.2.3.4:65536/?v=1&s=123456", False,
)

_secret5 = "12345"
assert len(_secret5) == SECRET_MIN_LEN - 1
add_pair_uri_parse_vector(
    "pair_uri_secret_5_bytes_too_short",
    "S10.3 `s` = the S10.1 secret; S10.1 minimum is 6 bytes -- boundary "
    "pair with pair_uri_canonical_6_digit_pin above",
    "atticpad://1.2.3.4:21100/?v=1&s=%s" % _secret5, False,
)

_secret65 = token_of_length(SECRET_MAX_LEN + 1)
assert len(_secret65) == SECRET_MAX_LEN + 1
add_pair_uri_parse_vector(
    "pair_uri_secret_65_bytes_too_long",
    "S10.3 `s` = the S10.1 secret; S10.1 maximum is 64 bytes -- boundary "
    "pair with pair_uri_secret_64_byte_maximum above",
    "atticpad://1.2.3.4:21100/?v=1&s=%s" % _secret65, False,
)

_secret_with_space = "12 345"
assert len(_secret_with_space) == 6
assert any(not (0x21 <= ord(c) <= 0x7E) for c in _secret_with_space)
add_pair_uri_parse_vector(
    "pair_uri_secret_byte_outside_printable_ascii",
    "S10.1 (\"The secret is printable ASCII (0x21-0x7E) ... 6 to 64 "
    "bytes\"; a literal space, 0x20, immediately below the range)",
    "atticpad://1.2.3.4:21100/?v=1&s=%s" % _secret_with_space, False,
)

_uri_129 = _uri_128 + "p"
assert len(_uri_129) == PAIR_URI_MAX_LEN + 1
add_pair_uri_parse_vector(
    "pair_uri_length_exactly_129_rejected",
    "S10.3 (\"The whole URI MUST be <= 128 bytes\", one byte past the "
    "ceiling; otherwise identical to pair_uri_length_exactly_128_parses)",
    _uri_129, False,
)

add_pair_uri_parse_vector(
    "pair_uri_wrong_scheme_http",
    "S10.3 (\"scheme -- atticpad, lowercase\")",
    "http://1.2.3.4:21100/?v=1&s=123456", False,
)
add_pair_uri_parse_vector(
    "pair_uri_wrong_scheme_single_slash",
    "S10.3 (grammar box requires \"atticpad://\" -- two slashes; one is "
    "missing here)",
    "atticpad:/1.2.3.4:21100/?v=1&s=123456", False,
)
add_pair_uri_parse_vector(
    "pair_uri_wrong_scheme_uppercase",
    "S10.3 (\"scheme -- atticpad, lowercase\"; this is uppercase)",
    "ATTICPAD://1.2.3.4:21100/?v=1&s=123456", False,
)

# ---- J.2 apad_pair_uri_build round-trip ------------------------------------

pair_uri_build_vectors = []


def add_pair_uri_build_vector(name, spec_ref, in_ip, in_port, in_secret):
    assert SECRET_MIN_LEN <= len(in_secret) <= SECRET_MAX_LEN
    exp_uri = "atticpad://%d.%d.%d.%d:%d/?v=1&s=%s" % (
        in_ip[0], in_ip[1], in_ip[2], in_ip[3], in_port, in_secret)
    assert len(exp_uri) <= PAIR_URI_MAX_LEN
    pair_uri_build_vectors.append(dict(
        name=name, spec_ref=spec_ref, in_ip=list(in_ip), in_port=in_port,
        in_secret=in_secret, exp_uri=exp_uri,
    ))


add_pair_uri_build_vector(
    "pair_uri_build_roundtrip_6_digit_pin",
    "S10.3 grammar box, build direction; S10.1 typed 6-digit PIN",
    [192, 168, 1, 42], 21100, "482913",
)
add_pair_uri_build_vector(
    "pair_uri_build_roundtrip_64_byte_secret",
    "S10.3 grammar box, build direction; S10.1 maximum secret length "
    "(64 bytes)",
    [10, 0, 0, 5], 9999, token_of_length(64),
)
add_pair_uri_build_vector(
    "pair_uri_build_roundtrip_port_1",
    "S10.3 grammar box, build direction; port minimum (1)",
    [1, 2, 3, 4], 1, "778899",
)
add_pair_uri_build_vector(
    "pair_uri_build_roundtrip_port_65535",
    "S10.3 grammar box, build direction; port maximum (65535)",
    [1, 2, 3, 4], 65535, "778899",
)


# ---------------------------------------------------------------------------
# C emission
# ---------------------------------------------------------------------------

def c_bytes_literal(data, indent="    "):
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append(indent + ", ".join("0x%02X" % b for b in chunk) + ",")
    return "\n".join(lines)


def c_ident(name):
    return "apad_vec_" + name


def c_bool(v):
    return "1" if v else "0"


def emit():
    out = []
    out.append("/*")
    out.append(" * core/testdata/vectors.h")
    out.append(" *")
    out.append(" * GENERATED FILE. Do not edit by hand -- edit")
    out.append(" * core/testdata/generate.py and re-run:")
    out.append(" *")
    out.append(" *     python3 core/testdata/generate.py")
    out.append(" *")
    out.append(" * Every byte pattern and expected value in this file is derived solely from")
    out.append(" * docs/PROTOCOL.md (the normative spec), by a generator that does not read")
    out.append(" * core/src/, core/include/, server/, clients/, or shim/. See docs/DESIGN.md S9.1")
    out.append(" * for why that independence matters: this file is")
    out.append(" * what runs as the on-device self-test (hold L+R+Start) on six platforms")
    out.append(" * nobody can otherwise test. (Section F additionally cross-checks Appendix A")
    out.append(" * against Python's own hashlib/hmac -- standard, independent crypto code, not")
    out.append(" * this project's implementation. See generate.py's Section F header comment.)")
    out.append(" *")
    out.append(" * Decoding rules assumed throughout (PROTOCOL.md S2, revised in this pass):")
    out.append(" * every field decodes as the literal bytes sent, EXCEPT fields the spec marks")
    out.append(" * reserved, which a receiver MUST SCRUB TO ZERO on decode -- not pass through")
    out.append(" * verbatim, and not reject the packet for. That covers: header flags bits")
    out.append(" * 2-15, INPUT_STATE buttons bits 20-31, axes[6]/axes[7], reserved0, and touch")
    out.append(" * entries at index >= touch_count. Three further transformations are explicit")
    out.append(" * in S5/S5.5 and are clamps/normalisations, not scrubs: touch_count > 2 -> 2,")
    out.append(" * negative axes[4]/axes[5] -> 0, and battery 101-254 -> 255 (S5.5, decode-time")
    out.append(" * normalisation, same category as the other two).")
    out.append(" *")
    out.append(" * Validation is a single ordered accept/discard/reject decision (PROTOCOL.md")
    out.append(" * S3.1, new in this revision): seven checks applied in order, stopping at the")
    out.append(" * first failure. Frame vectors below record exp_failed_check (0 = every check")
    out.append(" * passed) rather than independent per-aspect booleans.")
    out.append(" *")
    out.append(" * This is C99, self-contained: stdint.h only, no other includes, no libc")
    out.append(" * calls, no malloc, no floating point. Safe to compile into a 4 MB, no-MMU")
    out.append(" * target.")
    out.append(" */")
    out.append("")
    out.append("#ifndef ATTICPAD_TESTDATA_VECTORS_H")
    out.append("#define ATTICPAD_TESTDATA_VECTORS_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")

    # ---- protocol constants mirrored for the self-test's own use ----
    out.append("/* PROTOCOL.md S3 */")
    out.append("#define APAD_VEC_MAGIC 0x%04XU" % MAGIC)
    out.append("#define APAD_VEC_VERSION_1 %d" % VERSION_1)
    out.append("#define APAD_VEC_HEADER_LEN %d" % HEADER_LEN)
    out.append("#define APAD_VEC_TAG_LEN %d" % TAG_LEN)
    out.append("#define APAD_VEC_FLAG_AUTHENTICATED 0x%04XU" % FLAG_AUTHENTICATED)
    out.append("")
    out.append("/* PROTOCOL.md S3.1 check numbers and resulting actions/ERROR codes. */")
    out.append("#define APAD_VEC_CHECK_LENGTH_MIN      %d" % CHECK_LENGTH_MIN)
    out.append("#define APAD_VEC_CHECK_MAGIC           %d" % CHECK_MAGIC)
    out.append("#define APAD_VEC_CHECK_VERSION         %d" % CHECK_VERSION)
    out.append("#define APAD_VEC_CHECK_LENGTH_FORMULA  %d" % CHECK_LENGTH_FORMULA)
    out.append("#define APAD_VEC_CHECK_TYPE            %d" % CHECK_TYPE)
    out.append("#define APAD_VEC_CHECK_PAYLOAD_SIZE    %d" % CHECK_PAYLOAD_SIZE)
    out.append("#define APAD_VEC_CHECK_TAG             %d" % CHECK_TAG)
    out.append("#define APAD_VEC_ACTION_ACCEPT  %d" % ACTION_ACCEPT)
    out.append("#define APAD_VEC_ACTION_DISCARD %d" % ACTION_DISCARD)
    out.append("#define APAD_VEC_ACTION_REJECT  %d" % ACTION_REJECT)
    out.append("")
    out.append("/* PROTOCOL.md S5.1, apad_hat_lut -- copied verbatim */")
    out.append("static const uint8_t apad_vec_hat_lut_ref[16] = {")
    out.append("    " + ", ".join(str(x) for x in HAT_LUT))
    out.append("};")
    out.append("")

    # ---- Section A: frame vectors ----
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section A: header / framing vectors (PROTOCOL.md S3, S3.1, S4, S12)")
    out.append(" * S3.1's seven checks are applied in order; exp_failed_check is 0 if every")
    out.append(" * check passed (packet accepted), else the 1-based number of the first")
    out.append(" * check to fail. exp_action and exp_error_code follow mechanically from")
    out.append(" * exp_failed_check per the S3.1 table (see APAD_VEC_ACTION_* above).")
    out.append(" * The exp_version..exp_flags fields are meaningful ONLY when")
    out.append(" * exp_failed_check == 0.")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *bytes;")
    out.append("    uint32_t len;             /* logical datagram length fed to the decoder;")
    out.append("                                 may be less than sizeof(bytes)'s backing")
    out.append("                                 array for the 0-byte-datagram vector, whose")
    out.append("                                 array holds one unread placeholder byte */")
    out.append("    int exp_failed_check;      /* 0, or the S3.1 check number (1..6) that")
    out.append("                                   first fails. Never 7 here -- see the")
    out.append("                                   Section A header comment in generate.py. */")
    out.append("    int exp_action;            /* APAD_VEC_ACTION_* */")
    out.append("    int exp_error_code;        /* S6.11 ERROR code; 0 when exp_action != REJECT */")
    out.append("    uint8_t  exp_version;")
    out.append("    uint8_t  exp_type;")
    out.append("    uint16_t exp_session_id;")
    out.append("    uint16_t exp_sequence;")
    out.append("    uint16_t exp_payload_len;")
    out.append("    uint16_t exp_flags;")
    out.append("} apad_vec_frame;")
    out.append("")

    for fv in frame_vectors:
        ident = c_ident(fv['name'])
        out.append("/* %s */" % fv['spec_ref'])
        out.append("static const uint8_t %s_bytes[] = {" % ident)
        out.append(c_bytes_literal(fv['bytes']))
        out.append("};")
        out.append("")

    out.append("static const apad_vec_frame apad_frame_vectors[] = {")
    for fv in frame_vectors:
        ident = c_ident(fv['name'])
        ev = fv['exp_version'] if fv['exp_version'] is not None else 0
        et = fv['exp_type'] if fv['exp_type'] is not None else 0
        esid = fv['exp_session_id'] if fv['exp_session_id'] is not None else 0
        eseq = fv['exp_sequence'] if fv['exp_sequence'] is not None else 0
        epl = fv['exp_payload_len'] if fv['exp_payload_len'] is not None else 0
        efl = fv['exp_flags'] if fv['exp_flags'] is not None else 0
        out.append("    { \"%s\", %s_bytes, %dU, %d, %d, %d, %dU, %dU, %dU, %dU, %dU, %dU }," % (
            fv['name'], ident, fv['logical_len'],
            fv['exp_failed_check'], fv['exp_action'], fv['exp_error_code'],
            ev, et, esid, eseq, epl, efl,
        ))
    out.append("};")
    out.append("#define APAD_FRAME_VECTOR_COUNT %dU" % len(frame_vectors))
    out.append("")

    # ---- Section B: truncation ----
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section B: truncation sweep, every byte offset of the canonical")
    out.append(" * 68-byte unauthenticated INPUT_STATE datagram (PROTOCOL.md S3.1, S13")
    out.append(" * \"truncation at every byte offset\"). trunc_len in [0, 67] MUST fail per")
    out.append(" * exp_failed_check (never 0); trunc_len == 68 is the full, valid datagram")
    out.append(" * (see apad_vec_valid_input_state_frame in Section A, identical bytes).")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")
    out.append("static const uint8_t apad_vec_truncation_canonical[] = {")
    out.append(c_bytes_literal(CANONICAL_DATAGRAM))
    out.append("};")
    out.append("#define APAD_VEC_TRUNCATION_FULL_LEN %dU" % len(CANONICAL_DATAGRAM))
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    uint32_t trunc_len;    /* prefix length of apad_vec_truncation_canonical")
    out.append("                              to feed the decoder */")
    out.append("    int exp_failed_check;  /* S3.1 check number that MUST fail; never 0 */")
    out.append("} apad_vec_truncation;")
    out.append("")
    out.append("static const apad_vec_truncation apad_truncation_vectors[] = {")
    for tv in truncation_vectors:
        out.append("    { \"%s\", %dU, %d }," % (tv['name'], tv['trunc_len'], tv['exp_failed_check']))
    out.append("};")
    out.append("#define APAD_TRUNCATION_VECTOR_COUNT %dU" % len(truncation_vectors))
    out.append("")

    out.append("/* Second sweep: Appendix A's 28-byte AUTHENTICATED PING datagram (S14),")
    out.append(" * truncated at every offset -- exercises the \"+8\" tag arm of check 4,")
    out.append(" * including truncation points that land inside the tag itself. */")
    out.append("")
    out.append("static const uint8_t apad_vec_auth_truncation_canonical[] = {")
    out.append(c_bytes_literal(APPENDIX_A_DATAGRAM))
    out.append("};")
    out.append("#define APAD_VEC_AUTH_TRUNCATION_FULL_LEN %dU" % len(APPENDIX_A_DATAGRAM))
    out.append("")
    out.append("static const apad_vec_truncation apad_auth_truncation_vectors[] = {")
    for tv in auth_truncation_vectors:
        out.append("    { \"%s\", %dU, %d }," % (tv['name'], tv['trunc_len'], tv['exp_failed_check']))
    out.append("};")
    out.append("#define APAD_AUTH_TRUNCATION_VECTOR_COUNT %dU" % len(auth_truncation_vectors))
    out.append("")

    # ---- Section C: input state vectors ----
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section C: INPUT_STATE payload decode vectors (PROTOCOL.md S5, S2 scrub)")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")
    out.append("typedef struct {")
    out.append("    uint8_t id;")
    out.append("    uint8_t pressure;")
    out.append("    int16_t x;")
    out.append("    int16_t y;")
    out.append("} apad_vec_touch;")
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;  /* full 68-byte datagram: 12-byte header +")
    out.append("                               56-byte INPUT_STATE payload, unauthenticated */")
    out.append("    uint32_t packet_len;    /* always 68 */")
    out.append("    uint16_t exp_header_sequence;")
    out.append("    uint32_t exp_buttons;       /* bits 20..31 scrubbed to 0 (S5.1, S2) */")
    out.append("    int16_t  exp_axes[8];       /* [6],[7] scrubbed to 0 (S5, S2) */")
    out.append("    uint8_t  exp_touch_count;")
    out.append("    uint8_t  exp_reserved0;     /* always 0: scrubbed on decode (S5, S2) */")
    out.append("    apad_vec_touch exp_touches[2]; /* entries at index >= exp_touch_count")
    out.append("                                       scrubbed to all-zero (S5.2, S2) */")
    out.append("    int16_t  exp_accel[3];")
    out.append("    int16_t  exp_gyro[3];")
    out.append("    uint8_t  exp_battery;       /* 101-254 normalised to 255 (S5.5) */")
    out.append("    uint32_t exp_client_ticks_ms;")
    out.append("    uint8_t  exp_hat;  /* apad_vec_hat_lut_ref[(exp_buttons >> 4) & 0xF] */")
    out.append("} apad_vec_input_state;")
    out.append("")

    for iv in input_state_vectors:
        ident = c_ident(iv['name'])
        out.append("/* %s */" % iv['spec_ref'])
        out.append("static const uint8_t %s_packet[] = {" % ident)
        out.append(c_bytes_literal(iv['packet']))
        out.append("};")
        out.append("")

    out.append("static const apad_vec_input_state apad_input_state_vectors[] = {")
    for iv in input_state_vectors:
        ident = c_ident(iv['name'])
        axes_str = ", ".join(str(a) for a in iv['exp_axes'])
        touches_str = ", ".join(
            "{ %dU, %dU, %d, %d }" % t for t in iv['exp_touches']
        )
        accel_str = ", ".join(str(a) for a in iv['exp_accel'])
        gyro_str = ", ".join(str(a) for a in iv['exp_gyro'])
        out.append("    { \"%s\", %s_packet, %dU," % (iv['name'], ident, len(iv['packet'])))
        out.append("      %dU, %dU," % (iv['exp_header_sequence'], iv['exp_buttons']))
        out.append("      { %s }," % axes_str)
        out.append("      %dU, %dU," % (iv['exp_touch_count'], iv['exp_reserved0']))
        out.append("      { %s }," % touches_str)
        out.append("      { %s }," % accel_str)
        out.append("      { %s }," % gyro_str)
        out.append("      %dU, %dU, %dU }," % (
            iv['exp_battery'], iv['exp_client_ticks_ms'], iv['exp_hat']))
    out.append("};")
    out.append("#define APAD_INPUT_STATE_VECTOR_COUNT %dU" % len(input_state_vectors))
    out.append("")

    # ---- Section D: seq_newer ----
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section D: apad_seq_newer wrap-safe comparison (PROTOCOL.md S9)")
    out.append(" * Literal evaluations of the normative reference code in S9 (unsigned")
    out.append(" * arithmetic form -- see S9's rationale for why NOT the signed-cast form).")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    uint16_t a;")
    out.append("    uint16_t b;")
    out.append("    int expect_newer;  /* apad_seq_newer(a, b) per S9 */")
    out.append("} apad_vec_seq_newer;")
    out.append("")
    out.append("static const apad_vec_seq_newer apad_seq_newer_vectors[] = {")
    for v in seq_newer_vectors:
        out.append("    /* %s */" % v['spec_ref'])
        out.append("    { \"%s\", %dU, %dU, %s }," % (
            v['name'], v['a'], v['b'], c_bool(v['expect'])))
    out.append("};")
    out.append("#define APAD_SEQ_NEWER_VECTOR_COUNT %dU" % len(seq_newer_vectors))
    out.append("")

    # ---- Section E: time_after ----
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section E: apad_time_after wrap-safe comparison (PROTOCOL.md S9)")
    out.append(" * Literal evaluations of the normative reference code in S9 (unsigned")
    out.append(" * arithmetic form).")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    uint32_t a;")
    out.append("    uint32_t b;")
    out.append("    int expect_after;  /* apad_time_after(a, b) per S9 */")
    out.append("} apad_vec_time_after;")
    out.append("")
    out.append("static const apad_vec_time_after apad_time_after_vectors[] = {")
    for v in time_after_vectors:
        out.append("    /* %s */" % v['spec_ref'])
        out.append("    { \"%s\", %dUL, %dUL, %s }," % (
            v['name'], v['a'], v['b'], c_bool(v['expect'])))
    out.append("};")
    out.append("#define APAD_TIME_AFTER_VECTOR_COUNT %dU" % len(time_after_vectors))
    out.append("")

    # ---- Section F: Appendix A authentication vectors ----
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section F: Appendix A authentication vectors (PROTOCOL.md S10, S14 --")
    out.append(" * new in this revision). Fixed PIN/salt/iterations/derived key/tag, so a")
    out.append(" * byte-exact result is checkable without any implementation in view.")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")
    out.append("static const uint8_t apad_vec_appendix_a_pin[] = {")
    out.append(c_bytes_literal(APPENDIX_A_PIN))
    out.append("};")
    out.append("#define APAD_VEC_APPENDIX_A_PIN_LEN %dU" % len(APPENDIX_A_PIN))
    out.append("")
    out.append("static const uint8_t apad_vec_appendix_a_salt[] = {")
    out.append(c_bytes_literal(APPENDIX_A_SALT))
    out.append("};")
    out.append("#define APAD_VEC_APPENDIX_A_SALT_LEN %dU" % len(APPENDIX_A_SALT))
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *pin;")
    out.append("    uint32_t pin_len;")
    out.append("    const uint8_t *salt;")
    out.append("    uint32_t salt_len;")
    out.append("    uint32_t iterations;")
    out.append("    const uint8_t *expected_key;  /* 32 bytes */")
    out.append("} apad_vec_pbkdf2;")
    out.append("")
    for pv in pbkdf2_vectors:
        ident = c_ident(pv['name'])
        out.append("/* %s */" % pv['spec_ref'])
        out.append("static const uint8_t %s_key[] = {" % ident)
        out.append(c_bytes_literal(pv['expected_key']))
        out.append("};")
        out.append("")
    out.append("static const apad_vec_pbkdf2 apad_pbkdf2_vectors[] = {")
    for pv in pbkdf2_vectors:
        ident = c_ident(pv['name'])
        out.append("    { \"%s\", apad_vec_appendix_a_pin, %dU, apad_vec_appendix_a_salt, %dU, %dUL, %s_key }," % (
            pv['name'], len(pv['pin']), len(pv['salt']), pv['iterations'], ident))
    out.append("};")
    out.append("#define APAD_PBKDF2_VECTOR_COUNT %dU" % len(pbkdf2_vectors))
    out.append("")

    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *key;        /* 32 bytes, APAD_SESSION_KEY_LEN */")
    out.append("    const uint8_t *datagram;   /* full datagram, header + payload + tag */")
    out.append("    uint32_t datagram_len;")
    out.append("    int exp_verify_ok;         /* 1 = tag MUST verify; 0 = MUST fail */")
    out.append("} apad_vec_auth_tag;")
    out.append("")
    out.append("static const uint8_t apad_vec_appendix_a_key[] = {")
    out.append(c_bytes_literal(APPENDIX_A_KEY))
    out.append("};")
    out.append("")
    for av in auth_tag_vectors:
        ident = c_ident(av['name'])
        out.append("/* %s */" % av['spec_ref'])
        out.append("static const uint8_t %s_datagram[] = {" % ident)
        out.append(c_bytes_literal(av['datagram']))
        out.append("};")
        out.append("")
    out.append("static const apad_vec_auth_tag apad_auth_tag_vectors[] = {")
    for av in auth_tag_vectors:
        ident = c_ident(av['name'])
        out.append("    { \"%s\", apad_vec_appendix_a_key, %s_datagram, %dU, %s }," % (
            av['name'], ident, len(av['datagram']), c_bool(av['exp_verify_ok'])))
    out.append("};")
    out.append("#define APAD_AUTH_TAG_VECTOR_COUNT %dU" % len(auth_tag_vectors))
    out.append("")

    # ---- Section I: S10.1 secret-length boundary vectors ----
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section I: PROTOCOL.md S10.1 secret-length boundary vectors (S10.1 is")
    out.append(" * NEW in this revision). \"A conforming implementation MUST accept a")
    out.append(" * secret of any length from 6 to 64 bytes and MUST NOT assume six")
    out.append(" * digits.\" Every entry reuses Appendix A's server_nonce as the PBKDF2")
    out.append(" * salt (S14) so every case shares one known input. expected_key for")
    out.append(" * every length other than 6 is computed directly by generate.py with")
    out.append(" * Python's stdlib hashlib.pbkdf2_hmac -- independent crypto, not")
    out.append(" * core/src (see generate.py's Section I header comment, which also")
    out.append(" * records a token-alphabet ambiguity S10.1 leaves open: byte CONTENT")
    out.append(" * below is arbitrary deterministic filler, only LENGTH is normative).")
    out.append(" * Reuses apad_vec_pbkdf2 (Section F's struct shape, unchanged).")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")
    for pv in secret_length_vectors:
        ident = c_ident(pv['name'])
        out.append("/* %s */" % pv['spec_ref'])
        out.append("static const uint8_t %s_pin[] = {" % ident)
        out.append(c_bytes_literal(pv['pin'] + bytes(1)))  # NUL-terminated C string
        out.append("};")
        out.append("static const uint8_t %s_key[] = {" % ident)
        out.append(c_bytes_literal(pv['expected_key']))
        out.append("};")
        out.append("")
    out.append("static const apad_vec_pbkdf2 apad_secret_length_vectors[] = {")
    for pv in secret_length_vectors:
        ident = c_ident(pv['name'])
        out.append("    { \"%s\", %s_pin, %dU, apad_vec_appendix_a_salt, %dU, %dUL, %s_key }," % (
            pv['name'], ident, len(pv['pin']), len(pv['salt']), pv['iterations'], ident))
    out.append("};")
    out.append("#define APAD_SECRET_LENGTH_VECTOR_COUNT %dU" % len(secret_length_vectors))
    out.append("")

    # ---- Section G: the eleven S6 payloads ----
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section G: S6 payload vectors -- ANNOUNCE, HELLO, WELCOME, BYE, PING,")
    out.append(" * PONG, RUMBLE, LED, STATUS, ACK, ERROR (PROTOCOL.md S6.2-S6.11).")
    out.append(" *")
    out.append(" * Every field is a distinctive, non-zero, non-repeating value -- never")
    out.append(" * all-zero, which would pass against a decoder reading the wrong offset.")
    out.append(" * Every named reserved0/reserved1 byte is sent non-zero on the wire and")
    out.append(" * MUST decode as zero (S2 scrub). Type-specific receive-side rules are")
    out.append(" * exercised at their boundaries: S6.2 pairing_required (any non-zero -> 1),")
    out.append(" * S6.3 caps bits 14-31 (masked off), S6.4 key_material (MUST be all-zero")
    out.append(" * on decode AND MUST be forced to zero on encode regardless of input --")
    out.append(" * see apad_welcome_encode_vectors for the encode side), S6.8")
    out.append(" * player_index (>4 -> 0, but 0 itself is untouched).")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")

    def emit_packet_arrays(vectors):
        for v in vectors:
            ident = c_ident(v['name'])
            out.append("/* %s */" % v['spec_ref'])
            out.append("static const uint8_t %s_packet[] = {" % ident)
            out.append(c_bytes_literal(v['packet']))
            out.append("};")
            out.append("")

    def u8_inline(data):
        return "{ " + ", ".join("0x%02X" % b for b in data) + " }"

    # -- G.1 ANNOUNCE --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint8_t  exp_server_name[32];")
    out.append("    uint8_t  exp_pads_total;")
    out.append("    uint8_t  exp_pads_free;")
    out.append("    uint8_t  exp_pairing_required;  /* S6.2: any non-zero on wire -> 1 */")
    out.append("    uint16_t exp_server_port;")
    out.append("} apad_vec_announce;")
    out.append("")
    emit_packet_arrays(announce_vectors)
    out.append("static const apad_vec_announce apad_announce_vectors[] = {")
    for v in announce_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU," % (v['name'], ident, len(v['packet'])))
        out.append("      %s, %dU, %dU, %dU, %dU }," % (
            u8_inline(v['exp_server_name']), v['exp_pads_total'], v['exp_pads_free'],
            v['exp_pairing_required'], v['exp_server_port']))
    out.append("};")
    out.append("#define APAD_ANNOUNCE_VECTOR_COUNT %dU" % len(announce_vectors))
    out.append("")

    # -- G.2 HELLO --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint8_t  exp_client_id[16];")
    out.append("    uint32_t exp_caps;              /* bits 14..31 masked off (S6.3) */")
    out.append("    uint8_t  exp_device_name[32];")
    out.append("    uint8_t  exp_client_nonce[16];")
    out.append("    uint16_t exp_desired_rate_hz;")
    out.append("    uint8_t  exp_proto_major;")
    out.append("    uint32_t exp_client_ticks_ms;")
    out.append("} apad_vec_hello;")
    out.append("")
    emit_packet_arrays(hello_vectors)
    out.append("static const apad_vec_hello apad_hello_vectors[] = {")
    for v in hello_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU," % (v['name'], ident, len(v['packet'])))
        out.append("      %s, %dUL," % (u8_inline(v['exp_client_id']), v['exp_caps']))
        out.append("      %s," % u8_inline(v['exp_device_name']))
        out.append("      %s," % u8_inline(v['exp_client_nonce']))
        out.append("      %dU, %dU, %dUL }," % (
            v['exp_desired_rate_hz'], v['exp_proto_major'], v['exp_client_ticks_ms']))
    out.append("};")
    out.append("#define APAD_HELLO_VECTOR_COUNT %dU" % len(hello_vectors))
    out.append("")

    # -- G.3 WELCOME (decode) --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint16_t exp_session_id;")
    out.append("    uint8_t  exp_pad_slot;")
    out.append("    uint8_t  exp_flags;             /* bits 1..7 masked off (S6.4) */")
    out.append("    uint16_t exp_input_rate_hz;")
    out.append("    uint8_t  exp_server_nonce[16];")
    out.append("    uint8_t  exp_key_material[32];  /* MUST be all-zero (S6.4) */")
    out.append("    uint32_t exp_server_ticks_ms;")
    out.append("} apad_vec_welcome;")
    out.append("")
    emit_packet_arrays(welcome_vectors)
    out.append("static const apad_vec_welcome apad_welcome_vectors[] = {")
    for v in welcome_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU," % (v['name'], ident, len(v['packet'])))
        out.append("      %dU, %dU, %dU, %dU," % (
            v['exp_session_id'], v['exp_pad_slot'], v['exp_flags'], v['exp_input_rate_hz']))
        out.append("      %s," % u8_inline(v['exp_server_nonce']))
        out.append("      %s," % u8_inline(v['exp_key_material']))
        out.append("      %dUL }," % v['exp_server_ticks_ms'])
    out.append("};")
    out.append("#define APAD_WELCOME_VECTOR_COUNT %dU" % len(welcome_vectors))
    out.append("")

    # -- G.3b WELCOME (encode) --
    out.append("/* S6.4 key_material, SEND side: a decoder-only vectors.h can't drive an")
    out.append(" * encoder itself, so this gives the raw input field values a self-test")
    out.append(" * harness uses to build an apad_welcome, plus the exact 60-byte payload")
    out.append(" * apad_encode_welcome MUST produce -- key_material forced to zero even")
    out.append(" * though in_key_material below is deliberately non-zero. */")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    uint16_t in_session_id;")
    out.append("    uint8_t  in_pad_slot;")
    out.append("    uint8_t  in_flags;")
    out.append("    uint16_t in_input_rate_hz;")
    out.append("    uint8_t  in_server_nonce[16];")
    out.append("    uint8_t  in_key_material[32];   /* non-zero on purpose */")
    out.append("    uint32_t in_server_ticks_ms;")
    out.append("    const uint8_t *exp_payload;     /* required encoded 60 bytes */")
    out.append("    uint32_t exp_payload_len;")
    out.append("} apad_vec_welcome_encode;")
    out.append("")
    for v in welcome_encode_vectors:
        ident = c_ident(v['name'])
        out.append("/* %s */" % v['spec_ref'])
        out.append("static const uint8_t %s_exp_payload[] = {" % ident)
        out.append(c_bytes_literal(v['exp_payload']))
        out.append("};")
        out.append("")
    out.append("static const apad_vec_welcome_encode apad_welcome_encode_vectors[] = {")
    for v in welcome_encode_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %dU, %dU, %dU, %dU," % (
            v['name'], v['in_session_id'], v['in_pad_slot'], v['in_flags'], v['in_input_rate_hz']))
        out.append("      %s," % u8_inline(v['in_server_nonce']))
        out.append("      %s," % u8_inline(v['in_key_material']))
        out.append("      %dUL, %s_exp_payload, %dU }," % (
            v['in_server_ticks_ms'], ident, len(v['exp_payload'])))
    out.append("};")
    out.append("#define APAD_WELCOME_ENCODE_VECTOR_COUNT %dU" % len(welcome_encode_vectors))
    out.append("")

    # -- G.4 BYE --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint8_t  exp_reason;")
    out.append("} apad_vec_bye;")
    out.append("")
    emit_packet_arrays(bye_vectors)
    out.append("static const apad_vec_bye apad_bye_vectors[] = {")
    for v in bye_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU, %dU }," % (
            v['name'], ident, len(v['packet']), v['exp_reason']))
    out.append("};")
    out.append("#define APAD_BYE_VECTOR_COUNT %dU" % len(bye_vectors))
    out.append("")

    # -- G.5/G.6 PING / PONG (shared struct shape) --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint32_t exp_origin_ticks_ms;")
    out.append("    uint32_t exp_responder_ticks_ms;")
    out.append("} apad_vec_ping;  /* shared shape: also used for the PONG vectors below */")
    out.append("")
    emit_packet_arrays(ping_vectors)
    out.append("static const apad_vec_ping apad_ping_vectors[] = {")
    for v in ping_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU, %dUL, %dUL }," % (
            v['name'], ident, len(v['packet']), v['exp_origin_ticks_ms'],
            v['exp_responder_ticks_ms']))
    out.append("};")
    out.append("#define APAD_PING_VECTOR_COUNT %dU" % len(ping_vectors))
    out.append("")
    emit_packet_arrays(pong_vectors)
    out.append("static const apad_vec_ping apad_pong_vectors[] = {")
    for v in pong_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU, %dUL, %dUL }," % (
            v['name'], ident, len(v['packet']), v['exp_origin_ticks_ms'],
            v['exp_responder_ticks_ms']))
    out.append("};")
    out.append("#define APAD_PONG_VECTOR_COUNT %dU" % len(pong_vectors))
    out.append("")

    # -- G.7 RUMBLE --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint16_t exp_low_freq;")
    out.append("    uint16_t exp_high_freq;")
    out.append("    uint16_t exp_duration_ms;")
    out.append("} apad_vec_rumble;")
    out.append("")
    emit_packet_arrays(rumble_vectors)
    out.append("static const apad_vec_rumble apad_rumble_vectors[] = {")
    for v in rumble_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU, %dU, %dU, %dU }," % (
            v['name'], ident, len(v['packet']), v['exp_low_freq'], v['exp_high_freq'],
            v['exp_duration_ms']))
    out.append("};")
    out.append("#define APAD_RUMBLE_VECTOR_COUNT %dU" % len(rumble_vectors))
    out.append("")

    # -- G.8 LED --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint8_t  exp_player_index;  /* >4 normalised to 0 (S6.8); the LED clamp")
    out.append("                                   the pre-freeze audit found missing */")
    out.append("    uint8_t  exp_r;")
    out.append("    uint8_t  exp_g;")
    out.append("    uint8_t  exp_b;")
    out.append("} apad_vec_led;")
    out.append("")
    emit_packet_arrays(led_vectors)
    out.append("static const apad_vec_led apad_led_vectors[] = {")
    for v in led_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU, %dU, %dU, %dU, %dU }," % (
            v['name'], ident, len(v['packet']), v['exp_player_index'], v['exp_r'], v['exp_g'],
            v['exp_b']))
    out.append("};")
    out.append("#define APAD_LED_VECTOR_COUNT %dU" % len(led_vectors))
    out.append("")

    # -- G.9 STATUS --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint8_t  exp_code;")
    out.append("    uint8_t  exp_text[60];")
    out.append("} apad_vec_status;")
    out.append("")
    emit_packet_arrays(status_vectors)
    out.append("static const apad_vec_status apad_status_vectors[] = {")
    for v in status_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU, %dU," % (
            v['name'], ident, len(v['packet']), v['exp_code']))
        out.append("      %s }," % u8_inline(v['exp_text']))
    out.append("};")
    out.append("#define APAD_STATUS_VECTOR_COUNT %dU" % len(status_vectors))
    out.append("")

    # -- G.10 ACK --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint16_t exp_sequence;")
    out.append("} apad_vec_ack;")
    out.append("")
    emit_packet_arrays(ack_vectors)
    out.append("static const apad_vec_ack apad_ack_vectors[] = {")
    for v in ack_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU, %dU }," % (
            v['name'], ident, len(v['packet']), v['exp_sequence']))
    out.append("};")
    out.append("#define APAD_ACK_VECTOR_COUNT %dU" % len(ack_vectors))
    out.append("")

    # -- G.11 ERROR --
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *packet;")
    out.append("    uint32_t packet_len;")
    out.append("    uint16_t exp_code;")
    out.append("    uint8_t  exp_text[60];")
    out.append("} apad_vec_error;")
    out.append("")
    emit_packet_arrays(error_vectors)
    out.append("static const apad_vec_error apad_error_vectors[] = {")
    for v in error_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_packet, %dU, %dU," % (
            v['name'], ident, len(v['packet']), v['exp_code']))
        out.append("      %s }," % u8_inline(v['exp_text']))
    out.append("};")
    out.append("#define APAD_ERROR_VECTOR_COUNT %dU" % len(error_vectors))
    out.append("")

    # ---- Section H: apad_text_set / apad_text_len / apad_text_get -------
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section H: apad_text_set / apad_text_len / apad_text_get -- PROTOCOL.md")
    out.append(" * S2's text-field paragraph, all four obligations. H.1 (SET) is the sender")
    out.append(" * truncation rule this section exists to cover (\"MUST NOT split a")
    out.append(" * multi-byte UTF-8 sequence across the end of the field -- truncate at a")
    out.append(" * character boundary and NUL-pad the remainder\"); H.2 (GET/LEN) covers the")
    out.append(" * bounded-width and malformed-trailing-sequence-tolerance obligations,")
    out.append(" * calling apad_text_len/apad_text_get directly rather than only through a")
    out.append(" * full HELLO/STATUS/ERROR payload decode (see generate.py's Section H")
    out.append(" * header comment for the full obligation-to-vector mapping).")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")

    # -- H.1 apad_text_set (encode side) --
    out.append("/* apad_text_set can't be driven by a decoder-only vectors.h any more than")
    out.append(" * apad_encode_welcome could (see apad_vec_welcome_encode above for that")
    out.append(" * precedent): in_src is the NUL-terminated C string a self-test harness")
    out.append(" * passes to apad_text_set(field, width, in_src); exp_field is the exact")
    out.append(" * `width`-byte result apad_text_set MUST produce, computed independently")
    out.append(" * from the S2 rule text by generate.py's _text_set_reference. */")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const char *in_src;       /* NUL-terminated C string fed to apad_text_set */")
    out.append("    uint32_t width;           /* APAD_NAME_LEN (32) or APAD_TEXT_LEN (60) */")
    out.append("    const uint8_t *exp_field; /* exactly `width` bytes apad_text_set MUST write */")
    out.append("} apad_vec_text_set;")
    out.append("")
    for v in text_set_vectors:
        ident = c_ident(v['name'])
        out.append("/* %s */" % v['spec_ref'])
        out.append("static const uint8_t %s_src[] = {" % ident)
        out.append(c_bytes_literal(v['in_src'] + bytes(1)))  # NUL-terminated C string
        out.append("};")
        out.append("static const uint8_t %s_exp_field[] = {" % ident)
        out.append(c_bytes_literal(v['exp_field']))
        out.append("};")
        out.append("")
    out.append("static const apad_vec_text_set apad_text_set_vectors[] = {")
    for v in text_set_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", (const char *)%s_src, %dU, %s_exp_field }," % (
            v['name'], ident, v['width'], ident))
    out.append("};")
    out.append("#define APAD_TEXT_SET_VECTOR_COUNT %dU" % len(text_set_vectors))
    out.append("")

    # -- H.2 apad_text_len / apad_text_get (decode side) --
    out.append("/* in_field is exactly `width` bytes, as if read off the wire (never")
    out.append(" * decoded/scrubbed -- these vectors call apad_text_len/apad_text_get")
    out.append(" * directly on raw field bytes, including malformed ones a non-conforming")
    out.append(" * sender could have produced). exp_len is what apad_text_len(in_field,")
    out.append(" * width) MUST return; exp_cstr/exp_cstr_len is the NUL-terminated buffer")
    out.append(" * apad_text_get(dst, dst_cap, in_field, width) MUST produce -- dst_cap is")
    out.append(" * generous throughout (width + 1) so these vectors test only the S2")
    out.append(" * text-field rule, not apad_text_get's separate dst_cap-clamping")
    out.append(" * contract. */")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const uint8_t *in_field;")
    out.append("    uint32_t width;")
    out.append("    uint32_t dst_cap;")
    out.append("    uint32_t exp_len;")
    out.append("    const uint8_t *exp_cstr;")
    out.append("    uint32_t exp_cstr_len;")
    out.append("} apad_vec_text_get;")
    out.append("")
    for v in text_get_vectors:
        ident = c_ident(v['name'])
        out.append("/* %s */" % v['spec_ref'])
        out.append("static const uint8_t %s_field[] = {" % ident)
        out.append(c_bytes_literal(v['in_field']))
        out.append("};")
        out.append("static const uint8_t %s_exp_cstr[] = {" % ident)
        out.append(c_bytes_literal(v['exp_cstr']))
        out.append("};")
        out.append("")
    out.append("static const apad_vec_text_get apad_text_get_vectors[] = {")
    for v in text_get_vectors:
        ident = c_ident(v['name'])
        out.append("    { \"%s\", %s_field, %dU, %dU, %dU, %s_exp_cstr, %dU }," % (
            v['name'], ident, v['width'], v['dst_cap'], v['exp_len'], ident,
            len(v['exp_cstr'])))
    out.append("};")
    out.append("#define APAD_TEXT_GET_VECTOR_COUNT %dU" % len(text_get_vectors))
    out.append("")

    # ---- Section J: pairing URI (S10.3) ----
    out.append("/* ----------------------------------------------------------------------")
    out.append(" * Section J: PROTOCOL.md S10.3 pairing URI (NEW today). J.1 exercises")
    out.append(" * apad_pair_uri_parse in both directions (MUST parse / MUST reject) plus")
    out.append(" * every boundary S10.1/S10.3 name explicitly. J.2 exercises")
    out.append(" * apad_pair_uri_build: exp_uri is the EXACT byte sequence build() MUST")
    out.append(" * produce, taken directly from S10.3's own literal grammar box.")
    out.append(" *")
    out.append(" * AMBIGUITY (reported, not resolved here): S10.3 does not say what a")
    out.append(" * parser MUST do with a RECOGNISED query key repeated (e.g. two `s=` in")
    out.append(" * one URI). No vector below exercises that case.")
    out.append(" * ---------------------------------------------------------------------- */")
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    const char *uri;         /* NUL-terminated input to apad_pair_uri_parse */")
    out.append("    uint32_t    uri_len;     /* strlen(uri) */")
    out.append("    uint32_t    exp_ok;      /* 1 = MUST parse, 0 = MUST be rejected */")
    out.append("    uint8_t     exp_ip[4];   /* meaningful only when exp_ok == 1 */")
    out.append("    uint16_t    exp_port;    /* meaningful only when exp_ok == 1 */")
    out.append("    const char *exp_secret;  /* meaningful only when exp_ok == 1, NUL-terminated */")
    out.append("} apad_vec_pair_uri_parse;")
    out.append("")
    out.append("static const apad_vec_pair_uri_parse apad_pair_uri_parse_vectors[] = {")
    for v in pair_uri_parse_vectors:
        out.append("    /* %s */" % v['spec_ref'])
        out.append("    { \"%s\", %s, %dU, %dU," % (
            v['name'], c_string_literal(v['uri']), len(v['uri']), v['exp_ok']))
        out.append("      { %dU, %dU, %dU, %dU }, %dU, %s }," % (
            v['exp_ip'][0], v['exp_ip'][1], v['exp_ip'][2], v['exp_ip'][3],
            v['exp_port'], c_string_literal(v['exp_secret'])))
    out.append("};")
    out.append("#define APAD_PAIR_URI_PARSE_VECTOR_COUNT %dU" % len(pair_uri_parse_vectors))
    out.append("")

    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    uint8_t     in_ip[4];")
    out.append("    uint16_t    in_port;")
    out.append("    const char *in_secret;   /* NUL-terminated */")
    out.append("    const char *exp_uri;     /* exact NUL-terminated string apad_pair_uri_build MUST produce */")
    out.append("    uint32_t    exp_uri_len; /* strlen(exp_uri) */")
    out.append("} apad_vec_pair_uri_build;")
    out.append("")
    out.append("static const apad_vec_pair_uri_build apad_pair_uri_build_vectors[] = {")
    for v in pair_uri_build_vectors:
        out.append("    /* %s */" % v['spec_ref'])
        out.append("    { \"%s\", { %dU, %dU, %dU, %dU }, %dU, %s," % (
            v['name'], v['in_ip'][0], v['in_ip'][1], v['in_ip'][2], v['in_ip'][3],
            v['in_port'], c_string_literal(v['in_secret'])))
        out.append("      %s, %dU }," % (c_string_literal(v['exp_uri']), len(v['exp_uri'])))
    out.append("};")
    out.append("#define APAD_PAIR_URI_BUILD_VECTOR_COUNT %dU" % len(pair_uri_build_vectors))
    out.append("")

    out.append("#endif /* ATTICPAD_TESTDATA_VECTORS_H */")
    out.append("")
    return "\n".join(out)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out_path = os.path.join(repo_root, "core", "testdata", "vectors.h")
    content = emit()
    with open(out_path, "w") as f:
        f.write(content)
    print("wrote %s (%d bytes)" % (out_path, len(content)))
    print("frame vectors:            %d" % len(frame_vectors))
    print("truncation vectors:       %d" % len(truncation_vectors))
    print("auth truncation vectors:  %d" % len(auth_truncation_vectors))
    print("input_state vectors:      %d" % len(input_state_vectors))
    print("seq_newer vectors:        %d" % len(seq_newer_vectors))
    print("time_after vectors:       %d" % len(time_after_vectors))
    print("pbkdf2 vectors:           %d" % len(pbkdf2_vectors))
    print("auth tag vectors:         %d" % len(auth_tag_vectors))
    print("secret length vectors:    %d" % len(secret_length_vectors))
    print("announce vectors:         %d" % len(announce_vectors))
    print("hello vectors:            %d" % len(hello_vectors))
    print("welcome vectors:          %d" % len(welcome_vectors))
    print("welcome encode vectors:   %d" % len(welcome_encode_vectors))
    print("bye vectors:              %d" % len(bye_vectors))
    print("ping vectors:             %d" % len(ping_vectors))
    print("pong vectors:             %d" % len(pong_vectors))
    print("rumble vectors:           %d" % len(rumble_vectors))
    print("led vectors:              %d" % len(led_vectors))
    print("status vectors:           %d" % len(status_vectors))
    print("ack vectors:              %d" % len(ack_vectors))
    print("error vectors:            %d" % len(error_vectors))
    section_g_total = (len(announce_vectors) + len(hello_vectors) + len(welcome_vectors)
                        + len(welcome_encode_vectors) + len(bye_vectors) + len(ping_vectors)
                        + len(pong_vectors) + len(rumble_vectors) + len(led_vectors)
                        + len(status_vectors) + len(ack_vectors) + len(error_vectors))
    print("Section G total:          %d" % section_g_total)
    print("text_set vectors:         %d" % len(text_set_vectors))
    print("text_get vectors:         %d" % len(text_get_vectors))
    section_h_total = len(text_set_vectors) + len(text_get_vectors)
    print("Section H total:          %d" % section_h_total)
    print("pair_uri parse vectors:   %d" % len(pair_uri_parse_vectors))
    print("pair_uri build vectors:   %d" % len(pair_uri_build_vectors))
    section_j_total = len(pair_uri_parse_vectors) + len(pair_uri_build_vectors)
    print("Section J total:          %d" % section_j_total)
    total = (len(frame_vectors) + len(truncation_vectors) + len(auth_truncation_vectors)
             + len(input_state_vectors) + len(seq_newer_vectors) + len(time_after_vectors)
             + len(pbkdf2_vectors) + len(auth_tag_vectors) + len(secret_length_vectors)
             + section_g_total + section_h_total + section_j_total)
    print("TOTAL:                    %d" % total)


if __name__ == "__main__":
    main()
