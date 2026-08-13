#!/usr/bin/env python3
"""Generate server/host/common/profiles_builtin.h from the shipped profiles.

The shipped profiles are data, but a release is one binary: a user who
downloads a server and nothing else got the compiled-in fallback only, which
on a 3DS silently costs the touchscreen triggers and gyro aim while the client
keeps drawing both. Embedding them removes that whole failure mode.

The library never learns about this. apad_profiles_load() already takes blobs
rather than paths (server/src/profiles.h: "finding and reading files is the
host's, parsing is the library's"), so the hosts simply hand it these strings
when the disk scan comes back empty.

Run via scripts/build.sh; CI re-runs it and fails if the result differs from
what is committed, so the header cannot drift from the .jsonc files it mirrors.
"""
import pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = sorted((ROOT / "server" / "profiles").glob("*.jsonc"))
OUT = ROOT / "server" / "host" / "common" / "profiles_builtin.h"


def c_string(text: str) -> str:
    """One C string literal per source line, so the header stays diffable and
    a compiler error points at a line that exists in the .jsonc."""
    out = []
    for line in text.splitlines():
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'"{esc}\\n"')
    return "\n".join(out) if out else '""'


def main() -> int:
    if not SRC:
        print("no .jsonc profiles found", file=sys.stderr)
        return 1
    parts = [
        "/* server/host/common/profiles_builtin.h -- GENERATED, do not edit.",
        " *",
        " * Source: the .jsonc files in server/profiles/",
        " * Regenerate: scripts/support/gen_profiles_builtin.py (scripts/build.sh",
        " * does it, and CI fails if the committed copy differs).",
        " *",
        " * Why these are compiled in: a release is a single binary, and a server",
        " * that finds no profiles on disk would otherwise fall back to the",
        " * built-in default -- which on a 3DS means no touch-region triggers and",
        " * no gyro aim, while the client still draws both. Files on disk still",
        " * win; these are only the floor.",
        " */",
        "#ifndef ATTICPAD_HOST_COMMON_PROFILES_BUILTIN_H",
        "#define ATTICPAD_HOST_COMMON_PROFILES_BUILTIN_H",
        "",
        "typedef struct {",
        "    const char *label;   /* for diagnostics: where this came from */",
        "    const char *name;    /* profile file stem, as on disk         */",
        "    const char *text;    /* the JSONC itself                      */",
        "} apad_builtin_profile;",
        "",
    ]
    for p in SRC:
        stem = p.stem
        sym = stem.upper().replace("-", "_")
        parts.append(f"/* {p.relative_to(ROOT)} */")
        parts.append(f"static const char ATTICPAD_PROFILE_{sym}[] =")
        parts.append(c_string(p.read_text(encoding="utf-8")) + ";")
        parts.append("")
    parts.append("static const apad_builtin_profile ATTICPAD_BUILTIN_PROFILES[] = {")
    for p in SRC:
        sym = p.stem.upper().replace("-", "_")
        parts.append(f'    {{ "built-in {p.name}", "{p.stem}", ATTICPAD_PROFILE_{sym} }},')
    parts.append("};")
    parts.append("")
    parts.append("#define ATTICPAD_BUILTIN_PROFILE_COUNT "
                 f"((size_t)({len(SRC)}))")
    parts.append("")
    parts.append("#endif /* ATTICPAD_HOST_COMMON_PROFILES_BUILTIN_H */")
    OUT.write_text("\n".join(parts) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} from {len(SRC)} profile(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
