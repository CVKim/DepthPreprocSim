import numpy as np, os, sys, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import guide_sim as g

OUT = os.path.dirname(os.path.abspath(__file__))
mats = g.load_profiles(3, 2)          # shoulder R: three 10x1940 scans (rows = 10 profiles, cols = x)
p = g.PARAMS[2]
xs = p["fov_x_start"] + np.arange(mats[0].shape[1]) * p["x_res"]

print("=== scan 0 (origin), columns x in [-16, 2]: per-column valid count / min / median / max across 10 rows ===")
sel = (xs >= -16) & (xs <= 2)
for i in np.where(sel)[0][::5]:
    col = mats[0][:, i]
    v = col[g.valid(col)]
    if v.size:
        print(f"x={xs[i]:6.1f} n={v.size:2d} min={v.min():7.2f} med={np.median(v):7.2f} max={v.max():7.2f} spread={v.max()-v.min():6.2f}")
    else:
        print(f"x={xs[i]:6.1f} n=0")

print("\n=== per-row minimum in x∈[-16,0] for each of the 10 rows (is the -20 dip in one row or all?) ===")
for s, mat in enumerate(mats):
    m = (xs >= -16) & (xs <= 0)
    rows = []
    for r in range(mat.shape[0]):
        v = mat[r, m]; ok = g.valid(v)
        rows.append(f"{v[ok].min():6.1f}" if ok.any() else "  none")
    print(f"scan{s}: " + " ".join(rows))

print("\n=== alternative feature profiles (scan 0) -> y_min / x_at / shift_z (before clip) / machine move ===")
def eval_profile(feat, label):
    r, *_ = g.run(3, 2, prof_override=feat, tag=label)
    print(f"{label:34} y_min={r['feature_z']:7.2f} at x={r['feature_x']:6.1f}  shift_z(unclipped)={r['shift_z_unclipped']:7.2f} -> clipped {r['shift_z']:6.2f}  mach=({r['mach_x']:+.3f}, {r['mach_z']:+.3f})  fit_applied={r['fit'] and r['fit']['applied']}")
    return r
res = {}
res["site k=0.1 (min of 10)"] = eval_profile(g.extract_bottom(mats[0], 0.1), "site: bottom k=0.1 (min of 10)")
res["bottom k=0.3"] = eval_profile(g.extract_bottom(mats[0], 0.3), "bottom k=0.3 (mean of lowest 3)")
res["bottom k=0.5"] = eval_profile(g.extract_bottom(mats[0], 0.5), "bottom k=0.5 (mean of lowest 5)")
res["mean"] = eval_profile(g.extract_mean(mats[0]), "mean profile (as Jacobian uses)")
med = np.full(mats[0].shape[1], np.nan)
for i in range(mats[0].shape[1]):
    c = mats[0][:, i]; c = c[g.valid(c)]
    if c.size: med[i] = np.median(c)
res["median"] = eval_profile(med, "median of 10 rows")

print("\n=== scan-to-scan consistency of the feature (bottom k=0.1 on each scan, no shift compensation) ===")
for s in range(3):
    f = g.extract_bottom(mats[s], 0.1)
    clean = g.remove_small_chunks(f, 30)
    sm = g.moving_average(g.median_filter(clean, 31), 15)
    inr = (xs >= -97) & (xs <= 60) & ~np.isnan(sm)
    idx = np.where(inr)[0]; i = idx[np.argmin(sm[idx])]
    print(f"scan{s}: y_min={sm[i]:7.2f} at x={xs[i]:6.1f}")

# where do valid pixels end (wall near x=0)? count of valid rows per column around the wall
print("\n=== valid-row count per column near the wall x in [-4, 4] (scan 0) ===")
for i in np.where((xs >= -4) & (xs <= 4))[0][::4]:
    col = mats[0][:, i]; print(f"x={xs[i]:5.1f} n_valid={g.valid(col).sum()} vals={np.round(col[g.valid(col)],1).tolist()}")

# save the profiles for plotting
np.save(os.path.join(OUT, "spike_profiles.npy"), np.stack([xs, g.extract_bottom(mats[0],0.1), g.extract_mean(mats[0]), med]))
json.dump({k: {kk: vv for kk, vv in v.items() if kk not in ('J','J_inv')} for k, v in res.items()}, open(os.path.join(OUT, "spike.json"), "w"), indent=1, default=float)
