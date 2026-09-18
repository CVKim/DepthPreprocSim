# -*- coding: utf-8 -*-
"""DepthPreprocSim verification harness (DESIGN.md section 5, cases T1..T5).

Usage:
  python tests/verify.py --exe <depth_sim.exe> --cases tests/cases.json --out tests/results [--only T1,T3]

Writes <out>/results.json and <out>/RESULTS.md.
Exit code: 0 when every selected case passed, 1 otherwise (also 1 when the exe is missing).
The exe may also be a .py stand-in (tests/fake_exe.py); it is then run with the current python.
"""
import argparse
import importlib
import json
import logging
import os
import shutil
import subprocess
import sys
import time
import traceback

import numpy as np

try:
    import cv2
    import tifffile
except ImportError as e:  # pragma: no cover
    print("[verify] missing python package: %s (need numpy, tifffile, opencv-python)" % e, file=sys.stderr)
    sys.exit(1)

logging.getLogger("tifffile").setLevel(logging.ERROR)

HERE = os.path.dirname(os.path.abspath(__file__))
CASE_ORDER = ["T1", "T2", "T3", "T4", "T5"]
METRIC_KEYS = ["exact_pct", "within1_pct", "within2_pct", "max_abs", "mismatch_count",
               "black_mask_iou", "ref_black_not_result", "result_black_not_ref"]


# ----------------------------------------------------------------------------- generic helpers
def log(msg):
    print("[verify] " + msg, flush=True)


def rm_rf(path):
    if os.path.isdir(path):
        shutil.rmtree(path, ignore_errors=True)


def decode(b):
    if b is None:
        return ""
    if isinstance(b, str):
        return b
    return b.decode("utf-8", errors="replace")


def last_json_line(text):
    """Parsed dict of the last JSON line in text (stderr contract: one-line {"error": ...}), else None."""
    for ln in reversed([ln.strip() for ln in text.splitlines() if ln.strip()]):
        try:
            obj = json.loads(ln)
        except ValueError:
            continue
        if isinstance(obj, dict):
            return obj
    return None


def load_json(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)


def fnum(v, nd=3):
    if v is None:
        return "-"
    if isinstance(v, float):
        return ("%%.%df" % nd) % v
    return str(v)


class Exe:
    def __init__(self, path):
        self.path = path
        self.is_py = path.lower().endswith(".py")

    def cmd(self, args):
        head = [sys.executable, self.path] if self.is_py else [self.path]
        return head + [str(a) for a in args]

    def run(self, args, timeout):
        cmd = self.cmd(args)
        t0 = time.time()
        try:
            cp = subprocess.run(cmd, capture_output=True, timeout=timeout)
            rc, out, err = cp.returncode, decode(cp.stdout), decode(cp.stderr)
        except subprocess.TimeoutExpired as e:
            rc, out, err = -999, decode(e.stdout), decode(e.stderr) + "\n[verify] TIMEOUT after %ds" % timeout
        except OSError as e:
            rc, out, err = -998, "", "[verify] cannot start process: %s" % e
        return {"cmd": subprocess.list2cmdline(cmd), "rc": rc, "stdout": out, "stderr": err,
                "elapsed_s": round(time.time() - t0, 2)}


# ----------------------------------------------------------------------------- images / metrics
def read_u8(path):
    """Load result/reference as 2-D uint8 (.mim/.tif via tifffile, float 0..255 -> u8; else cv2.imdecode)."""
    ext = os.path.splitext(path)[1].lower()
    if ext in (".mim", ".tif", ".tiff"):
        a = tifffile.imread(path)
    else:
        a = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    if a is None:
        raise IOError("cannot read image: %s" % path)
    if a.ndim == 3:
        a = a[:, :, 0]
    if a.dtype != np.uint8:
        a = np.clip(np.rint(a.astype(np.float64)), 0, 255).astype(np.uint8)
    return np.ascontiguousarray(a)


def compare_u8(result, ref):
    """Pixel metrics between two uint8 images. Black mask = value 0 (null or lower clip, as in the DLL)."""
    if result.shape != ref.shape:
        return {"error": "shape mismatch result=%s ref=%s" % (list(result.shape), list(ref.shape))}
    d = np.abs(result.astype(np.int16) - ref.astype(np.int16))
    n = int(d.size)
    exact = int(np.count_nonzero(d == 0))
    rb = ref == 0
    sb = result == 0
    inter = int(np.count_nonzero(rb & sb))
    union = int(np.count_nonzero(rb | sb))
    rb_only = int(np.count_nonzero(rb & ~sb))
    sb_only = int(np.count_nonzero(sb & ~rb))
    return {
        "total": n,
        "exact_pct": 100.0 * exact / n,
        "within1_pct": 100.0 * int(np.count_nonzero(d <= 1)) / n,
        "within2_pct": 100.0 * int(np.count_nonzero(d <= 2)) / n,
        "max_abs": int(d.max()),
        "mismatch_count": n - exact,
        "black_mask_iou": (inter / union) if union else 1.0,
        "ref_black_not_result": rb_only,
        "result_black_not_ref": sb_only,
        "ref_black_pct": 100.0 * int(np.count_nonzero(rb)) / n,
        "result_black_pct": 100.0 * int(np.count_nonzero(sb)) / n,
        # per-pixel agreement of the black masks (the "99.6%" figure of the python-reference memo)
        "black_agree_pct": 100.0 * (n - rb_only - sb_only) / n,
    }


def cross_check(mine, exe_ref, tol_pct):
    """Compare harness metrics with the exe's stats.json 'ref' block.
    pct fields: |a-b| <= tol_pct (percentage points); iou: tol_pct/100; counts: tol_pct% of total pixels; max_abs: exact."""
    if not isinstance(exe_ref, dict):
        return ["stats.json 'ref' block missing or null"]
    n = mine["total"]
    issues = []
    for k in METRIC_KEYS:
        if exe_ref.get(k) is None:
            issues.append("%s: missing in exe ref block" % k)
            continue
        try:
            a, b = float(mine[k]), float(exe_ref[k])
        except (TypeError, ValueError):
            issues.append("%s: non-numeric in exe ref block (%r)" % (k, exe_ref[k]))
            continue
        if k.endswith("_pct"):
            ok = abs(a - b) <= tol_pct
        elif k == "black_mask_iou":
            ok = abs(a - b) <= tol_pct / 100.0
        elif k == "max_abs":
            ok = a == b
        else:
            ok = abs(a - b) <= tol_pct / 100.0 * n
        if not ok:
            issues.append("%s: harness=%.6g exe=%.6g" % (k, a, b))
    return issues


def check_params(stats, cal_def, stage):
    """params_effective must reflect the CAL definition (patch/step/overlap/lower/upper/stage) and dll_identical=true."""
    pe = stats.get("params_effective")
    if not isinstance(pe, dict):
        return ["params_effective missing"]
    expect = {"patch_w": cal_def["patch"][0], "patch_h": cal_def["patch"][1],
              "step_w": cal_def["step"][0], "step_h": cal_def["step"][1],
              "overlap": float(cal_def["overlap"]),
              "lower": cal_def.get("lower", 5), "upper": cal_def.get("upper", 95),
              "stage": stage}
    issues = []
    for k, v in expect.items():
        got = pe.get(k)
        if got is None:
            issues.append("%s missing" % k)
            continue
        try:
            if isinstance(v, float):
                ok = abs(float(got) - v) < 1e-6
            elif isinstance(v, str):
                ok = str(got).upper() == v.upper()
            else:
                ok = int(got) == int(v)
        except (TypeError, ValueError):
            ok = False
        if not ok:
            issues.append("%s: expected %r got %r" % (k, v, got))
    if pe.get("dll_identical") is not True:
        issues.append("dll_identical is %r (expected true)" % pe.get("dll_identical"))
    return issues


def find_ref_file(folder, ref_idx):
    for g in sorted(os.listdir(folder)):
        if g.startswith("%d_" % ref_idx) and "_Proc" in g and g.lower().endswith(".mim"):
            return os.path.join(folder, g)
    return None


# ----------------------------------------------------------------------------- context
class Ctx(object):
    def __init__(self, exe, cases, out_dir):
        self.exe = exe
        self.cases = cases
        self.out = out_dir
        self.ini = cases["recipe"]["ini"]
        self.fovproc = cases["recipe"]["fovproc"]
        self.tires = cases["tires"]
        self.cals = cases["cals"]
        self.mapping = {int(k): v for k, v in cases["mapping"].items() if not k.startswith("_")}
        self.pairs = {t: {p["id"]: p for p in lst} for t, lst in cases["pairs"].items()}
        self.cases_dir = cases.get("_cases_dir", HERE)   # base for relative paths in cases.json
        to = cases.get("timeouts_sec", {})
        self.timeout_run = to.get("run", 600)
        self.timeout_batch = to.get("batch", 1800)

    def pair(self, tire, pid):
        return self.pairs[tire][pid]

    def cal(self, n):
        return self.cals[str(n)]


def run_pair(ctx, pair, out_dir, extra=(), stage="AUTO", with_ref=True, tol_pct=0.01):
    """Run `depth_sim run` for one raw/ref pair; return a record with metrics, cross-check and params issues."""
    rm_rf(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    rec = {"id": pair["id"], "kind": pair["kind"], "cal": pair["cal"], "raw": pair["raw"],
           "ref": pair["ref"] if with_ref else None, "out_dir": out_dir, "issues": []}
    for p in (pair["raw"], pair["ref"] if with_ref else None):
        if p and not os.path.isfile(p):
            rec["issues"].append("input missing: %s" % p)
    if rec["issues"]:
        rec["status"] = "FAIL"
        return rec
    args = ["run", "--in", pair["raw"], "--out", out_dir]
    if with_ref:
        args += ["--ref", pair["ref"]]
    args += ["--ini", ctx.ini, "--cal", pair["cal"], "--dump", "min"] + list(extra)
    rr = ctx.exe.run(args, ctx.timeout_run)
    rec.update({"cmd": rr["cmd"], "rc": rr["rc"], "elapsed_s": rr["elapsed_s"], "stderr_tail": rr["stderr"][-600:]})
    if rr["rc"] != 0:
        rec["issues"].append("exit code %d" % rr["rc"])
        rec["status"] = "FAIL"
        return rec
    result_png = os.path.join(out_dir, "result.png")
    stats_json = os.path.join(out_dir, "stats.json")
    missing = [os.path.basename(p) for p in (result_png, stats_json) if not os.path.isfile(p)]
    if missing:
        rec["issues"].append("missing output: %s" % ", ".join(missing))
        rec["status"] = "FAIL"
        return rec
    try:
        stats = load_json(stats_json)
    except ValueError as e:
        rec["issues"].append("stats.json not valid JSON: %s" % e)
        rec["status"] = "FAIL"
        return rec
    rec["exe_total_ms"] = (stats.get("timing_ms") or {}).get("total")
    rec["exe_ref"] = stats.get("ref")
    rec["stats_null"] = stats.get("null")
    rec["stats_clip"] = stats.get("clip")
    rec["params_issues"] = check_params(stats, ctx.cal(pair["cal"]), stage)
    result = read_u8(result_png)
    rec["result_shape"] = list(result.shape)
    if with_ref:
        m = compare_u8(result, read_u8(pair["ref"]))
        rec["metrics"] = m
        if "error" in m:
            rec["issues"].append(m["error"])
        else:
            rec["cross_issues"] = cross_check(m, stats.get("ref"), tol_pct)
    rec["status"] = "PASS" if not rec["issues"] else "FAIL"
    return rec


def finalize(rec):
    rec["status"] = "PASS" if not rec["issues"] else "FAIL"
    return rec


def overall(rows, key="status"):
    if not rows:
        return "FAIL"
    return "PASS" if all(r.get(key) == "PASS" for r in rows) else "FAIL"


def has_metrics(rec):
    return isinstance(rec.get("metrics"), dict) and "error" not in rec["metrics"]


# ----------------------------------------------------------------------------- cases
def case_T1(ctx, case, state):
    exp = case.get("expect", {})
    tol = exp.get("cross_check_tol_pct", 0.01)
    iou_min = exp.get("black_mask_iou_min", 0.99)
    exact_min = exp.get("exact_min_pct", None)
    rows = []
    for pid in case["pairs"]:
        pair = ctx.pair(case["tire"], pid)
        log("T1 run %s (CAL%d)" % (pid, pair["cal"]))
        rec = run_pair(ctx, pair, os.path.join(ctx.out, "T1", pid), tol_pct=tol)
        if has_metrics(rec):
            if rec["metrics"]["black_mask_iou"] < iou_min:
                rec["issues"].append("black_mask_iou %.4f < %.2f" % (rec["metrics"]["black_mask_iou"], iou_min))
            if exact_min is not None and rec["metrics"]["exact_pct"] < float(exact_min):
                rec["issues"].append("exact_pct %.3f < %.2f" % (rec["metrics"]["exact_pct"], float(exact_min)))
            if rec.get("cross_issues"):
                rec["issues"].append("cross-check: " + "; ".join(rec["cross_issues"]))
            if rec.get("params_issues"):
                rec["issues"].append("params: " + "; ".join(rec["params_issues"]))
            log("T1 %s exact=%.3f%% within1=%.3f%% within2=%.3f%% iou=%.4f (%ss)" % (
                pid, rec["metrics"]["exact_pct"], rec["metrics"]["within1_pct"], rec["metrics"]["within2_pct"],
                rec["metrics"]["black_mask_iou"], rec.get("elapsed_s")))
        finalize(rec)
        rows.append(rec)
    state["T1"] = {r["id"]: r for r in rows}
    return {"desc": case.get("desc", ""), "status": overall(rows), "rows": rows}


def prior_T1(ctx, pair):
    """Rebuild a T1 record from a previous invocation's on-disk output (results/T1/<id>/), or None."""
    d = os.path.join(ctx.out, "T1", pair["id"])
    png, sj = os.path.join(d, "result.png"), os.path.join(d, "stats.json")
    if not (os.path.isfile(png) and os.path.isfile(sj) and os.path.isfile(pair["ref"])):
        return None
    try:
        stats = load_json(sj)
    except ValueError:
        return None
    m = compare_u8(read_u8(png), read_u8(pair["ref"]))
    if "error" in m:
        return None
    return {"id": pair["id"], "metrics": m, "out_dir": d, "stats_null": stats.get("null"), "issues": [],
            "status": "PASS", "from_disk": True}


def case_T2(ctx, case, state):
    stage = case.get("stage", "NONE")
    drop_min = float(case.get("expect", {}).get("exact_drop_min_pp", 20.0))
    rows = []
    for pid in case["pairs"]:
        pair = ctx.pair(case["tire"], pid)
        base = state.get("T1", {}).get(pid)
        if not (base and has_metrics(base)):
            base = prior_T1(ctx, pair)
        if not (base and has_metrics(base)):
            log("T2 baseline run %s (no T1 result available)" % pid)
            base = run_pair(ctx, pair, os.path.join(ctx.out, "T2", pid + "_baseline"))
        log("T2 run %s --stage %s" % (pid, stage))
        rec = run_pair(ctx, pair, os.path.join(ctx.out, "T2", pid), extra=["--stage", stage], stage=stage)
        rec["t1_exact_pct"] = base["metrics"]["exact_pct"] if has_metrics(base) else None
        rec["t1_black_mask_iou"] = base["metrics"]["black_mask_iou"] if has_metrics(base) else None
        rec["stage_removed_auto"] = (base.get("stats_null") or {}).get("stage_removed_count")
        rec["stage_removed_none"] = (rec.get("stats_null") or {}).get("stage_removed_count")
        if has_metrics(rec) and rec["t1_exact_pct"] is not None:
            rec["drop_pp"] = rec["t1_exact_pct"] - rec["metrics"]["exact_pct"]
            if rec["drop_pp"] < drop_min:
                rec["issues"].append("exact_pct drop %.2f pp < %.1f pp" % (rec["drop_pp"], drop_min))
            if rec.get("params_issues"):
                rec["issues"].append("params: " + "; ".join(rec["params_issues"]))
            # informational: how much the NONE output differs from the AUTO output of the same exe
            base_png = os.path.join(base.get("out_dir", ""), "result.png")
            if os.path.isfile(base_png):
                mm = compare_u8(read_u8(base_png), read_u8(os.path.join(rec["out_dir"], "result.png")))
                rec["auto_vs_none_exact_pct"] = mm.get("exact_pct")
            log("T2 %s exact T1=%.3f%% NONE=%.3f%% drop=%.2f pp (AUTO<->NONE identical %s%%)" % (
                pid, rec["t1_exact_pct"], rec["metrics"]["exact_pct"], rec["drop_pp"],
                fnum(rec.get("auto_vs_none_exact_pct"), 2)))
        elif has_metrics(rec):
            rec["issues"].append("no T1 baseline metrics")
        finalize(rec)
        rows.append(rec)
    return {"desc": case.get("desc", ""), "status": overall(rows), "rows": rows}


def import_reference(path):
    """Import simulate.py by inserting its folder into sys.path (module exposes load, remove_stage, process_high_curvature)."""
    folder = os.path.dirname(os.path.abspath(path))
    modname = os.path.splitext(os.path.basename(path))[0]
    if folder not in sys.path:
        sys.path.insert(0, folder)
    mod = importlib.import_module(modname)
    for fn in ("load", "remove_stage", "process_high_curvature"):
        if not hasattr(mod, fn):
            raise ImportError("python reference lacks %s()" % fn)
    return mod


def write_png(path, arr):
    ok, buf = cv2.imencode(".png", arr)
    if not ok:
        raise IOError("png encode failed")
    buf.tofile(path)


def case_T3(ctx, case, state):
    within2_min = float(case.get("expect", {}).get("within2_min_pct", 96.0))
    ref_py = ctx.cases["python_reference"]
    if not os.path.isabs(ref_py):   # relative -> relative to the cases.json folder (in-repo copy)
        ref_py = os.path.normpath(os.path.join(ctx.cases_dir, ref_py))
    if not os.path.isfile(ref_py):
        return {"desc": case.get("desc", ""), "status": "FAIL", "rows": [],
                "error": "python reference not found: %s" % ref_py}
    mod = import_reference(ref_py)
    rows = []
    for pid in case["pairs"]:
        pair = ctx.pair(case["tire"], pid)
        cal = ctx.cal(pair["cal"])
        rec = {"id": pid, "kind": pair["kind"], "cal": pair["cal"], "issues": []}
        exe_png = os.path.join(ctx.out, "T1", pid, "result.png")
        if not os.path.isfile(exe_png):
            log("T3 exe run %s (T1 result not available)" % pid)
            r = run_pair(ctx, pair, os.path.join(ctx.out, "T3", pid + "_exe"))
            rec["exe_run"] = {"rc": r.get("rc"), "elapsed_s": r.get("elapsed_s"), "issues": r["issues"]}
            if r["issues"]:
                rec["issues"].append("exe run failed: " + "; ".join(r["issues"]))
                rows.append(finalize(rec))
                continue
            exe_png = os.path.join(r["out_dir"], "result.png")
        rec["exe_result"] = exe_png
        if not os.path.isfile(pair["raw"]):
            rec["issues"].append("input missing: %s" % pair["raw"])
            rows.append(finalize(rec))
            continue
        log("T3 python reference %s patch=%s overlap=%s" % (pid, cal["patch"], cal["overlap"]))
        t0 = time.time()
        raw = tifffile.imread(pair["raw"]).astype(np.float32)
        # site build: stage removal depends on DepthPreprocType (INNERCENTER none / BEAD / INSHOULDER erode 3)
        if hasattr(mod, "remove_stage_for_type"):
            src = mod.remove_stage_for_type(raw, cal.get("name", "INSHOULDER"))
        else:
            src = mod.remove_stage(raw)
        out = mod.process_high_curvature(src, patch=tuple(cal["patch"]), overlap=float(cal["overlap"]))
        py_u8 = out[0] if isinstance(out, tuple) else out
        rec["py_low_high"] = [float(out[1]), float(out[2])] if isinstance(out, tuple) and len(out) >= 3 else None
        rec["py_elapsed_s"] = round(time.time() - t0, 2)
        del raw, src, out
        py_dir = os.path.join(ctx.out, "T3", pid + "_py")
        os.makedirs(py_dir, exist_ok=True)
        write_png(os.path.join(py_dir, "result.png"), py_u8)
        exe_u8 = read_u8(exe_png)
        m = compare_u8(exe_u8, py_u8)
        rec["metrics_exe_vs_py"] = m
        if "error" in m:
            rec["issues"].append(m["error"])
        else:
            if m["within2_pct"] < within2_min:
                rec["issues"].append("within2 %.3f%% < %.1f%%" % (m["within2_pct"], within2_min))
            if os.path.isfile(pair["ref"]):
                rec["metrics_py_vs_site"] = compare_u8(py_u8, read_u8(pair["ref"]))
            log("T3 %s exe-vs-py exact=%.3f%% within2=%.3f%% iou=%.4f (py %.1fs)" % (
                pid, m["exact_pct"], m["within2_pct"], m["black_mask_iou"], rec["py_elapsed_s"]))
        rows.append(finalize(rec))
    return {"desc": case.get("desc", ""), "status": overall(rows), "rows": rows}


def batch_items(obj):
    if isinstance(obj, list):
        return obj
    if isinstance(obj, dict):
        for k in ("items", "results", "images"):
            if isinstance(obj.get(k), list):
                return obj[k]
    return None


def case_T4(ctx, case, state):
    exp = case.get("expect", {})
    tol = exp.get("cross_check_tol_pct", 0.01)
    iou_min = exp.get("black_mask_iou_min", 0.99)
    exact_min = exp.get("exact_min_pct", None)
    want_items = int(case.get("batch_items", 8))
    tire_dir = ctx.tires[case["tire"]]
    out_b = os.path.join(ctx.out, "T4", "batch")
    rm_rf(out_b)
    os.makedirs(out_b, exist_ok=True)
    res = {"desc": case.get("desc", ""), "issues": [], "warnings": [], "rows": [], "runs": []}
    if not os.path.isdir(tire_dir):
        res["issues"].append("tire folder missing: %s" % tire_dir)
        return finalize(res)
    args = ["batch", "--folder", tire_dir, "--out", out_b, "--fovproc", ctx.fovproc, "--ini", ctx.ini, "--dump", "min"]
    log("T4 batch %s" % tire_dir)
    rr = ctx.exe.run(args, ctx.timeout_batch)
    res.update({"cmd": rr["cmd"], "rc": rr["rc"], "elapsed_s": rr["elapsed_s"], "stderr_tail": rr["stderr"][-600:]})
    if rr["rc"] != 0:
        res["issues"].append("batch exit code %d" % rr["rc"])
        return finalize(res)
    bj = os.path.join(out_b, "batch.json")
    items = None
    if not os.path.isfile(bj):
        res["issues"].append("batch.json missing")
    else:
        try:
            items = batch_items(load_json(bj))
        except ValueError as e:
            res["issues"].append("batch.json invalid JSON: %s" % e)
        if items is None:
            res["issues"].append("batch.json has no item array")
        else:
            res["batch_json_items"] = len(items)
            if len(items) != want_items:
                res["issues"].append("batch.json items %d != %d" % (len(items), want_items))
    folders = sorted([d for d in os.listdir(out_b)
                      if os.path.isdir(os.path.join(out_b, d))
                      and os.path.isfile(os.path.join(out_b, d, "result.png"))
                      and os.path.isfile(os.path.join(out_b, d, "stats.json"))],
                     key=lambda d: int(d.split("_")[0]) if d.split("_")[0].isdigit() else 999)
    res["item_folders"] = folders
    if len(folders) != want_items:
        res["issues"].append("item folders with result.png+stats.json: %d != %d" % (len(folders), want_items))
    by_idx = {}
    for d in folders:
        head = d.split("_")[0]
        row = {"folder": d, "issues": []}
        if not head.isdigit() or int(head) not in ctx.mapping:
            row["issues"].append("folder name does not start with a mapped imgIdx")
            res["rows"].append(finalize(row))
            continue
        idx = int(head)
        mp = ctx.mapping[idx]
        row.update({"imgIdx": idx, "ref_idx": mp["ref"], "cal": mp["cal"]})
        by_idx[idx] = d
        try:
            stats = load_json(os.path.join(out_b, d, "stats.json"))
        except ValueError as e:
            row["issues"].append("stats.json invalid: %s" % e)
            res["rows"].append(finalize(row))
            continue
        row["params_issues"] = check_params(stats, ctx.cal(mp["cal"]), "AUTO")
        if row["params_issues"]:
            row["issues"].append("params: " + "; ".join(row["params_issues"]))
        ref_path = find_ref_file(tire_dir, mp["ref"])
        row["ref"] = ref_path
        if ref_path is None:
            row["issues"].append("reference _Proc for idx %d not found" % mp["ref"])
        else:
            m = compare_u8(read_u8(os.path.join(out_b, d, "result.png")), read_u8(ref_path))
            row["metrics"] = m
            if "error" in m:
                row["issues"].append(m["error"])
            else:
                row["cross_issues"] = cross_check(m, stats.get("ref"), tol)
                if row["cross_issues"]:
                    row["issues"].append("cross-check: " + "; ".join(row["cross_issues"]))
                # same thresholds as T1: below-threshold items fail the case (not just a warning)
                if m["black_mask_iou"] < iou_min:
                    row["issues"].append("black_mask_iou %.4f < %.2f" % (m["black_mask_iou"], iou_min))
                if exact_min is not None and m["exact_pct"] < float(exact_min):
                    row["issues"].append("exact_pct %.3f < %.2f" % (m["exact_pct"], float(exact_min)))
                log("T4 item %s exact=%.3f%% within2=%.3f%% iou=%.4f" % (d, m["exact_pct"], m["within2_pct"], m["black_mask_iou"]))
        res["rows"].append(finalize(row))
    # individual runs must reproduce the batch output byte for byte
    for pid in case.get("individual_pairs", []):
        pair = ctx.pair(case["tire"], pid)
        log("T4 individual run %s" % pid)
        rec = run_pair(ctx, pair, os.path.join(ctx.out, "T4", "run_" + pid), tol_pct=tol)
        if rec.get("cross_issues"):
            rec["issues"].append("cross-check: " + "; ".join(rec["cross_issues"]))
        if has_metrics(rec):
            if rec["metrics"]["black_mask_iou"] < iou_min:
                rec["issues"].append("black_mask_iou %.4f < %.2f" % (rec["metrics"]["black_mask_iou"], iou_min))
            if exact_min is not None and rec["metrics"]["exact_pct"] < float(exact_min):
                rec["issues"].append("exact_pct %.3f < %.2f" % (rec["metrics"]["exact_pct"], float(exact_min)))
        d = by_idx.get(pair["raw_idx"])
        if d is None:
            rec["issues"].append("no batch folder for imgIdx %d" % pair["raw_idx"])
        elif rec["status"] != "FAIL" and os.path.isfile(os.path.join(rec["out_dir"], "result.png")):
            a = read_u8(os.path.join(rec["out_dir"], "result.png"))
            b = read_u8(os.path.join(out_b, d, "result.png"))
            rec["identical_to_batch"] = bool(a.shape == b.shape and np.array_equal(a, b))
            if not rec["identical_to_batch"]:
                rec["issues"].append("run output differs from batch output (%s)" % d)
        res["runs"].append(finalize(rec))
    if any(r["status"] != "PASS" for r in res["rows"]):
        res["issues"].append("%d batch item(s) failed" % sum(1 for r in res["rows"] if r["status"] != "PASS"))
    if any(r["status"] != "PASS" for r in res["runs"]):
        res["issues"].append("%d individual run(s) failed" % sum(1 for r in res["runs"] if r["status"] != "PASS"))
    return finalize(res)


def case_T5(ctx, case, state):
    exp = case.get("expect", {})
    miss_exit = int(exp.get("missing_exit", 3))
    bad_exit = int(exp.get("bad_args_exit", 2))
    pair = ctx.pair(case["tire"], case["valid_pair"])
    base = os.path.join(ctx.out, "T5")
    rm_rf(base)
    rows = []

    def scenario(name, args, want_rc, need_json):
        out_dir = os.path.join(base, name)
        os.makedirs(out_dir, exist_ok=True)
        rr = ctx.exe.run(args + ["--out", out_dir], 120)
        js = last_json_line(rr["stderr"])
        rec = {"name": name, "cmd": rr["cmd"], "want_rc": want_rc, "rc": rr["rc"],
               "stderr_json": js, "stderr_tail": rr["stderr"][-300:], "issues": []}
        if rr["rc"] != want_rc:
            rec["issues"].append("exit code %d != %d" % (rr["rc"], want_rc))
        if js is None or "error" not in js:
            msg = "stderr has no one-line JSON with 'error'"
            if need_json:
                rec["issues"].append(msg)
            else:
                rec["warning"] = msg
        log("T5 %s rc=%d (want %d) json=%s" % (name, rr["rc"], want_rc, js is not None))
        return finalize(rec)

    rows.append(scenario("missing_input", ["run", "--in", case["missing_input"]], miss_exit, True))
    for i, bad in enumerate(case.get("bad_args", [])):
        rows.append(scenario("bad_args_%d_%s" % (i + 1, bad[0].lstrip("-")),
                             ["run", "--in", pair["raw"]] + list(bad), bad_exit, False))
    return {"desc": case.get("desc", ""), "status": overall(rows), "rows": rows}


CASE_FN = {"T1": case_T1, "T2": case_T2, "T3": case_T3, "T4": case_T4, "T5": case_T5}


# ----------------------------------------------------------------------------- report
def md_issues(L, rows, key="id"):
    bad = [r for r in rows if r.get("issues")]
    if bad:
        L.append("")
        for r in bad:
            L.append("- **%s**: %s" % (r.get(key, r.get("folder", r.get("name", "?"))), " / ".join(r["issues"])))


def md_T1(L, c):
    L.append("| 쌍(raw→ref) | 종류 | CAL | exact % | ±1 % | ±2 % | max\\|Δ\\| | 검정 IoU | 검정 일치 % | 참조만 검정 | 결과만 검정 | 교차검증 | 파라미터 | exe 시간(s) | 판정 |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in c["rows"]:
        m = r.get("metrics") or {}
        L.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["id"], r.get("kind"), r.get("cal"), fnum(m.get("exact_pct")), fnum(m.get("within1_pct")),
            fnum(m.get("within2_pct")), fnum(m.get("max_abs")), fnum(m.get("black_mask_iou"), 4),
            fnum(m.get("black_agree_pct")), fnum(m.get("ref_black_not_result")), fnum(m.get("result_black_not_ref")),
            "일치" if r.get("cross_issues") == [] else ("불일치(%d)" % len(r["cross_issues"]) if r.get("cross_issues") else "-"),
            "일치" if r.get("params_issues") == [] else ("불일치(%d)" % len(r["params_issues"]) if r.get("params_issues") else "-"),
            fnum(r.get("elapsed_s"), 1), r["status"]))
    md_issues(L, c["rows"])


def md_T2(L, c):
    L.append("| 쌍 | T1(AUTO) exact % | NONE exact % | 하락(pp) | AUTO↔NONE 동일 % | 지그 제거 화소 AUTO / NONE | NONE 검정 IoU | 판정 |")
    L.append("|---|---|---|---|---|---|---|---|")
    for r in c["rows"]:
        m = r.get("metrics") or {}
        L.append("| %s | %s | %s | %s | %s | %s / %s | %s | %s |" % (
            r["id"], fnum(r.get("t1_exact_pct")), fnum(m.get("exact_pct")), fnum(r.get("drop_pp"), 2),
            fnum(r.get("auto_vs_none_exact_pct"), 2), fnum(r.get("stage_removed_auto")), fnum(r.get("stage_removed_none")),
            fnum(m.get("black_mask_iou"), 4), r["status"]))
    md_issues(L, c["rows"])


def md_T3(L, c):
    if c.get("error"):
        L.append("- 오류: %s" % c["error"])
    L.append("| 쌍 | exe↔py exact % | ±1 % | ±2 % | 검정 IoU | py↔현장 exact % | py↔현장 ±2 % | py↔현장 검정 IoU | py 시간(s) | 판정 |")
    L.append("|---|---|---|---|---|---|---|---|---|---|")
    for r in c["rows"]:
        m = r.get("metrics_exe_vs_py") or {}
        s = r.get("metrics_py_vs_site") or {}
        L.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["id"], fnum(m.get("exact_pct")), fnum(m.get("within1_pct")), fnum(m.get("within2_pct")),
            fnum(m.get("black_mask_iou"), 4), fnum(s.get("exact_pct")), fnum(s.get("within2_pct")),
            fnum(s.get("black_mask_iou"), 4), fnum(r.get("py_elapsed_s"), 1), r["status"]))
    md_issues(L, c["rows"])


def md_T4(L, c):
    L.append("- batch 종료 코드: %s, 소요 %s s, batch.json 항목 %s, 결과 폴더 %s개" % (
        c.get("rc"), fnum(c.get("elapsed_s"), 1), c.get("batch_json_items"), len(c.get("item_folders", []))))
    if c.get("warnings"):
        L.append("- 경고: " + " / ".join(c["warnings"]))
    L.append("")
    L.append("| 폴더 | imgIdx→ref | CAL | exact % | ±1 % | ±2 % | 검정 IoU | 검정 일치 % | 교차검증 | 파라미터 | 판정 |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in c.get("rows", []):
        m = r.get("metrics") or {}
        L.append("| %s | %s→%s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["folder"], r.get("imgIdx"), r.get("ref_idx"), r.get("cal"), fnum(m.get("exact_pct")),
            fnum(m.get("within1_pct")), fnum(m.get("within2_pct")), fnum(m.get("black_mask_iou"), 4),
            fnum(m.get("black_agree_pct")),
            "일치" if r.get("cross_issues") == [] else ("불일치(%d)" % len(r["cross_issues"]) if r.get("cross_issues") else "-"),
            "일치" if r.get("params_issues") == [] else ("불일치(%d)" % len(r["params_issues"]) if r.get("params_issues") else "-"),
            r["status"]))
    md_issues(L, c.get("rows", []), key="folder")
    if c.get("runs"):
        L.append("")
        L.append("개별 `run` 과 batch 결과 동일성:")
        L.append("")
        L.append("| 쌍 | exact % | 검정 IoU | batch 와 동일 | 시간(s) | 판정 |")
        L.append("|---|---|---|---|---|---|")
        for r in c["runs"]:
            m = r.get("metrics") or {}
            L.append("| %s | %s | %s | %s | %s | %s |" % (r["id"], fnum(m.get("exact_pct")), fnum(m.get("black_mask_iou"), 4),
                                                         r.get("identical_to_batch", "-"), fnum(r.get("elapsed_s"), 1), r["status"]))
        md_issues(L, c["runs"])
    if c.get("issues"):
        L.append("")
        L.append("- 케이스 이슈: " + " / ".join(c["issues"]))


def md_T5(L, c):
    L.append("| 시나리오 | 기대 exit | 실제 exit | stderr JSON | 판정 |")
    L.append("|---|---|---|---|---|")
    for r in c["rows"]:
        js = r.get("stderr_json")
        L.append("| %s | %s | %s | %s | %s |" % (r["name"], r["want_rc"], r["rc"],
                                                ("`%s`" % json.dumps(js, ensure_ascii=False)[:120]) if js else "없음", r["status"]))
    md_issues(L, c["rows"], key="name")


MD_FN = {"T1": md_T1, "T2": md_T2, "T3": md_T3, "T4": md_T4, "T5": md_T5}
TITLES = {"T1": "T1 tire1 기본 파라미터 vs 현장 _Proc", "T2": "T2 --stage NONE 대비 하락(removeStage 확인)",
          "T3": "T3 파이썬 레퍼런스(withStage) 대비", "T4": "T4 tire2 batch(--fovproc) + 개별 run 동일성",
          "T5": "T5 CLI 오류 경로"}


def write_results_md(path, results):
    meta = results["meta"]
    L = ["# DepthPreprocSim 검증 결과 (tests/verify.py)", "",
         "- 실행 시각: %s" % meta["started"],
         "- 실행 파일: `%s`" % meta["exe"],
         "- 실행 파일 `version` 출력: `%s`" % (meta.get("exe_version") or "(없음)"),
         "- 케이스 정의: `%s`" % meta["cases_file"],
         "- 선택 케이스: %s" % ", ".join(meta["selected"]),
         "- 총 소요 시간: %.1f s" % meta["elapsed_s"],
         "- 전체 판정: **%s**" % meta["overall"], "",
         "## 요약", "", "| 케이스 | 판정 | 설명 |", "|---|---|---|"]
    for name in CASE_ORDER:
        c = results["cases"].get(name)
        if c:
            L.append("| %s | %s | %s |" % (name, c["status"], c.get("desc", "")))
    L.append("")
    L.append("측정 정의: exact = 화소값 동일 비율, ±1/±2 = |result-ref| 가 1/2 이하인 비율, 검정 IoU = (result==0)∩(ref==0) / (result==0)∪(ref==0), "
             "검정 일치 % = 두 영상의 검정(0) 여부가 같은 화소 비율, 참조만 검정 = ref==0 이고 result!=0 인 화소 수, 결과만 검정 = 그 반대. "
             "교차검증 = 위 값과 exe 의 stats.json `ref` 블록 일치 여부(허용 0.01%).")
    L.append("")
    for name in CASE_ORDER:
        c = results["cases"].get(name)
        if not c:
            continue
        L.append("## %s — %s" % (TITLES.get(name, name), c["status"]))
        L.append("")
        if c.get("desc"):
            L.append(c["desc"])
            L.append("")
        if c.get("traceback"):
            L.append("```")
            L.append(c["traceback"].rstrip())
            L.append("```")
        else:
            MD_FN[name](L, c)
        L.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")


# ----------------------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description="DepthPreprocSim verification harness (DESIGN.md section 5)")
    ap.add_argument("--exe", required=True, help="depth_sim.exe (or tests/fake_exe.py stand-in)")
    ap.add_argument("--cases", default=os.path.join(HERE, "cases.json"))
    ap.add_argument("--out", default=os.path.join(HERE, "results"))
    ap.add_argument("--only", default="", help="comma list, e.g. T1,T3")
    args = ap.parse_args(argv)

    exe_path = os.path.abspath(args.exe)
    if not os.path.isfile(exe_path):
        print("[verify] depth_sim executable not found: %s" % exe_path, file=sys.stderr)
        print("[verify] build it first (build.bat -> build\\Release\\depth_sim.exe) or pass --exe <path>.", file=sys.stderr)
        return 1
    if not os.path.isfile(args.cases):
        print("[verify] cases file not found: %s" % args.cases, file=sys.stderr)
        return 1
    cases = load_json(args.cases)
    cases["_cases_dir"] = os.path.dirname(os.path.abspath(args.cases))
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    selected = [c.strip().upper() for c in args.only.split(",") if c.strip()] or list(CASE_ORDER)
    unknown = [c for c in selected if c not in CASE_FN]
    if unknown:
        print("[verify] unknown case(s): %s (valid: %s)" % (", ".join(unknown), ", ".join(CASE_ORDER)), file=sys.stderr)
        return 1

    ctx = Ctx(Exe(exe_path), cases, out_dir)
    t_start = time.time()
    ver = ctx.exe.run(["version"], 60)
    exe_version = (ver["stdout"].strip().splitlines() or [""])[0] if ver["rc"] == 0 else "(version failed rc=%d)" % ver["rc"]
    log("exe: %s" % exe_path)
    log("version: %s" % exe_version)

    results = {"meta": {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "exe": exe_path, "exe_version": exe_version,
                        "cases_file": os.path.abspath(args.cases), "out": out_dir, "selected": selected,
                        "python": sys.version.split()[0], "numpy": np.__version__, "cv2": cv2.__version__},
               "cases": {}}
    state = {}
    for name in CASE_ORDER:
        if name not in selected:
            continue
        case = cases["cases"].get(name)
        if case is None:
            results["cases"][name] = {"status": "FAIL", "desc": "", "error": "case not defined in cases.json", "rows": []}
            continue
        log("=== %s ===" % name)
        t0 = time.time()
        try:
            res = CASE_FN[name](ctx, case, state)
        except Exception:
            res = {"desc": case.get("desc", ""), "status": "FAIL", "rows": [], "traceback": traceback.format_exc()}
            log("%s crashed:\n%s" % (name, res["traceback"]))
        res["case_elapsed_s"] = round(time.time() - t0, 1)
        results["cases"][name] = res
        log("%s -> %s (%.1fs)" % (name, res["status"], res["case_elapsed_s"]))

    results["meta"]["elapsed_s"] = round(time.time() - t_start, 1)
    results["meta"]["overall"] = "PASS" if all(c["status"] == "PASS" for c in results["cases"].values()) else "FAIL"
    with open(os.path.join(out_dir, "results.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, ensure_ascii=False, indent=1)
    write_results_md(os.path.join(out_dir, "RESULTS.md"), results)
    log("summary: " + ", ".join("%s=%s" % (k, v["status"]) for k, v in results["cases"].items()))
    log("overall %s; wrote %s and %s" % (results["meta"]["overall"], os.path.join(out_dir, "RESULTS.md"),
                                         os.path.join(out_dir, "results.json")))
    return 0 if results["meta"]["overall"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
