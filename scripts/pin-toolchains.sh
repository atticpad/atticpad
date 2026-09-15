#!/usr/bin/env bash
# Regenerate pinned toolchain digests. Deliberate, never automatic.
set -euo pipefail

# KEY|image:tag  — the KEY is what CI and build.sh reference, so switching
# image providers touches this file only.
IMAGES=(
  "DEVKITARM|devkitpro/devkitarm:latest"
  "DEVKITA64|devkitpro/devkita64:latest"
  "PSPDEV|pspdev/pspdev:latest"
  "VITASDK|gnuton/vitasdk-docker:latest"
  "BLOCKSDS|skylyrac/blocksds:slim-v1.23.0"   # versioned tag on purpose: slim-latest moves
)

# Single-key mode: `scripts/pin-toolchains.sh BLOCKSDS` re-pins ONLY that
# entry and rewrites its line in place, leaving every other digest exactly as
# it was. Without this, adding one toolchain meant re-pulling every :latest
# above and silently moving four unrelated pins in the same commit.
ONLY="${1:-}"
if [[ -n "$ONLY" ]]; then
  found=""
  for entry in "${IMAGES[@]}"; do
    [[ "${entry%%|*}" == "$ONLY" ]] && found="$entry"
  done
  [[ -n "$found" ]] || { echo "FATAL: no image keyed $ONLY in IMAGES" >&2; exit 1; }
  ref="${found#*|}"
  echo ">>> pulling $ref" >&2
  docker pull "$ref"
  digest=$(docker inspect --format='{{index .RepoDigests 0}}' "$ref" 2>/dev/null || true)
  case "$digest" in
    *@sha256:*) ;;
    *) echo "FATAL: no digest for $ref" >&2; exit 1 ;;
  esac
  if grep -q "^${ONLY}=" scripts/toolchains.env; then
    sed -i "s|^${ONLY}=.*|${ONLY}=${digest}|" scripts/toolchains.env
  else
    echo "${ONLY}=${digest}" >> scripts/toolchains.env
  fi
  echo "OK — ${ONLY} pinned to ${digest}" >&2
  exit 0
fi

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
{
  echo "# Pinned toolchain digests — CI and scripts/build.sh both source this."
  echo "# Regenerate with scripts/pin-toolchains.sh. Never edit by hand."
  echo "# Generated $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$TMP"

for entry in "${IMAGES[@]}"; do
  key="${entry%%|*}"; ref="${entry#*|}"
  echo ">>> pulling $ref" >&2
  docker pull "$ref"                       # progress visible on purpose
  digest=$(docker inspect --format='{{index .RepoDigests 0}}' "$ref" 2>/dev/null || true)
  case "$digest" in
    *@sha256:*) ;;
    *) echo "FATAL: no digest for $ref — refusing to write a partial file" >&2; exit 1 ;;
  esac
  echo "${key}=${digest}" >> "$TMP"
done

mv "$TMP" scripts/toolchains.env; trap - EXIT
echo "OK — $(grep -c '@sha256:' scripts/toolchains.env) digests pinned" >&2
