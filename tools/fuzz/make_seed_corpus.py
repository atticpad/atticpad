#!/usr/bin/env python3
"""
tools/fuzz/make_seed_corpus.py

Populates tools/fuzz/seed_corpus/ with the raw bytes of every conformance
vector in core/testdata/generate.py -- docs/DESIGN.md S9.2: "Seed the corpus from
the conformance vectors, including the wrap-boundary cases."

This imports core/testdata/generate.py as a module (its vector-construction
code runs at import time; main() -- which writes vectors.h -- is guarded by
`if __name__ == "__main__"` and does not run here). That keeps the seed
corpus and vectors.h generated from a single source of truth instead of two
copies that can drift.

Usage:
    python3 tools/fuzz/make_seed_corpus.py
"""

import importlib.util
import os
import sys


def load_generate_module():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(os.path.dirname(here))
    gen_path = os.path.join(repo_root, "core", "testdata", "generate.py")
    spec = importlib.util.spec_from_file_location("apad_vector_generate", gen_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write(out_dir, name, data):
    # Sanitize the filename: libFuzzer/AFL corpus directories are plain
    # files, no subdirectories.
    safe = "".join(c if c.isalnum() or c in "._-" else "_" for c in name)
    path = os.path.join(out_dir, safe)
    with open(path, "wb") as f:
        f.write(data)
    return path


def main():
    g = load_generate_module()
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(here, "seed_corpus")
    os.makedirs(out_dir, exist_ok=True)
    # Clear any stale seeds from a previous run so this stays reproducible.
    for f in os.listdir(out_dir):
        os.remove(os.path.join(out_dir, f))

    count = 0

    # Section A: every framing vector, at its logical length (so the
    # 0-byte-datagram vector seeds an actual empty file, not a 1-byte one).
    for fv in g.frame_vectors:
        write(out_dir, "frame_%s" % fv["name"], fv["bytes"][:fv["logical_len"]])
        count += 1

    # Section B: every truncation point of the canonical INPUT_STATE
    # datagram, 0..68 inclusive (68 is the full, valid datagram).
    for t in range(0, len(g.CANONICAL_DATAGRAM) + 1):
        write(out_dir, "trunc_%02d" % t, g.CANONICAL_DATAGRAM[:t])
        count += 1

    # Section B2: every truncation point of Appendix A's authenticated PING
    # datagram, 0..28 inclusive -- covers the AUTHENTICATED "+8" length arm
    # and the tag bytes themselves.
    for t in range(0, len(g.APPENDIX_A_DATAGRAM) + 1):
        write(out_dir, "auth_trunc_%02d" % t, g.APPENDIX_A_DATAGRAM[:t])
        count += 1

    # Section C: every INPUT_STATE payload decode vector, including the
    # sequence-wrap-boundary (0xFFFF / 0x0000) and tick-wrap-boundary
    # (0 / 0xFFFFFFFF) packets explicitly called for by docs/DESIGN.md S9.2.
    for iv in g.input_state_vectors:
        write(out_dir, "input_state_%s" % iv["name"], iv["packet"])
        count += 1

    # Section F: the Appendix A authenticated datagrams, valid tag and every
    # single-bit-flip variant.
    for av in g.auth_tag_vectors:
        write(out_dir, "auth_tag_%s" % av["name"], av["datagram"])
        count += 1

    print("wrote %d seed files to %s" % (count, out_dir))


if __name__ == "__main__":
    main()
