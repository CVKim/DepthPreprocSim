# -*- coding: utf-8 -*-
"""UI smoke test for ui/server.js (DESIGN.md section 5, T6).

Starts `node ui/server.js --port <port> --cli <exe> [--ini <ini>]`, waits for the port, then calls
/api/config, /api/run (tire1 7->12), /api/pixel (x=4000,y=1200), GET /runs/<id>/result.png and
/api/batch (tire2, up to 20 minutes), checks HTTP 200 + expected keys, and terminates the server.

Usage:
  python tests/ui_smoke.py --exe <depth_sim.exe> [--port 8799] [--node <node.exe>] [--server ui/server.js]
                           [--cases tests/cases.json] [--no-ini] [--skip-batch] [--batch-timeout 1200]
Exit code 0 when all checks pass, 1 otherwise. Writes tests/results/ui_smoke.json and ui_smoke_server.log.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_NODE = r"C:\Program Files\nodejs\node.exe"


def log(msg):
    print("[ui_smoke] " + msg, flush=True)


def wait_port(host, port, timeout, proc):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return True
        except OSError:
            time.sleep(0.3)
    return False


def http(method, url, body=None, timeout=60):
    """Return (status, parsed_json_or_text, content_type). Never raises for HTTP errors."""
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json; charset=utf-8"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            status, ctype = resp.status, resp.headers.get("Content-Type", "")
    except urllib.error.HTTPError as e:
        raw = e.read()
        status, ctype = e.code, e.headers.get("Content-Type", "") if e.headers else ""
    except (urllib.error.URLError, socket.timeout, OSError) as e:
        return 0, "connection error: %s" % e, ""
    if "json" in ctype or (raw[:1] in (b"{", b"[")):
        try:
            return status, json.loads(raw.decode("utf-8", errors="replace")), ctype
        except ValueError:
            pass
    if ctype.startswith("image/") or ctype.startswith("application/octet-stream"):
        return status, raw, ctype
    return status, raw.decode("utf-8", errors="replace"), ctype


class Checks(object):
    def __init__(self):
        self.items = []

    def add(self, name, ok, detail=""):
        self.items.append({"name": name, "ok": bool(ok), "detail": str(detail)[:500]})
        log("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))
        return ok

    def all_ok(self):
        return all(c["ok"] for c in self.items)


def pick_preset(cfg, cal=3, name="INSHOULDER"):
    for pr in cfg.get("presets") or []:
        if isinstance(pr, dict) and (str(pr.get("cal")) == str(cal) or str(pr.get("name", "")).upper() == name
                                     or str(pr.get("type", "")).upper() == name):
            return pr
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description="DepthPreprocSim UI smoke test (T6)")
    ap.add_argument("--exe", required=True, help="depth_sim.exe passed to the server as --cli")
    ap.add_argument("--port", type=int, default=8799)
    ap.add_argument("--node", default=None, help="node executable (default: PATH, then %s)" % DEFAULT_NODE)
    ap.add_argument("--server", default=os.path.join(ROOT, "ui", "server.js"))
    ap.add_argument("--cases", default=os.path.join(HERE, "cases.json"))
    ap.add_argument("--no-ini", action="store_true", help="do not pass --ini <recipe ini> to the server")
    ap.add_argument("--skip-batch", action="store_true")
    ap.add_argument("--batch-timeout", type=int, default=1200)
    ap.add_argument("--start-timeout", type=int, default=30)
    ap.add_argument("--out", default=os.path.join(HERE, "results"))
    args = ap.parse_args(argv)

    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        print("[ui_smoke] exe not found: %s" % exe, file=sys.stderr)
        return 1
    if not os.path.isfile(args.server):
        print("[ui_smoke] server script not found: %s" % args.server, file=sys.stderr)
        return 1
    node = args.node or shutil.which("node") or DEFAULT_NODE
    if not os.path.isfile(node) and shutil.which(node) is None:
        print("[ui_smoke] node not found: %s" % node, file=sys.stderr)
        return 1
    with open(args.cases, "r", encoding="utf-8-sig") as f:
        cases = json.load(f)
    tire1 = {p["id"]: p for p in cases["pairs"]["tire1"]}["7_12"]
    tire2_dir = cases["tires"]["tire2"]
    ini = cases["recipe"]["ini"]
    fovproc = cases["recipe"]["fovproc"]
    os.makedirs(args.out, exist_ok=True)

    cmd = [node, args.server, "--port", str(args.port), "--cli", exe]
    if not args.no_ini and os.path.isfile(ini):
        cmd += ["--ini", ini]
    log("starting: " + subprocess.list2cmdline(cmd))
    log_path = os.path.join(args.out, "ui_smoke_server.log")
    logf = open(log_path, "wb")
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT)
    base = "http://127.0.0.1:%d" % args.port
    ck = Checks()
    report = {"cmd": subprocess.list2cmdline(cmd), "base": base, "checks": ck.items}
    try:
        if not ck.add("server_port_open", wait_port("127.0.0.1", args.port, args.start_timeout, proc),
                      "port %d within %ds (rc=%s)" % (args.port, args.start_timeout, proc.poll())):
            return finish(proc, logf, log_path, ck, report, args.out)

        # /api/config
        st, cfg, _ = http("GET", base + "/api/config", timeout=30)
        ok = st == 200 and isinstance(cfg, dict) and all(k in cfg for k in ("cli", "roots", "presets", "mapping"))
        ck.add("api_config", ok, "status=%s keys=%s" % (st, sorted(cfg.keys()) if isinstance(cfg, dict) else cfg))
        cfg = cfg if isinstance(cfg, dict) else {}
        ck.add("api_config_presets", isinstance(cfg.get("presets"), list) and len(cfg["presets"]) >= 3,
               "presets=%d" % len(cfg.get("presets") or []))

        # /api/run tire1 7->12
        preset = pick_preset(cfg) or {}
        params = {"cal": 3, "type": "INSHOULDER", "patch": "15,15", "overlap": 0.25, "lower": 5, "upper": 95,
                  "stage": "AUTO", "roi": None}
        for k in ("cal", "type", "patch", "overlap", "lower", "upper", "stage", "roi"):
            if k in preset:
                params[k] = preset[k]
        params["dump"] = "all"
        body = {"input": tire1["raw"], "ref": tire1["ref"], "params": params}
        log("POST /api/run %s" % os.path.basename(tire1["raw"]))
        t0 = time.time()
        st, run, _ = http("POST", base + "/api/run", body, timeout=600)
        ok = st == 200 and isinstance(run, dict) and all(k in run for k in ("runId", "outDir", "stats", "files"))
        ck.add("api_run", ok, "status=%s %.1fs keys=%s" % (st, time.time() - t0,
                                                            sorted(run.keys()) if isinstance(run, dict) else str(run)[:200]))
        run = run if isinstance(run, dict) else {}
        files = run.get("files") if isinstance(run.get("files"), dict) else {}
        ck.add("api_run_files_result", bool(files.get("result")), "files.result=%s" % files.get("result"))
        stats = run.get("stats") if isinstance(run.get("stats"), dict) else {}
        ref = stats.get("ref") if isinstance(stats.get("ref"), dict) else None
        ck.add("api_run_stats_ref", ref is not None and "exact_pct" in ref and "black_mask_iou" in ref,
               "ref=%s" % (json.dumps({k: ref[k] for k in ("exact_pct", "within2_pct", "black_mask_iou") if k in ref})
                           if ref else None))
        report["run"] = {"runId": run.get("runId"), "outDir": run.get("outDir"), "ref": ref,
                         "params_effective": stats.get("params_effective")}
        run_id = run.get("runId")

        # static file
        if files.get("result"):
            url = files["result"] if files["result"].startswith("http") else base + files["result"]
            st, img, ctype = http("GET", url, timeout=120)
            ck.add("runs_static_result_png", st == 200 and isinstance(img, (bytes, bytearray)) and len(img) > 100,
                   "status=%s type=%s bytes=%s" % (st, ctype, len(img) if isinstance(img, (bytes, bytearray)) else "-"))

        # /api/pixel
        if run_id:
            q = urllib.parse.urlencode({"runId": run_id, "x": 4000, "y": 1200})
            st, px, _ = http("GET", base + "/api/pixel?" + q, timeout=60)
            ok = st == 200 and isinstance(px, dict) and all(k in px for k in ("raw", "basis", "diff", "result", "ref"))
            ck.add("api_pixel", ok, "status=%s %s" % (st, json.dumps(px, ensure_ascii=False)[:200] if isinstance(px, dict) else px))
            ck.add("api_pixel_values", isinstance(px, dict) and px.get("result") is not None and px.get("raw") is not None,
                   "raw=%s result=%s" % (px.get("raw") if isinstance(px, dict) else None,
                                         px.get("result") if isinstance(px, dict) else None))
            report["pixel"] = px if isinstance(px, dict) else str(px)[:300]
        else:
            ck.add("api_pixel", False, "no runId from /api/run")

        # /api/batch tire2
        if not args.skip_batch:
            body = {"folder": tire2_dir, "params": {"stage": "AUTO"}, "fovproc": fovproc}
            log("POST /api/batch %s (timeout %ds)" % (tire2_dir, args.batch_timeout))
            t0 = time.time()
            st, bt, _ = http("POST", base + "/api/batch", body, timeout=args.batch_timeout)
            ok = st == 200 and isinstance(bt, dict) and "batchId" in bt and isinstance(bt.get("items"), list)
            ck.add("api_batch", ok, "status=%s %.1fs keys=%s" % (st, time.time() - t0,
                                                                  sorted(bt.keys()) if isinstance(bt, dict) else str(bt)[:200]))
            items = bt.get("items") if isinstance(bt, dict) and isinstance(bt.get("items"), list) else []
            ck.add("api_batch_items_8", len(items) == 8, "items=%d" % len(items))
            ck.add("api_batch_item_keys", bool(items) and all(isinstance(it, dict) and
                                                               all(k in it for k in ("imgIdx", "name", "stats", "files"))
                                                               for it in items),
                   "first=%s" % (sorted(items[0].keys()) if items and isinstance(items[0], dict) else None))
            report["batch"] = {"batchId": bt.get("batchId") if isinstance(bt, dict) else None,
                               "items": [{"imgIdx": it.get("imgIdx"), "name": it.get("name"),
                                          "exact_pct": ((it.get("stats") or {}).get("ref") or {}).get("exact_pct")
                                          if isinstance(it.get("stats"), dict) else None}
                                         for it in items if isinstance(it, dict)]}
        return finish(proc, logf, log_path, ck, report, args.out)
    except Exception as e:  # noqa: BLE001
        ck.add("exception", False, repr(e))
        return finish(proc, logf, log_path, ck, report, args.out)


def finish(proc, logf, log_path, ck, report, out_dir):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
    logf.close()
    report["server_rc"] = proc.returncode
    report["ok"] = ck.all_ok()
    with open(os.path.join(out_dir, "ui_smoke.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    try:
        with open(log_path, "rb") as f:
            tail = f.read()[-1500:].decode("utf-8", errors="replace")
    except OSError:
        tail = ""
    log("server terminated (rc=%s). log tail:\n%s" % (proc.returncode, tail))
    log("RESULT %s (%d/%d checks passed) -> %s" % ("PASS" if report["ok"] else "FAIL",
                                                     sum(1 for c in ck.items if c["ok"]), len(ck.items),
                                                     os.path.join(out_dir, "ui_smoke.json")))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
