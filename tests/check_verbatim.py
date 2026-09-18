# -*- coding: utf-8 -*-
"""Check that the DepthProcessor functions in core/DepthPreprocCore.cpp are verbatim copies
of the original DLL source (DESIGN.md section 0-1: only whitespace/comments may differ).

Usage:
  python tests/check_verbatim.py [--sim core/DepthPreprocCore.cpp] [--orig <3dDepthProcessing.cpp>]
Exit code 0 when every function matches, 1 otherwise.
"""
import argparse
import difflib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_SIM = os.path.join(ROOT, "core", "DepthPreprocCore.cpp")
# Reference = the SITE (PC3) build source kept in the repo (2026-07-08, rc 1.0.2.0.38cf930_HT).
DEFAULT_ORIG = os.path.join(ROOT, "reference", "site_pc3_20260708", "3dDepthProcessing.cpp")

FUNCTIONS = [
    ("void", "removeStageFromRawData"),
    ("void", "removeStageFromRawDataBead"),
    ("double", "computePercentile"),
    ("cv::Mat", "postClipNormalize"),
    ("cv::Mat", "scaleTo16bitIgnoreNull"),
    ("cv::Mat", "patchBasedMedian"),
    ("cv::Mat", "processHighCurvature"),
]


def read_text(path):
    with open(path, "rb") as f:
        data = f.read()
    for enc in ("utf-8-sig", "cp949", "latin-1"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")


def strip_comments(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n and src[j] != q:
                if src[j] == "\\":
                    j += 1
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        else:
            out.append(c)
            i += 1
    return "".join(out)


def extract_function(src, ret, name):
    """Return the definition text (signature through the closing brace) of `ret name(...) {...}`."""
    pat = re.compile(r"^[ \t]*" + re.escape(ret) + r"[ \t]+" + re.escape(name) + r"[ \t]*\(", re.M)
    m = pat.search(src)
    if not m:
        return None
    start = m.start()
    open_idx = src.find("{", m.end())
    if open_idx < 0:
        return None
    depth = 0
    i = open_idx
    while i < len(src):
        ch = src[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
        i += 1
    return None


def normalize(text):
    text = strip_comments(text)
    return re.sub(r"\s+", "", text)


def spaced(text):
    text = strip_comments(text)
    return [ln.strip() for ln in text.splitlines() if ln.strip()]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default=DEFAULT_SIM)
    ap.add_argument("--orig", default=DEFAULT_ORIG)
    args = ap.parse_args(argv)
    for p in (args.sim, args.orig):
        if not os.path.isfile(p):
            print("[verbatim] file not found: %s" % p, file=sys.stderr)
            return 1
    sim = read_text(args.sim)
    orig = read_text(args.orig)
    ok_all = True
    for ret, name in FUNCTIONS:
        a = extract_function(orig, ret, name)
        b = extract_function(sim, ret, name)
        if a is None or b is None:
            print("[verbatim] %-24s MISSING (orig=%s sim=%s)" % (name, a is not None, b is not None))
            ok_all = False
            continue
        same = normalize(a) == normalize(b)
        print("[verbatim] %-24s %s (%d chars normalized)" % (name, "OK" if same else "DIFFERENT", len(normalize(a))))
        if not same:
            ok_all = False
            for ln in difflib.unified_diff(spaced(a), spaced(b), "original", "simulator", lineterm="", n=1):
                print("    " + ln)
    print("[verbatim] %s" % ("ALL OK" if ok_all else "MISMATCH"))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
