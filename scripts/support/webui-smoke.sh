#!/usr/bin/env bash
# scripts/support/webui-smoke.sh — manual smoke test for the server's web
# API (server/host/common/webui.h), covering the profile-editor routes end
# to end: GET /api/state, GET /api/profiles, GET/PUT/DELETE
# /api/profile/{name}, POST /api/profiles/reload, GET /api/client/{slot}/
# input, POST /api/client/{slot}/profile.
#
# This is NOT part of the <10s core loop and NOT wired into any CI
# workflow — a manually-run helper only (see docs/CONVENTIONS.md "Build and test").
# Run it by hand after touching webui.h/profile_store.h/profile_json.h to
# re-verify the routes a browser-based editor will actually hit.
#
# Usage:
#   scripts/support/webui-smoke.sh
#
# Exit 0: every check PASSed (or SKIPped, see below). Exit 1: at least one
# FAIL.
#
# SCRATCH ONLY — this script must never touch the dev box's long-lived
# server (UDP 21100 / UI 21150) or the repo's own server/profiles/
# directory. It binds two scratch ports (21377 UDP / 21378 UI) and copies
# server/profiles/*.jsonc into a mktemp'd scratch directory instead.
#
# LIVE SESSION, and how this script gets one: tools/loopback-client has no
# "hold the session open" flag — its --target mode runs one fixed S8.1
# scenario (DISCOVER, HELLO/WELCOME, duplicate-HELLO, then >3200ms of
# INPUT_STATE streaming past the S11 idle timeout, then a short burst,
# PING/PONG, BYE) and exits. Rather than loop or background several runs,
# this script runs ONE loopback-client in the background and structures
# every check that needs a live session (c: input polling, h: reload while
# connected) around that >3200ms streaming window, detected by POLLING THE
# SERVER's own state (GET /api/state's clients array, then GET
# /api/client/{slot}/input's frame counter) rather than the other
# process's stdout — see the comment right before wait_for_first_client()
# below for why stdout-scraping does not work here.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"

UDP_PORT=21377
UI_PORT=21378
BASE="http://127.0.0.1:${UI_PORT}"

SCRATCH_DIR="$(mktemp -d "${TMPDIR:-/tmp}/atticpad-webui-smoke.XXXXXX")"
PROFILES_DIR="${SCRATCH_DIR}/profiles"
BUILD_DIR="${SCRATCH_DIR}/build"
SERVER_LOG="${SCRATCH_DIR}/server.log"
LOOPBACK_LOG="${SCRATCH_DIR}/loopback.log"

SERVER_PID=""
LOOPBACK_PID=""

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

log()  { printf '== webui-smoke: %s ==\n' "$*"; }
warn() { printf 'webui-smoke: WARNING: %s\n' "$*" >&2; }

pass() { PASS_COUNT=$((PASS_COUNT + 1)); printf '[PASS] %s) %s\n' "$1" "$2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); printf '[FAIL] %s) %s\n' "$1" "$2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); printf '[SKIP] %s) %s\n' "$1" "$2"; }

# ---------------------------------------------------------------------------
# cleanup — runs on every exit path (normal completion, a FAIL that does not
# abort the script, or an unexpected error under set -e). NEVER touches
# 21100/21150 or server/profiles/: only the PIDs and scratch dir this script
# itself created.
# ---------------------------------------------------------------------------
cleanup() {
  local rc=$?
  if [[ -n "${LOOPBACK_PID}" ]] && kill -0 "${LOOPBACK_PID}" 2>/dev/null; then
    kill -TERM "${LOOPBACK_PID}" 2>/dev/null || true
    wait "${LOOPBACK_PID}" 2>/dev/null || true
  fi
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill -TERM "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -rf "${SCRATCH_DIR}"
  exit "${rc}"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# 1. build server + tools/loopback-client
#
# Always rebuilt (not "if missing"): webui.h is actively being edited by
# another agent per this task's brief (Host/Origin hardening), and a stale
# binary would make checks j/k lie about what is actually landed. Isolated
# APAD_BUILD_DIR under the scratch dir so this never races another agent's
# build in the shared build/ (scripts/build.sh's own convention for this).
#
# Only tools/loopback-client is built directly (not `scripts/build.sh
# tools`, which also builds tools/fuzz, tools/server-harness and
# tools/engine-client — none of which this smoke test needs, and the extra
# fuzz/sanitizer builds cost real time for no benefit here).
# ---------------------------------------------------------------------------
log "building server (APAD_BUILD_DIR=${BUILD_DIR})"
APAD_BUILD_DIR="${BUILD_DIR}" "${REPO_ROOT}/scripts/build.sh" server

log "building tools/loopback-client"
"${REPO_ROOT}/tools/loopback-client/build.sh" build-only

SERVER_BIN="${BUILD_DIR}/server/atticpad-server"
LOOPBACK_BIN="${REPO_ROOT}/tools/loopback-client/loopback-client"

[[ -x "${SERVER_BIN}" ]]   || { warn "server binary missing at ${SERVER_BIN}"; exit 1; }
[[ -x "${LOOPBACK_BIN}" ]] || { warn "loopback-client binary missing at ${LOOPBACK_BIN}"; exit 1; }

# ---------------------------------------------------------------------------
# 2. scratch profiles dir + scratch server, on scratch ports only
# ---------------------------------------------------------------------------
mkdir -p "${PROFILES_DIR}"
cp "${REPO_ROOT}/server/profiles/"*.jsonc "${PROFILES_DIR}/"

log "starting scratch server: UDP ${UDP_PORT}, UI ${UI_PORT}, profiles=${PROFILES_DIR}"
ATTICPAD_UI_PORT="${UI_PORT}" ATTICPAD_PROFILES_DIR="${PROFILES_DIR}" \
  "${SERVER_BIN}" "${UDP_PORT}" --no-mdns >"${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

# Wait for the UI socket to come up (webui_open() binds synchronously
# before main()'s loop starts, but give it a moment on a loaded box).
ready=0
for _ in $(seq 1 50); do
  if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
    warn "server exited before the UI port came up -- see ${SERVER_LOG}"
    cat "${SERVER_LOG}" >&2
    exit 1
  fi
  if curl -sS -o /dev/null -m 1 "${BASE}/api/state" 2>/dev/null; then
    ready=1
    break
  fi
  sleep 0.1
done
[[ "${ready}" -eq 1 ]] || { warn "server UI never answered on ${BASE}"; exit 1; }
log "scratch server up, pid=${SERVER_PID}"

# ---------------------------------------------------------------------------
# small curl helper -- every call is bounded (-m) so a hung connection
# cannot stall the whole script under set -e's error paths.
#
# NOT `status=$(http ...)`: http() needs to set TWO outputs (a status code
# AND where the body landed), and command substitution runs its command in
# a SUBSHELL -- any variable http() set would vanish the instant that
# subshell exits. So http() sets two globals (HTTP_STATUS, REPLY_BODY)
# directly instead of printing anything; every call site below is a plain
# statement (`http GET /path`) followed by reading ${HTTP_STATUS}.
# ---------------------------------------------------------------------------
http() {
  # http METHOD PATH [BODY_FILE] [EXTRA_HEADER]
  local method="$1" path="$2" body_file="${3:-}" extra_header="${4:-}"
  REPLY_BODY="${SCRATCH_DIR}/reply.$$.${RANDOM}"
  local -a args=(-sS -o "${REPLY_BODY}" -w '%{http_code}' -m 5 -X "${method}" "${BASE}${path}")
  [[ -n "${body_file}" ]] && args+=(--data-binary "@${body_file}")
  [[ -n "${extra_header}" ]] && args+=(-H "${extra_header}")
  HTTP_STATUS="$(curl "${args[@]}")"
}

jf() {
  # jf FIELD_EXPR < json-on-stdin -- tiny python3 JSON field reader, e.g.
  # `jf "d['clients']"` or `jf "d['buttons']['A']"`. Prints the value via
  # json.dumps so callers can string-compare reliably.
  python3 -c "
import json, sys
d = json.load(sys.stdin)
sys.stdout.write(json.dumps(eval(sys.argv[1])))
" "$1"
}

# ===========================================================================
# checks
# ===========================================================================

# ---- a: GET /api/state -----------------------------------------------------
http GET /api/state
if [[ "${HTTP_STATUS}" == "200" ]] && python3 -c "
import json, sys
d = json.load(open(sys.argv[1]))
sys.exit(0 if isinstance(d.get('clients'), list) else 1)
" "${REPLY_BODY}" 2>/dev/null; then
  pass a "GET /api/state -> 200, JSON with a clients array"
else
  fail a "GET /api/state -> got status=${HTTP_STATUS}, body=$(cat "${REPLY_BODY}" 2>/dev/null)"
fi

# ---- b: GET /api/profiles ---------------------------------------------------
http GET /api/profiles
status="${HTTP_STATUS}"
names="$(jf 'sorted(x["name"] for x in d)' <"${REPLY_BODY}" 2>/dev/null || echo '[]')"
if [[ "${status}" == "200" ]] \
  && [[ "${names}" == *'"3ds-default"'* ]] \
  && [[ "${names}" == *'"generic-default"'* ]] \
  && [[ "${names}" == *'"builtin-default"'* ]]; then
  pass b "GET /api/profiles -> 200, lists ${names}"
else
  fail b "GET /api/profiles -> status=${status}, names=${names}"
fi

# ---- start the loopback-client session (see this file's header comment) --
#
# Synchronisation is done entirely through the SERVER's own live state
# (GET /api/state, GET /api/client/{slot}/input), never by grepping
# loopback-client's own stdout. Tried that first and it does not work:
# tools/loopback-client's main.c never calls setvbuf() (unlike
# server/host/linux/main.c, which does, specifically for this reason -- see
# that file's own comment), so the instant its stdout is redirected to
# LOOPBACK_LOG (not a tty), glibc silently switches printf() from
# line-buffered to a ~4KB FULL buffer -- log lines this script wants to
# grep as "phase started" markers can sit unflushed for a while and land in
# the file well after the run has moved past the phase they announce
# (confirmed by hand: markers appeared "on time" but the live session was
# already gone by the time checks c/h ran, 404 "no client connected on that
# slot"). `stdbuf -oL` fixes THAT via LD_PRELOAD, but tools/loopback-client
# is built with -fsanitize=address (tools/loopback-client/build.sh), and
# ASan must be the FIRST preloaded library -- stdbuf's own preload beats it
# to the punch and the binary refuses to start ("ASan runtime does not come
# first in initial library list"), also confirmed by hand. Polling the
# server's HTTP state instead sidesteps both problems at once: the
# server's own stdio is already correctly configured (setvbuf, see above)
# and this script talks to it over loopback TCP, not through a pipe whose
# buffering behaviour depends on what the OTHER binary's build flags happen
# to be.
log "starting loopback-client --target 127.0.0.1 ${UDP_PORT} (background)"
"${LOOPBACK_BIN}" --target 127.0.0.1 "${UDP_PORT}" >"${LOOPBACK_LOG}" 2>&1 &
LOOPBACK_PID=$!

wait_for_first_client() {
  # Polls GET /api/state until its "clients" array is non-empty, then sets
  # SLOT from the first entry's own "slot" field -- the server's authoritative
  # session-table index (server/src/server.c: apad_server_list_clients()),
  # not anything parsed out of the other process's output.
  local tries="$1"
  local i n
  for ((i = 0; i < tries; i++)); do
    http GET /api/state
    if [[ "${HTTP_STATUS}" == "200" ]]; then
      n="$(jf 'len(d["clients"])' <"${REPLY_BODY}" 2>/dev/null || echo 0)"
      if [[ "${n}" -gt 0 ]]; then
        SLOT="$(jf 'd["clients"][0]["slot"]' <"${REPLY_BODY}")"
        return 0
      fi
    fi
    if ! kill -0 "${LOOPBACK_PID}" 2>/dev/null; then
      return 1   # process already exited -- caller decides if that's fatal
    fi
    sleep 0.1
  done
  return 1
}

if ! wait_for_first_client 50; then
  warn "server never showed a connected client -- see ${LOOPBACK_LOG}"
  cat "${LOOPBACK_LOG}" >&2
  exit 1
fi
log "server sees the loopback-client connected on slot=${SLOT}"

wait_for_frame_positive() {
  # Polls GET /api/client/${SLOT}/input until frame > 0 -- the only phase
  # that actually sends INPUT_STATE (tools/loopback-client/main.c:
  # target_survive_past_idle_timeout(), >3200ms long) is what makes this
  # true; everything before it is HELLO/WELCOME/duplicate-HELLO
  # handshaking. Leaves the last poll's status/frame in HTTP_STATUS/FRAME.
  local tries="$1"
  local i
  for ((i = 0; i < tries; i++)); do
    http GET "/api/client/${SLOT}/input"
    if [[ "${HTTP_STATUS}" == "200" ]]; then
      FRAME="$(jf 'd["frame"]' <"${REPLY_BODY}" 2>/dev/null || echo 0)"
      if [[ "${FRAME}" -gt 0 ]]; then
        return 0
      fi
    fi
    if ! kill -0 "${LOOPBACK_PID}" 2>/dev/null; then
      return 1
    fi
    sleep 0.1
  done
  return 1
}

if ! wait_for_frame_positive 50; then
  warn "no positive input frame ever showed up on slot ${SLOT} -- see ${LOOPBACK_LOG}"
  cat "${LOOPBACK_LOG}" >&2
  exit 1
fi
log "input is flowing (frame=${FRAME}) -- running the live-session checks now"

# ---- c: GET /api/client/{slot}/input, frame > 0 and climbing --------------
status1="${HTTP_STATUS}"
frame1="${FRAME}"
sleep 0.3
http GET "/api/client/${SLOT}/input"
status2="${HTTP_STATUS}"
frame2="$(jf 'd["frame"]' <"${REPLY_BODY}" 2>/dev/null || echo 0)"
if [[ "${status1}" == "200" && "${status2}" == "200" ]] \
  && [[ "${frame1}" -gt 0 ]] && [[ "${frame2}" -gt "${frame1}" ]]; then
  pass c "GET /api/client/${SLOT}/input -> frame ${frame1} then ${frame2} (climbing) while session live"
else
  fail c "GET /api/client/${SLOT}/input -> status1=${status1} frame1=${frame1} status2=${status2} frame2=${frame2}"
fi

# ---- d: PUT /api/profile/00-smoke ------------------------------------------
http GET /api/profile/generic-default
if [[ "${HTTP_STATUS}" != "200" ]]; then
  fail d "GET /api/profile/generic-default (needed as PUT body source) -> status=${HTTP_STATUS}"
else
  SMOKE_BODY="${SCRATCH_DIR}/00-smoke-put.json"
  python3 -c "
import json, sys
d = json.load(open(sys.argv[1]))
d['buttons']['A'] = 'X'   # the one deliberate change this check verifies
json.dump(d, open(sys.argv[2], 'w'))
" "${REPLY_BODY}" "${SMOKE_BODY}"
  http PUT /api/profile/00-smoke "${SMOKE_BODY}"
  status="${HTTP_STATUS}"
  smoke_file="${PROFILES_DIR}/00-smoke.jsonc"
  if [[ "${status}" == "200" ]] && [[ -f "${smoke_file}" ]] \
    && grep -qF "// generated by the AtticPad editor" "${smoke_file}"; then
    pass d "PUT /api/profile/00-smoke -> 200, ${smoke_file} written with the generated-header comment"
  else
    fail d "PUT /api/profile/00-smoke -> status=${status}, file exists=$( [[ -f "${smoke_file}" ]] && echo yes || echo no )"
  fi
fi

# ---- e: GET /api/profile/00-smoke reflects the change ----------------------
http GET /api/profile/00-smoke
status="${HTTP_STATUS}"
btn_a="$(jf 'd["buttons"]["A"]' <"${REPLY_BODY}" 2>/dev/null || echo '""')"
if [[ "${status}" == "200" ]] && [[ "${btn_a}" == '"X"' ]]; then
  pass e "GET /api/profile/00-smoke -> 200, buttons.A == X"
else
  fail e "GET /api/profile/00-smoke -> status=${status}, buttons.A=${btn_a}"
fi

# ---- f: shipped profile (generic-default) is read-only, 409 on write ------
http PUT /api/profile/generic-default "${SMOKE_BODY}"
put_409=$([[ "${HTTP_STATUS}" == "409" ]] && echo 1 || echo 0)
put_status="${HTTP_STATUS}"
http DELETE /api/profile/generic-default
del_409=$([[ "${HTTP_STATUS}" == "409" ]] && echo 1 || echo 0)
del_status="${HTTP_STATUS}"
if [[ "${put_409}" == "1" && "${del_409}" == "1" ]]; then
  pass f "PUT and DELETE /api/profile/generic-default both -> 409 (shipped, read-only)"
else
  fail f "PUT/DELETE /api/profile/generic-default -> put_status=${put_status} del_status=${del_status}"
fi

# ---- g: hand-edit 00-smoke.jsonc, reload, confirm the edit is live --------
smoke_file="${PROFILES_DIR}/00-smoke.jsonc"
before="$(grep -o '"deadzone":[0-9.]*' "${smoke_file}" | head -1)"
sed -i 's/"deadzone":0\.08/"deadzone":0.5/' "${smoke_file}"
after="$(grep -o '"deadzone":[0-9.]*' "${smoke_file}" | head -1)"
http POST /api/profiles/reload
status="${HTTP_STATUS}"
http GET /api/profile/00-smoke
new_dz="$(jf 'd["sticks"]["left"]["deadzone"]' <"${REPLY_BODY}" 2>/dev/null || echo null)"
if [[ "${status}" == "200" ]] && [[ "${before}" != "${after}" ]] && [[ "${new_dz}" == "0.5" ]]; then
  pass g "hand-edited ${smoke_file} (${before} -> ${after}), POST /api/profiles/reload -> 200, GET shows sticks.left.deadzone=0.5"
else
  fail g "reload status=${status}, sed changed ${before}->${after}, GET sticks.left.deadzone=${new_dz}"
fi

# ---- h: reload while the loopback session is mid-flight -------------------
if ! kill -0 "${LOOPBACK_PID}" 2>/dev/null; then
  fail h "loopback-client already exited -- the reload-mid-flight check needs a live session; see this script's timing comment and ${LOOPBACK_LOG}"
else
  http POST /api/profiles/reload
  status_reload="${HTTP_STATUS}"
  http GET /api/state
  status_state="${HTTP_STATUS}"
  if [[ "${status_reload}" == "200" ]] && [[ "${status_state}" == "200" ]] && kill -0 "${LOOPBACK_PID}" 2>/dev/null; then
    pass h "POST /api/profiles/reload mid-session -> 200, server stayed up (GET /api/state -> 200), loopback-client still running"
  else
    fail h "reload_status=${status_reload} state_status=${status_state} loopback_alive=$(kill -0 "${LOOPBACK_PID}" 2>/dev/null && echo yes || echo no)"
  fi
fi

# ---- i: DELETE /api/profile/00-smoke ---------------------------------------
http GET /api/profiles
before_names="$(jf 'sorted(x["name"] for x in d)' <"${REPLY_BODY}")"
http DELETE /api/profile/00-smoke
status="${HTTP_STATUS}"
http GET /api/profiles
after_names="$(jf 'sorted(x["name"] for x in d)' <"${REPLY_BODY}")"
if [[ "${status}" == "200" ]] && [[ ! -f "${smoke_file}" ]] \
  && [[ "${before_names}" == *'"00-smoke"'* ]] && [[ "${after_names}" != *'"00-smoke"'* ]]; then
  pass i "DELETE /api/profile/00-smoke -> 200, ${smoke_file} gone, list shrank (${before_names} -> ${after_names})"
else
  fail i "DELETE -> status=${status}, file_gone=$([[ ! -f "${smoke_file}" ]] && echo yes || echo no), before=${before_names} after=${after_names}"
fi

# ---- j: forged Host header -> rejected (non-2xx), else SKIP ----------------
# webui.h's ui_host_is_loopback()/DNS-rebinding defence is, per this task's
# brief, possibly still landing in another agent's session right now. If
# it's not in this build yet, the server has no reason to refuse a
# syntactically fine request and will answer 200 -- that is an expected
# "not yet hardened" state, not a bug this script should FAIL on.
http GET /api/state "" "Host: evil.example"
status="${HTTP_STATUS}"
if [[ "${status}" -ge 200 && "${status}" -lt 300 ]]; then
  skip j "forged 'Host: evil.example' -> ${status} (accepted) -- Host-header validation not landed in this build yet, per this task's brief"
else
  pass j "forged 'Host: evil.example' -> ${status} (rejected)"
fi

# ---- k: cross-origin POST -> 403, else SKIP --------------------------------
http POST /api/profiles/reload "" "Origin: http://evil.example"
status="${HTTP_STATUS}"
if [[ "${status}" == "403" ]]; then
  pass k "POST /api/profiles/reload with Origin: http://evil.example -> 403 (rejected)"
elif [[ "${status}" -ge 200 && "${status}" -lt 300 ]]; then
  skip k "POST with cross-origin Origin header -> ${status} (accepted) -- Origin/CSRF check not landed in this build yet, per this task's brief"
else
  fail k "POST with cross-origin Origin header -> ${status} (expected 403 or a 2xx-if-unhardened SKIP)"
fi

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------
log "loopback-client final lines:"
tail -5 "${LOOPBACK_LOG}" || true

echo
log "SUMMARY: ${PASS_COUNT} passed, ${FAIL_COUNT} failed, ${SKIP_COUNT} skipped"
if [[ "${FAIL_COUNT}" -gt 0 ]]; then
  exit 1
fi
exit 0
