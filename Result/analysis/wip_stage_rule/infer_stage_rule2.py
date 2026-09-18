"""Round 2: row-cut, dilation and per-column run rules for the site (WIP) stage removal."""
import os, sys, json, time
import numpy as np, cv2, tifffile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from infer_stage_rule import load, pipeline, score, T1, T2  # noqa: E402

CASES = [(T1, "7_SWU_Sh_R_D", "12_SWU_Sh_R_D_Proc", (15, 15), 0.25),
         (T1, "5_SWU_Sh_L_D", "11_SWU_Sh_L_D_Proc", (15, 15), 0.25),
         (T1, "19_SWD_Sh_R_D", "24_SWD_Sh_R_D_Proc", (15, 15), 0.25),
         (T2, "7_SWU_Sh_R_D", "12_SWU_Sh_R_D_Proc", (15, 15), 0.25),
         (T2, "5_SWU_Sh_L_D", "11_SWU_Sh_L_D_Proc", (15, 15), 0.25)]


def runs_per_column(valid):
    """label contiguous valid runs per column; return run lengths per pixel and run ids"""
    h, w = valid.shape
    v = valid.astype(np.int8)
    # run start where valid and (row==0 or above invalid)
    start = v.copy(); start[1:] &= (1 - v[:-1])
    run_id = np.cumsum(start, axis=0) * v  # 0 for invalid
    return run_id


def rules2(raw):
    null0 = raw <= -900
    valid = ~null0
    h, w = raw.shape
    n, lab, stats, cent = cv2.connectedComponentsWithStats(valid.astype(np.uint8), connectivity=8)
    areas = stats[1:, cv2.CC_STAT_AREA]
    big = 1 + int(np.argmax(areas))
    band = lab == big
    bx, by, bw, bh, ba = stats[big]
    out = {}
    others = [k for k in range(1, n) if k != big]
    # stage side: where is most of the non-largest valid area?
    top_area = sum(stats[k, cv2.CC_STAT_AREA] for k in others if cent[k, 1] < by + bh / 2)
    bot_area = sum(stats[k, cv2.CC_STAT_AREA] for k in others if cent[k, 1] >= by + bh / 2)
    stage_top = top_area >= bot_area
    # R9a: row cut at extreme row of all non-largest comps on the stage side
    if stage_top:
        cut = max([stats[k, cv2.CC_STAT_TOP] + stats[k, cv2.CC_STAT_HEIGHT] for k in others if cent[k, 1] < by + bh / 2] or [0])
        m = np.zeros_like(valid); m[cut:] = True
    else:
        cut = min([stats[k, cv2.CC_STAT_TOP] for k in others if cent[k, 1] >= by + bh / 2] or [h])
        m = np.zeros_like(valid); m[:cut] = True
    out[f"R9a_rowcut_all_comps(cut={cut},{'top' if stage_top else 'bot'})"] = ~(valid & m)
    # R9b: same but only comps >= 1000 px
    if stage_top:
        cut = max([stats[k, cv2.CC_STAT_TOP] + stats[k, cv2.CC_STAT_HEIGHT] for k in others if cent[k, 1] < by + bh / 2 and stats[k, cv2.CC_STAT_AREA] >= 1000] or [0])
        m = np.zeros_like(valid); m[cut:] = True
    else:
        cut = min([stats[k, cv2.CC_STAT_TOP] for k in others if cent[k, 1] >= by + bh / 2 and stats[k, cv2.CC_STAT_AREA] >= 1000] or [h])
        m = np.zeros_like(valid); m[:cut] = True
    out[f"R9b_rowcut_comps>=1000(cut={cut})"] = ~(valid & m)
    # R10: keep valid pixels within vertical dilation D of the largest component
    for D in (5, 20, 60):
        k = cv2.getStructuringElement(cv2.MORPH_RECT, (1, 2 * D + 1))
        dil = cv2.dilate(band.astype(np.uint8), k) > 0
        out[f"R10_dilate_largest_D={D}"] = ~(valid & dil)
    # R11: per-column keep only the longest valid run
    rid = runs_per_column(valid)
    keep = np.zeros_like(valid)
    for x in range(w):
        col = rid[:, x]
        if not col.any():
            continue
        ids, counts = np.unique(col[col > 0], return_counts=True)
        best = ids[np.argmax(counts)]
        keep[:, x] = col == best
    out["R11_per_column_longest_run"] = ~keep
    # R12: per-column remove runs shorter than N
    for N in (30, 100, 300):
        keep = np.zeros_like(valid)
        for x in range(w):
            col = rid[:, x]
            if not col.any():
                continue
            ids, counts = np.unique(col[col > 0], return_counts=True)
            good = ids[counts >= N]
            keep[:, x] = np.isin(col, good)
        out[f"R12_per_column_runs>={N}"] = ~keep
    # R13: per-column keep runs that intersect the largest CC
    keep = np.zeros_like(valid)
    for x in range(w):
        col = rid[:, x]
        if not col.any():
            continue
        ids_in_band = np.unique(col[band[:, x] & (col > 0)])
        keep[:, x] = np.isin(col, ids_in_band)
    out["R13_per_column_runs_touching_largest"] = ~keep
    # R14: per-column longest run, but computed on a median-filtered validity? skip. R14: per-row longest run
    ridr = runs_per_column(valid.T).T
    keep = np.zeros_like(valid)
    for y in range(h):
        row = ridr[y]
        if not row.any():
            continue
        ids, counts = np.unique(row[row > 0], return_counts=True)
        keep[y] = row == ids[np.argmax(counts)]
    out["R14_per_row_longest_run"] = ~keep
    return out, dict(stage_top=bool(stage_top), largest_bbox=[int(bx), int(by), int(bw), int(bh)])


report = {}
for folder, raw_n, ref_n, patch, ov in CASES:
    t0 = time.time()
    raw = load(folder, raw_n); ref = load(folder, ref_n).astype(np.uint8)
    masks, info = rules2(raw)
    key = f"{os.path.basename(folder)[:10]}:{raw_n}->{ref_n}"
    report[key] = dict(info=info, rules={})
    print(f"\n=== {key} {info}")
    for name, nm in masks.items():
        out = pipeline(raw, nm, patch, ov)
        s = score(out, ref); s["nulled_px"] = int(nm.sum())
        report[key]["rules"][name] = s
        print(f"  {name:44} exact={s['exact']:7.3f}  within2={s['within2']:7.3f}  IoU={s['iou']:.4f}  refBlackOnly={s['ref_black_only']:8d}  outBlackOnly={s['out_black_only']:8d}")
    print(f"  ({time.time()-t0:.0f}s)")
json.dump(report, open(os.path.join(HERE, "infer_stage_rule2.json"), "w"), indent=1, ensure_ascii=False)
print("DONE")
