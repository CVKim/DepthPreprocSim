# -*- coding: utf-8 -*-
"""fake_exe.py - Python stand-in for depth_sim.exe, used only to validate tests/verify.py and tests/ui_smoke.py
before the C++ build exists.

It implements the CLI contract of DESIGN.md section 2 (run / batch / info / version, exit codes 0/2/3/4,
one-line JSON on stderr, result.png + stats.json with the 'ref' block, batch.json) on top of the python
reference simulate.py (remove_stage + process_high_curvature). It is NOT bit-identical to the DLL and does
not implement ROI or the exp_* options (they are only recorded in stats.json).

Usage:
  python tests/fake_exe.py run   --in <file> --out <dir> [--ref <file>] [--ini <ini> --cal N] [--patch W,H]
                                 [--overlap F] [--stage AUTO|TOP|BOTTOM|NONE] [--dump min|all] [--preview N]
  python tests/fake_exe.py batch --folder <dir> --out <dir> [--fovproc <FOVPROC.ini>] [--ini <ini>] [...]
  python tests/fake_exe.py info  --in <file>
  python tests/fake_exe.py version
Environment: DEPTHSIM_PY_REF overrides the simulate.py location.
"""
import json
import logging
import os
import re
import sys
import time

import numpy as np
import cv2
import tifffile

logging.getLogger("tifffile").setLevel(logging.ERROR)

TOOL_VERSION = "fake_exe 0.1 (python reference stand-in, not DLL-identical)"
NULL = -999.0
STAGES = ["AUTO", "TOP", "BOTTOM", "NONE"]
DEFAULT_MAPPING = {1: (9, 1), 3: (10, 2), 5: (11, 3), 7: (12, 3), 13: (21, 1), 15: (22, 2), 17: (23, 3), 19: (24, 3)}
REF_PY = os.environ.get("DEPTHSIM_PY_REF",
                        r"C:\Users\AIV\Desktop\PC3_DepthPreproc_Null검토\근거자료\simulate.py")


def fail(code, msg):
    sys.stderr.write(json.dumps({"error": msg}, ensure_ascii=False) + "\n")
    sys.stderr.flush()
    sys.exit(code)


def load_reference():
    if not os.path.isfile(REF_PY):
        fail(4, "python reference not found: " + REF_PY)
    folder = os.path.dirname(os.path.abspath(REF_PY))
    if folder not in sys.path:
        sys.path.insert(0, folder)
    import simulate  # noqa: E402
    return simulate


# ----------------------------------------------------------------------------- args / ini
def parse_opts(argv):
    opts = {}
    i = 0
    while i < len(argv):
        a = argv[i]
        if not a.startswith("--"):
            fail(2, "unexpected argument: " + a)
        if i + 1 >= len(argv):
            fail(2, "missing value for " + a)
        opts[a[2:]] = argv[i + 1]
        i += 2
    return opts


def parse_patch(s):
    parts = [p.strip() for p in str(s).split(",")]
    if len(parts) != 2:
        fail(2, "--patch must be W,H")
    try:
        w, h = int(parts[0]), int(parts[1])
    except ValueError:
        fail(2, "--patch must be two integers")
    if w <= 0 or h <= 0:
        fail(2, "--patch values must be > 0 (got %d,%d)" % (w, h))
    return w, h


def parse_overlap(s):
    try:
        v = float(s)
    except ValueError:
        fail(2, "--overlap must be a number")
    if not (0.0 < v < 1.0):
        fail(2, "--overlap must be in the open interval (0,1), got %g" % v)
    return v


def parse_stage(s):
    u = str(s).strip().upper()
    if u.isdigit():
        n = int(u)
        if 0 <= n < len(STAGES):
            return STAGES[n]
        fail(2, "--stage out of range: " + s)
    if u not in STAGES:
        fail(2, "--stage must be AUTO|TOP|BOTTOM|NONE")
    return u


def read_ini(path):
    secs, cur = {}, None
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        for line in f:
            s = line.strip()
            if not s or s[0] in ";#":
                continue
            if s.startswith("[") and s.endswith("]"):
                cur = s[1:-1].strip()
                secs[cur] = {}
                continue
            if "=" in s and cur is not None:
                k, v = s.split("=", 1)
                secs[cur][k.strip()] = v.strip()
    return secs


def cal_from_ini(ini_path, cal):
    if not os.path.isfile(ini_path):
        fail(3, "ini not found: " + ini_path)
    sec = read_ini(ini_path).get("CAL%04d" % cal)
    if sec is None:
        fail(2, "section CAL%04d not found in %s" % (cal, ini_path))
    p = {"overlap": 0.5}  # DLL default when the key is missing
    if "PatchSize" in sec:
        p["patch"] = parse_patch(sec["PatchSize"])
    if "Overlap" in sec:
        p["overlap"] = parse_overlap(sec["Overlap"])
    if "StagePosition" in sec:
        p["stage"] = parse_stage(sec["StagePosition"])
    if "DepthPreprocType" in sec:
        p["type"] = sec["DepthPreprocType"]
    p["lower"] = float(sec.get("Lower Percentage", 5))
    p["upper"] = float(sec.get("Upper Percentage", 95))
    return p


def fovproc_from_ini(path):
    if not os.path.isfile(path):
        fail(3, "fovproc ini not found: " + path)
    m = {}
    for name, sec in read_ini(path).items():
        if name.upper().startswith("FOVPROC") and "RequireImgIdx" in sec and "ResultImgIdx" in sec:
            m[int(sec["RequireImgIdx"])] = (int(sec["ResultImgIdx"]), int(sec.get("ParamIdx", 0)))
    return m


def resolve_params(opts, cal_override=None):
    """Defaults (15,15 / 0.25 / AUTO) <- ini [CAL] <- explicit options."""
    p = {"patch": (15, 15), "overlap": 0.25, "stage": "AUTO", "lower": 5.0, "upper": 95.0, "type": None, "roi": None,
         "cal": None}
    cal = cal_override if cal_override is not None else (int(opts["cal"]) if "cal" in opts else None)
    if "ini" in opts and cal is not None:
        p.update(cal_from_ini(opts["ini"], cal))
        p["cal"] = cal
    if "patch" in opts:
        p["patch"] = parse_patch(opts["patch"])
    if "overlap" in opts:
        p["overlap"] = parse_overlap(opts["overlap"])
    if "stage" in opts:
        p["stage"] = parse_stage(opts["stage"])
    if "type" in opts:
        p["type"] = opts["type"]
    if "lower" in opts:
        p["lower"] = float(opts["lower"])
    if "upper" in opts:
        p["upper"] = float(opts["upper"])
    if "roi" in opts:
        r = [int(x) for x in opts["roi"].split(",")]
        p["roi"] = r if len(r) == 4 and r[2] > r[0] and r[3] > r[1] else None
    exp = {"use_ini_pct": opts.get("exp-use-ini-pct", "0") == "1",
           "valid_pct": opts.get("exp-valid-pct", "0") == "1",
           "masked_median": opts.get("exp-masked-median", "0") == "1",
           "null_value": int(opts.get("exp-null-value", "-1")),
           "fill_holes": int(opts.get("exp-fill-holes", "0"))}
    p["exp"] = exp
    p["dll_identical"] = not (exp["use_ini_pct"] or exp["valid_pct"] or exp["masked_median"]
                              or exp["null_value"] >= 0 or exp["fill_holes"] > 0)
    dump = opts.get("dump", "all")
    if dump not in ("min", "all"):
        fail(2, "--dump must be min|all")
    p["dump"] = dump
    try:
        p["preview"] = max(1, int(opts.get("preview", "8")))
    except ValueError:
        fail(2, "--preview must be an integer")
    return p


# ----------------------------------------------------------------------------- io
def read_image_f32(path):
    if not os.path.isfile(path):
        fail(3, "input file not found: " + path)
    ext = os.path.splitext(path)[1].lower()
    try:
        if ext in (".mim", ".tif", ".tiff"):
            a = tifffile.imread(path)
        else:
            a = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    except Exception as e:  # noqa: BLE001
        fail(3, "cannot read %s: %s" % (path, e))
    if a is None:
        fail(3, "cannot decode image: " + path)
    if a.ndim == 3:
        a = a[:, :, 0]
    return np.ascontiguousarray(a.astype(np.float32)), str(a.dtype)


def read_u8(path):
    a, _ = read_image_f32(path)
    return np.clip(np.rint(a), 0, 255).astype(np.uint8)


def write_png(path, arr):
    ok, buf = cv2.imencode(".png", arr)
    if not ok:
        fail(4, "png encode failed: " + path)
    buf.tofile(path)


# ----------------------------------------------------------------------------- metrics
def ref_block(result, ref_path):
    ref = read_u8(ref_path)
    if ref.shape != result.shape:
        fail(4, "reference size %s != result size %s" % (list(ref.shape), list(result.shape)))
    d = np.abs(result.astype(np.int16) - ref.astype(np.int16))
    n = d.size
    rb, sb = ref == 0, result == 0
    inter, union = int(np.count_nonzero(rb & sb)), int(np.count_nonzero(rb | sb))
    return {"path": ref_path,
            "exact_pct": 100.0 * np.count_nonzero(d == 0) / n,
            "within1_pct": 100.0 * np.count_nonzero(d <= 1) / n,
            "within2_pct": 100.0 * np.count_nonzero(d <= 2) / n,
            "max_abs": int(d.max()), "mismatch_count": int(np.count_nonzero(d)),
            "black_mask_iou": (inter / union) if union else 1.0,
            "ref_black_not_result": int(np.count_nonzero(rb & ~sb)),
            "result_black_not_ref": int(np.count_nonzero(sb & ~rb))}, ref


def process_one(sim, in_path, out_dir, ref_path, p):
    t_all = time.time()
    timing = {}
    t0 = time.time()
    raw, dtype = read_image_f32(in_path)
    timing["read"] = (time.time() - t0) * 1000
    h, w = raw.shape
    orig_null = raw == NULL
    t0 = time.time()
    src = raw if p["stage"] == "NONE" else sim.remove_stage(raw)
    timing["remove_stage"] = (time.time() - t0) * 1000
    stage_removed = (src == NULL) & ~orig_null
    t0 = time.time()
    out, low, high, diff_f, nm = sim.process_high_curvature(src, patch=tuple(p["patch"]), overlap=float(p["overlap"]),
                                                            lower=5, upper=95)
    t_proc = (time.time() - t0) * 1000
    timing.update({"scale": 0.0, "basis": t_proc, "diff": 0.0, "clip_normalize": 0.0})
    valid = ~nm
    nvalid = int(np.count_nonzero(valid))
    n = h * w
    step_w = max(1, int(p["patch"][0] * (1.0 - float(p["overlap"]))))
    step_h = max(1, int(p["patch"][1] * (1.0 - float(p["overlap"]))))
    vrows = np.where(valid.any(axis=1))[0]
    first, last = (int(vrows[0]), int(vrows[-1])) if vrows.size else (-1, -1)
    zmin, zmax = (float(raw[valid].min()), float(raw[valid].max())) if nvalid else (0.0, 0.0)
    black, white = out == 0, out == 255
    bins = 20
    bpct, wpct = [], []
    if first >= 0:
        edges = np.linspace(first, last + 1, bins + 1).astype(int)
        for i in range(bins):
            r0, r1 = int(edges[i]), max(int(edges[i + 1]), int(edges[i]) + 1)
            v = valid[r0:r1]
            nv = int(np.count_nonzero(v))
            bpct.append(100.0 * np.count_nonzero(black[r0:r1] & v) / nv if nv else 0.0)
            wpct.append(100.0 * np.count_nonzero(white[r0:r1] & v) / nv if nv else 0.0)
    ncomp, _, st, _ = cv2.connectedComponentsWithStats(valid.astype(np.uint8), connectivity=8)
    largest = (100.0 * float(st[1:, cv2.CC_STAT_AREA].max()) / nvalid) if (ncomp > 1 and nvalid) else 0.0
    dv = diff_f[valid]
    counts, _ = np.histogram(np.clip(dv, -1000, 1000), bins=256, range=(-1000, 1000))
    cl = np.clip(diff_f, low, high)
    stats = {
        "tool_version": TOOL_VERSION,
        "input": {"path": in_path, "width": int(w), "height": int(h), "dtype": dtype},
        "params_effective": {"patch_w": int(p["patch"][0]), "patch_h": int(p["patch"][1]), "overlap": float(p["overlap"]),
                             "step_w": step_w, "step_h": step_h, "lower": 5, "upper": 95, "stage": p["stage"],
                             "roi": p["roi"], "exp": p["exp"], "dll_identical": bool(p["dll_identical"]),
                             "type": p["type"], "cal": p["cal"]},
        "timing_ms": timing,
        "null": {"count": int(np.count_nonzero(orig_null)), "pct": 100.0 * np.count_nonzero(orig_null) / n,
                 "stage_removed_count": int(np.count_nonzero(stage_removed)),
                 "stage_removed_pct": 100.0 * np.count_nonzero(stage_removed) / n,
                 "valid_after_stage": nvalid, "valid_components": int(ncomp - 1),
                 "largest_component_pct_of_valid": largest},
        "valid_rows": {"first": first, "last": last},
        "z": {"min": zmin, "max": zmax, "unit_mm_per_scaled_unit": (zmax - zmin) / 65536.0 if zmax > zmin else 0.0},
        "clip": {"low": float(low), "high": float(high), "low_mm": 0.0, "high_mm": 0.0,
                 "norm_min": float(cl.min()), "norm_max": float(cl.max()),
                 "zero_diff_pct": 100.0 * np.count_nonzero(dv == 0) / dv.size if dv.size else 0.0},
        "output": {"black_count": int(np.count_nonzero(black)), "black_pct": 100.0 * np.count_nonzero(black) / n,
                   "black_in_valid": int(np.count_nonzero(black & valid)),
                   "black_in_valid_pct": 100.0 * np.count_nonzero(black & valid) / nvalid if nvalid else 0.0,
                   "white_in_valid": int(np.count_nonzero(white & valid)),
                   "white_in_valid_pct": 100.0 * np.count_nonzero(white & valid) / nvalid if nvalid else 0.0,
                   "row_profile_bins": bins, "black_pct_by_valid_row_bin": bpct, "white_pct_by_valid_row_bin": wpct},
        "diff_hist": {"min": -1000, "max": 1000, "bins": 256, "counts": [int(c) for c in counts], "valid_only": True},
        "ref": None,
    }
    os.makedirs(out_dir, exist_ok=True)
    write_png(os.path.join(out_dir, "result.png"), out)
    ref = None
    if ref_path:
        stats["ref"], ref = ref_block(out, ref_path)
    if p["dump"] == "all":
        tifffile.imwrite(os.path.join(out_dir, "result_f32.tif"), out.astype(np.float32))
        write_png(os.path.join(out_dir, "null_mask.png"), (orig_null * 255).astype(np.uint8))
        write_png(os.path.join(out_dir, "stage_removed_mask.png"), (stage_removed * 255).astype(np.uint8))
        d8 = np.clip((diff_f + 1000.0) * (255.0 / 2000.0), 0, 255).astype(np.uint8)
        write_png(os.path.join(out_dir, "diff.png"), d8)
        raw.astype("<f4").tofile(os.path.join(out_dir, "raw.f32"))
        diff_f.astype("<f4").tofile(os.path.join(out_dir, "diff.f32"))
        pv = p["preview"]
        write_png(os.path.join(out_dir, "preview_result.png"),
                  cv2.resize(out, (max(1, w // pv), max(1, h // pv)), interpolation=cv2.INTER_AREA))
        if ref is not None:
            write_png(os.path.join(out_dir, "ref.png"), ref)
            write_png(os.path.join(out_dir, "ref_diff.png"),
                      np.clip(np.abs(out.astype(np.int16) - ref.astype(np.int16)) * 8, 0, 255).astype(np.uint8))
    timing["total"] = (time.time() - t_all) * 1000
    with open(os.path.join(out_dir, "stats.json"), "w", encoding="utf-8") as f:
        json.dump(stats, f, ensure_ascii=False, indent=1)
    return stats


# ----------------------------------------------------------------------------- commands
def cmd_run(opts):
    if "in" not in opts or "out" not in opts:
        fail(2, "run requires --in and --out")
    p = resolve_params(opts)
    if "ref" in opts and not os.path.isfile(opts["ref"]):
        fail(3, "reference file not found: " + opts["ref"])
    sim = load_reference()
    stats = process_one(sim, opts["in"], opts["out"], opts.get("ref"), p)
    print(json.dumps({"out": opts["out"], "total_ms": round(stats["timing_ms"]["total"], 1),
                      "ref": stats["ref"] and {k: stats["ref"][k] for k in ("exact_pct", "black_mask_iou")}},
                     ensure_ascii=False))
    return 0


def cmd_batch(opts):
    if "folder" not in opts or "out" not in opts:
        fail(2, "batch requires --folder and --out")
    folder, out = opts["folder"], opts["out"]
    if not os.path.isdir(folder):
        fail(3, "folder not found: " + folder)
    mapping = fovproc_from_ini(opts["fovproc"]) if "fovproc" in opts else DEFAULT_MAPPING

    def idx_of(f):
        m = re.match(r"(\d+)_", f)
        return int(m.group(1)) if m else -1

    files = sorted([f for f in os.listdir(folder) if f.lower().endswith("_d.mim") and "_proc" not in f.lower()
                    and idx_of(f) in mapping], key=idx_of)
    if not files:
        fail(3, "no *_D.mim depth images found in " + folder)
    sim = load_reference()
    os.makedirs(out, exist_ok=True)
    items = []
    for f in files:
        idx = idx_of(f)
        ref_idx, cal = mapping[idx]
        p = resolve_params(opts, cal_override=cal if "ini" in opts else None)
        stem = os.path.splitext(f)[0]
        name = stem.split("_", 1)[1] if "_" in stem else stem
        item_dir = os.path.join(out, "%d_%s" % (idx, name))
        ref = None
        for g in sorted(os.listdir(folder)):
            if g.startswith("%d_" % ref_idx) and "_Proc" in g and g.lower().endswith(".mim"):
                ref = os.path.join(folder, g)
                break
        stats = process_one(sim, os.path.join(folder, f), item_dir, ref, p)
        items.append({"imgIdx": idx, "name": name, "resultIdx": ref_idx, "cal": cal, "input": os.path.join(folder, f),
                      "ref": ref, "dir": item_dir,
                      "stats": {"params_effective": stats["params_effective"], "null": stats["null"],
                                "clip": stats["clip"], "output": {k: v for k, v in stats["output"].items()
                                                                  if not k.endswith("_bin")},
                                "timing_ms": stats["timing_ms"], "ref": stats["ref"]}})
        print(json.dumps({"item": item_dir, "ref": stats["ref"] and round(stats["ref"]["exact_pct"], 3)},
                         ensure_ascii=False), flush=True)
    with open(os.path.join(out, "batch.json"), "w", encoding="utf-8") as f:
        json.dump(items, f, ensure_ascii=False, indent=1)
    return 0


def cmd_info(opts):
    if "in" not in opts:
        fail(2, "info requires --in")
    a, dtype = read_image_f32(opts["in"])
    nm = a == NULL
    print(json.dumps({"path": opts["in"], "width": int(a.shape[1]), "height": int(a.shape[0]), "dtype": dtype,
                      "min": float(a[~nm].min()) if (~nm).any() else None,
                      "max": float(a[~nm].max()) if (~nm).any() else None,
                      "null_count": int(nm.sum()), "null_pct": 100.0 * float(nm.mean())}, ensure_ascii=False))
    return 0


def main(argv):
    if not argv:
        fail(2, "usage: fake_exe.py run|batch|info|version [options]")
    cmd, rest = argv[0], argv[1:]
    if cmd == "version":
        print(TOOL_VERSION)
        return 0
    opts = parse_opts(rest)
    if cmd == "run":
        return cmd_run(opts)
    if cmd == "batch":
        return cmd_batch(opts)
    if cmd == "info":
        return cmd_info(opts)
    fail(2, "unknown command: " + cmd)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except SystemExit:
        raise
    except Exception as e:  # noqa: BLE001
        fail(4, "%s: %s" % (type(e).__name__, e))
