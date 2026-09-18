import numpy as np, tifffile, cv2, os, sys, time, json
from numpy.lib.stride_tricks import sliding_window_view
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simulate import load, remove_stage, scale16_ignore_null, percentile_cpp, patch_median
OUT = r"C:\Users\AIV\AppData\Local\Temp\claude\E--\03deaf05-d57c-4433-a88b-e1c14195f06c\scratchpad\depthnull"


def patch_median_masked(data, null_mask, ph, pw, overlap):
    """median over valid pixels only; empty patches -> filled by nearest valid grid cell"""
    h, w = data.shape
    sh = max(1, int(ph * (1 - overlap)))
    sw = max(1, int(pw * (1 - overlap)))
    oh = (h - ph) // sh + 1
    ow = (w - pw) // sw + 1
    d = data.copy()
    d[null_mask] = np.nan
    win = sliding_window_view(d, (ph, pw))[::sh, ::sw][:oh, :ow].reshape(oh, ow, -1)
    with np.errstate(all="ignore"):
        med = np.nanmedian(win, axis=2).astype(np.float32)
    nanm = np.isnan(med)
    if nanm.any():
        _, idx = cv2.distanceTransformWithLabels(nanm.astype(np.uint8), cv2.DIST_L2, 3, labelType=cv2.DIST_LABEL_PIXEL)
        vr, vc = np.where(~nanm)
        lab_of_valid = idx[vr, vc]
        lut_r = np.zeros(int(idx.max()) + 1, np.int64)
        lut_c = np.zeros(int(idx.max()) + 1, np.int64)
        lut_r[lab_of_valid] = vr
        lut_c[lab_of_valid] = vc
        med = med.copy()
        med[nanm] = med[lut_r[idx[nanm]], lut_c[idx[nanm]]]
    return cv2.resize(med, (w, h), interpolation=cv2.INTER_LINEAR)


def clip_norm(diff, null_mask, lower, upper, valid_only):
    src = diff[~null_mask] if valid_only else diff
    vals = np.sort(src.ravel())

    def pct(p):
        pos = p / 100 * (vals.size - 1)
        lo = int(np.floor(pos))
        hi = int(np.ceil(pos))
        f = pos - lo
        return float(vals[lo]) + f * (float(vals[hi]) - float(vals[lo]))

    low = max(pct(lower), -1000)
    high = min(pct(upper), 1000)
    c = np.clip(diff, low, high)
    mn, mx = float(c.min()), float(c.max())
    if mx > mn:
        out = np.clip(np.rint((c - mn) / (mx - mn) * 255), 0, 255).astype(np.uint8)
    else:
        out = np.zeros_like(c, np.uint8)
    return out, low, high


def fill_small_holes(data, null_mask, max_area):
    """fill enclosed null holes <= max_area px with Telea inpainting on a local window"""
    n, lab, stats, _ = cv2.connectedComponentsWithStats(null_mask.astype(np.uint8), connectivity=8)
    small = np.zeros(n, bool)
    small[1:] = stats[1:, cv2.CC_STAT_AREA] <= max_area
    fillm = small[lab]
    out = data.copy()
    for k in np.where(small)[0]:
        x, y, w, h, a = stats[k]
        x0, y0 = max(0, x - 8), max(0, y - 8)
        x1, y1 = min(data.shape[1], x + w + 8), min(data.shape[0], y + h + 8)
        sub = out[y0:y1, x0:x1]
        m = (lab[y0:y1, x0:x1] == k)
        valid_sub = sub > -900
        if valid_sub.sum() == 0:
            continue
        lo, hi = float(sub[valid_sub].min()), float(sub[valid_sub].max())
        sub8 = np.zeros(sub.shape, np.uint8)
        if hi > lo:
            sub8[valid_sub] = np.clip((sub[valid_sub] - lo) / (hi - lo) * 255, 0, 255)
        rep = cv2.inpaint(sub8, ((~valid_sub) | m).astype(np.uint8), 3, cv2.INPAINT_TELEA)
        if hi > lo:
            fill_vals = lo + rep.astype(np.float32) / 255 * (hi - lo)
        else:
            fill_vals = np.full(sub.shape, lo, np.float32)
        sub[m] = fill_vals[m]
    return out, fillm


def run(raw_n, proc_n):
    raw = load(raw_n)
    proc = load(proc_n).astype(np.uint8)
    src = remove_stage(raw)
    null0 = src == -999
    valid_band = ~null0
    results = {}
    h, w = src.shape
    rows = np.where(valid_band.any(axis=1))[0]
    r0, r1 = int(rows.min()), int(rows.max())
    ytop = max(0, r0 - 60)
    ybot = min(h - 341, r1 - 280)
    ymid = (r0 + r1) // 2 - 170
    wins = {"top_x0": (ytop, 0), "top_x4000": (ytop, 4000), "mid_x0": (ymid, 0), "bot_x0": (ybot, 0)}

    def metrics(name, out, nm):
        band = valid_band
        blk = (out == 0) & band
        wht = (out == 255) & band
        bh = blk[r0:r1 + 1].sum(axis=1) / np.maximum(band[r0:r1 + 1].sum(axis=1), 1)
        L = len(bh)
        prof = [round(100 * bh[i * L // 20:(i + 1) * L // 20].mean(), 1) for i in range(20)]
        results[name] = dict(black_in_band_pct=round(100 * blk.sum() / band.sum(), 2),
                             white_in_band_pct=round(100 * wht.sum() / band.sum(), 2),
                             black_rows_over30=int((bh > 0.3).sum()), black_profile=prof, max_black_bin=max(prof))
        for wn, (y0, x0) in wins.items():
            cv2.imwrite(os.path.join(OUT, f"var_{proc_n}_{name}_{wn}.png"), out[y0:y0 + 341, x0:x0 + 671])
        cv2.imwrite(os.path.join(OUT, f"var_{proc_n}_{name}_full.png"),
                    cv2.resize(out, (w // 8, h // 8), interpolation=cv2.INTER_AREA))

    metrics("SITE", proc, null0)

    scaled = scale16_ignore_null(src, null0)
    basis = patch_median(scaled, 15, 15, 0.25)
    diff = np.clip((scaled - basis).astype(np.float64), -32768, 32767).astype(np.int16).astype(np.float32)
    out, lo, hi = clip_norm(diff, null0, 5, 95, valid_only=False)
    out[null0] = 0
    metrics("V0_sim_current", out, null0)
    results["V0_sim_current"]["low_high"] = [lo, hi]

    out, lo, hi = clip_norm(diff, null0, 5, 95, valid_only=True)
    out[null0] = 0
    metrics("V1_validpct", out, null0)
    results["V1_validpct"]["low_high"] = [lo, hi]

    basis_m = patch_median_masked(scaled, null0, 15, 15, 0.25)
    diff_m = np.clip((scaled - basis_m).astype(np.float64), -32768, 32767).astype(np.int16).astype(np.float32)
    out, lo, hi = clip_norm(diff_m, null0, 5, 95, valid_only=True)
    out[null0] = 0
    metrics("V2_maskmed_validpct", out, null0)
    results["V2_maskmed_validpct"]["low_high"] = [lo, hi]

    filled, fillm = fill_small_holes(src, null0, 5000)
    null3 = filled == -999
    scaled3 = scale16_ignore_null(filled, null3)
    basis3 = patch_median_masked(scaled3, null3, 15, 15, 0.25)
    diff3 = np.clip((scaled3 - basis3).astype(np.float64), -32768, 32767).astype(np.int16).astype(np.float32)
    out, lo, hi = clip_norm(diff3, null3, 5, 95, valid_only=True)
    out[null3] = 0
    metrics("V3_holefill_maskmed_validpct", out, null3)
    results["V3_holefill_maskmed_validpct"].update(low_high=[lo, hi], holes_filled_px=int(fillm.sum()))

    out, lo, hi = clip_norm(diff3, null3, 2, 98, valid_only=True)
    out[null3] = 0
    metrics("V4_V3_pct2_98", out, null3)
    results["V4_V3_pct2_98"]["low_high"] = [lo, hi]

    out, lo, hi = clip_norm(diff3, null3, 5, 95, valid_only=True)
    mid = int(np.clip(np.rint((0 - lo) / (hi - lo) * 255), 0, 255))
    out[null3] = mid
    metrics("V5_V3_nullmid", out, null3)
    results["V5_V3_nullmid"].update(low_high=[lo, hi], null_gray=mid)

    basis6 = patch_median_masked(scaled3, null3, 15, 45, 0.25)
    diff6 = np.clip((scaled3 - basis6).astype(np.float64), -32768, 32767).astype(np.int16).astype(np.float32)
    out, lo, hi = clip_norm(diff6, null3, 5, 95, valid_only=True)
    out[null3] = 0
    metrics("V6_V3_patch45x15", out, null3)
    results["V6_V3_patch45x15"]["low_high"] = [lo, hi]

    raw8 = np.zeros((h, w), np.uint8)
    v = ~null0
    p1, p99 = np.percentile(src[v], [1, 99])
    raw8[v] = np.clip((src[v] - p1) / (p99 - p1) * 255, 0, 255)
    rawc = cv2.cvtColor(raw8, cv2.COLOR_GRAY2BGR)
    rawc[null0] = (0, 0, 255)
    for wn, (y0, x0) in wins.items():
        cv2.imwrite(os.path.join(OUT, f"var_{proc_n}_RAW_{wn}.png"), rawc[y0:y0 + 341, x0:x0 + 671])

    zrow = np.array([src[r][~null0[r]].mean() if (~null0[r]).any() else np.nan for r in range(h)])
    results["_band_rows"] = [r0, r1]
    results["_zrow_every20"] = [None if np.isnan(z) else round(float(z), 3) for z in zrow[::20]]
    json.dump(results, open(os.path.join(OUT, f"variants_{proc_n}.json"), "w"), indent=1, ensure_ascii=False)
    for k, v in results.items():
        if not k.startswith("_"):
            print(k, {kk: vv for kk, vv in v.items() if kk != "black_profile"})
            print("   black profile:", v["black_profile"])


if __name__ == "__main__":
    for a, b in [("7_SWU_Sh_R_D", "12_SWU_Sh_R_D_Proc"), ("17_SWD_Sh_L_D", "23_SWD_Sh_L_D_Proc")]:
        print(f"\n######## {a} -> {b}")
        t = time.time()
        run(a, b)
        print(f"({time.time() - t:.0f}s)")
