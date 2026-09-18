import numpy as np, os, sys, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simulate import load, remove_stage, scale16_ignore_null, patch_median

OUT = r"C:\Users\AIV\AppData\Local\Temp\claude\E--\03deaf05-d57c-4433-a88b-e1c14195f06c\scratchpad\depthnull"
res = {}
for raw_n, proc_n in [("7_SWU_Sh_R_D", "12_SWU_Sh_R_D_Proc"), ("17_SWD_Sh_L_D", "23_SWD_Sh_L_D_Proc")]:
    raw = load(raw_n)
    proc = load(proc_n).astype(np.uint8)
    src = remove_stage(raw)
    null = src == -999
    valid = ~null
    zmin, zmax = float(src[valid].min()), float(src[valid].max())
    unit_mm = (zmax - zmin) / 65536.0  # mm per scaled unit
    scaled = scale16_ignore_null(src, null)
    basis = patch_median(scaled, 15, 15, 0.25)
    diff = (scaled - basis).astype(np.float32)
    diff_mm = diff * unit_mm
    rows = np.where(valid.any(axis=1))[0]
    r0, r1 = int(rows.min()), int(rows.max())
    # low/high of site pipeline (percentile over all pixels incl. nulls) in mm
    allv = np.sort(np.clip(diff.astype(np.float64), -32768, 32767).astype(np.int16).astype(np.float32).ravel())
    def pct(v, p):
        pos = p / 100 * (v.size - 1); lo = int(np.floor(pos)); hi = int(np.ceil(pos)); f = pos - lo
        return float(v[lo]) + f * (float(v[hi]) - float(v[lo]))
    low_all, high_all = pct(allv, 5), pct(allv, 95)
    vv = np.sort(diff[valid].ravel())
    low_val, high_val = pct(vv, 5), pct(vv, 95)
    print(f"\n=== {raw_n}: band rows {r0}..{r1}, z range {zmin:.2f}..{zmax:.2f} mm  -> 1 unit = {unit_mm*1000:.4f} um, 1 mm = {1/unit_mm:.0f} units")
    print(f"  site clip (all px incl. null): low={low_all:.0f} ({low_all*unit_mm:.3f} mm) high={high_all:.0f} ({high_all*unit_mm:.3f} mm)")
    print(f"  valid-only clip:               low={low_val:.0f} ({low_val*unit_mm:.3f} mm) high={high_val:.0f} ({high_val*unit_mm:.3f} mm)")
    print(f"  fraction of pixels with diff==0 exactly: {100*(diff==0).mean():.1f}%  (nulls: {100*null.mean():.1f}%)")
    # per band-row bin: p5 / p50 / p95 of diff (mm), mean z, black% of site
    L = r1 - r0 + 1
    print("  bin  rows          meanZ(mm)  p5(mm)   p50(mm)  p95(mm)  site_black%  frac<site_low")
    bins = []
    for i in range(20):
        a, b = r0 + i * L // 20, r0 + (i + 1) * L // 20
        m = valid[a:b]
        d = diff_mm[a:b][m]
        z = src[a:b][m]
        blk = ((proc[a:b] == 0) & m).sum() / max(m.sum(), 1)
        frac_low = (diff[a:b][m] < low_all).mean()
        bins.append(dict(rows=[a, b - 1], meanZ=round(float(z.mean()), 3), p5=round(float(np.percentile(d, 5)), 3),
                         p50=round(float(np.percentile(d, 50)), 3), p95=round(float(np.percentile(d, 95)), 3),
                         site_black=round(100 * blk, 1), frac_below_low=round(100 * frac_low, 1)))
        print(f"  {i:3}  {a:5}-{b-1:5}  {z.mean():8.3f}  {np.percentile(d,5):7.3f}  {np.percentile(d,50):7.3f}  {np.percentile(d,95):7.3f}  {100*blk:8.1f}   {100*frac_low:6.1f}")
    res[raw_n] = dict(z_range=[zmin, zmax], unit_um=unit_mm * 1000, site_low_mm=low_all * unit_mm, site_high_mm=high_all * unit_mm,
                      valid_low_mm=low_val * unit_mm, valid_high_mm=high_val * unit_mm, zero_frac=float((diff == 0).mean()), bins=bins)
json.dump(res, open(os.path.join(OUT, "physical.json"), "w"), indent=1)
