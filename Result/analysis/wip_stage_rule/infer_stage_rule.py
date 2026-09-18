"""Infer which stage-removal rule the site (WIP) DLL used, by testing candidate valid-mask rules
against the site _Proc output. Uses the python re-implementation (bit-compatible with depth_sim.exe
at 99.99%). Writes report to this folder."""
import os, sys, json, time
import numpy as np, cv2, tifffile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "근거자료"))
from simulate import scale16_ignore_null, patch_median  # noqa: E402

T1 = r"H:\000. PJT\01. HankookTire\03_이미지\PC3\1639390184_2000026091709423060"
T2 = r"H:\000. PJT\01. HankookTire\03_이미지\PC3\2000026091715281105"
CASES = [(T1, "7_SWU_Sh_R_D", "12_SWU_Sh_R_D_Proc", (15, 15), 0.25),
         (T1, "5_SWU_Sh_L_D", "11_SWU_Sh_L_D_Proc", (15, 15), 0.25),
         (T1, "19_SWD_Sh_R_D", "24_SWD_Sh_R_D_Proc", (15, 15), 0.25),
         (T2, "1_SWU_Cen_D", "9_SWU_Cen_D_Proc", (50, 50), 0.1),
         (T2, "7_SWU_Sh_R_D", "12_SWU_Sh_R_D_Proc", (15, 15), 0.25)]


def load(folder, n):
    return tifffile.imread(os.path.join(folder, n + ".mim")).astype(np.float32)


def pipeline(src, null_mask, patch, overlap):
    scaled = scale16_ignore_null(src, null_mask)
    basis = patch_median(scaled, patch[1], patch[0], overlap)
    diff = np.clip((scaled - basis).astype(np.float64), -32768, 32767).astype(np.int16).astype(np.float32)
    vals = np.sort(diff.ravel())
    def pct(p):
        pos = p / 100 * (vals.size - 1); lo = int(np.floor(pos)); hi = int(np.ceil(pos)); f = pos - lo
        return float(vals[lo]) + f * (float(vals[hi]) - float(vals[lo]))
    low, high = max(pct(5), -1000), min(pct(95), 1000)
    c = np.clip(diff, low, high); mn, mx = float(c.min()), float(c.max())
    out = np.clip(np.rint((c - mn) / (mx - mn) * 255), 0, 255).astype(np.uint8) if mx > mn else np.zeros_like(c, np.uint8)
    out[null_mask] = 0
    return out


def score(out, ref):
    d = np.abs(out.astype(int) - ref.astype(int))
    pz, rz = out == 0, ref == 0
    return dict(exact=round(100 * (d == 0).mean(), 3), within2=round(100 * (d <= 2).mean(), 3),
                iou=round(float((pz & rz).sum() / max((pz | rz).sum(), 1)), 4),
                ref_black_only=int((rz & ~pz).sum()), out_black_only=int((pz & ~rz).sum()))


def rules(raw):
    """return dict name -> null_mask (True = treated as -999)"""
    null0 = raw <= -900
    valid = ~null0
    n, lab, stats, cent = cv2.connectedComponentsWithStats(valid.astype(np.uint8), connectivity=8)
    areas = stats[1:, cv2.CC_STAT_AREA]
    big = 1 + int(np.argmax(areas))
    band = lab == big
    x, y, w, h, a = stats[big]
    out = {}
    out["R0_none"] = null0
    out["R1_largest_only(source)"] = ~band
    # R2: keep components with area >= N
    for N in (50, 500, 5000, 50000, 200000):
        keep = np.zeros(n, bool); keep[1:] = areas >= N
        out[f"R2_area>={N}"] = ~keep[lab]
    # R3: keep components whose bbox intersects the largest component's row range
    keep = np.zeros(n, bool)
    for k in range(1, n):
        ky, kh = stats[k, cv2.CC_STAT_TOP], stats[k, cv2.CC_STAT_HEIGHT]
        keep[k] = (ky < y + h) and (ky + kh > y)
    out["R3_bbox_rows_intersect_largest"] = ~keep[lab]
    # R4: keep everything valid inside the largest component's bounding rect
    m = np.zeros_like(valid); m[y:y + h, x:x + w] = True
    out["R4_valid_in_largest_bbox"] = ~(valid & m)
    # R5: row cut at largest bbox top/bottom (remove rows outside [y, y+h)) but keep all valid inside
    out["R5_rowcut_largest_bbox"] = ~(valid & m)  # same as R4 since bbox spans full width
    # R6: keep largest + any component whose centroid row lies within largest row range
    keep = np.zeros(n, bool); keep[big] = True
    for k in range(1, n):
        cy = cent[k, 1]
        keep[k] = keep[k] or (y <= cy < y + h)
    out["R6_centroid_in_largest_rows"] = ~keep[lab]
    # R7: 4-connectivity largest only
    n4, lab4, st4, _ = cv2.connectedComponentsWithStats(valid.astype(np.uint8), connectivity=4)
    big4 = 1 + int(np.argmax(st4[1:, cv2.CC_STAT_AREA]))
    out["R7_largest_only_conn4"] = lab4 != big4
    # R8: remove components touching top or bottom image border only
    keep = np.ones(n, bool); keep[0] = False
    H = raw.shape[0]
    for k in range(1, n):
        ky, kh = stats[k, cv2.CC_STAT_TOP], stats[k, cv2.CC_STAT_HEIGHT]
        if ky == 0 or ky + kh == H:
            keep[k] = False
    keep[big] = True
    out["R8_drop_border_touching"] = ~keep[lab]
    return out, dict(components=int(n - 1), largest_area=int(a), largest_bbox=[int(x), int(y), int(w), int(h)])


report = {}
for folder, raw_n, ref_n, patch, ov in CASES:
    t0 = time.time()
    raw = load(folder, raw_n); ref = load(folder, ref_n).astype(np.uint8)
    masks, info = rules(raw)
    key = f"{os.path.basename(folder)[:10]}:{raw_n}->{ref_n}"
    report[key] = dict(info=info, rules={})
    print(f"\n=== {key}  comps={info['components']} largest_bbox={info['largest_bbox']}")
    # where do site-valid (ref!=0) pixels lie that the source rule removed?  (B set)
    B = (ref != 0) & masks["R1_largest_only(source)"] & ~masks["R0_none"]
    if B.any():
        rows = np.where(B.any(axis=1))[0]
        nB, labB, stB, _ = cv2.connectedComponentsWithStats(B.astype(np.uint8), connectivity=8)
        ar = stB[1:, cv2.CC_STAT_AREA]; order = np.argsort(-ar)[:5]
        print(f"  site-kept-but-source-removed px={int(B.sum())} rows {rows.min()}..{rows.max()} comps={nB-1} top areas={ar[order].tolist()}")
        for k in order:
            bx, by, bw, bh, ba = stB[k + 1]; print(f"     comp area={ba} bbox x={bx} y={by} w={bw} h={bh}")
        report[key]["site_kept_source_removed"] = dict(px=int(B.sum()), rows=[int(rows.min()), int(rows.max())], comps=int(nB - 1), top_areas=ar[order].tolist())
    for name, nm in masks.items():
        out = pipeline(raw, nm, patch, ov)
        s = score(out, ref); s["nulled_px"] = int(nm.sum())
        report[key]["rules"][name] = s
        print(f"  {name:34} exact={s['exact']:7.3f}  within2={s['within2']:7.3f}  IoU={s['iou']:.4f}  refBlackOnly={s['ref_black_only']:8d}  outBlackOnly={s['out_black_only']:8d}")
    print(f"  ({time.time()-t0:.0f}s)")
json.dump(report, open(os.path.join(HERE, "infer_stage_rule.json"), "w"), indent=1, ensure_ascii=False)
print("DONE")
