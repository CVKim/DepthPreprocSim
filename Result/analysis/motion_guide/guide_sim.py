# Python re-implementation of talos-vision MotionGuideAlg (shoulder modes) for offline diagnosis.
# Source: E:\talos-vision\src\Algorithm\alg_body\src\MotionGuideAlg.cpp (branch feat/ALG-264-AutoTeaching)
import numpy as np, tifffile, os, sys, json, cv2

M = "H:/000. PJT/01. HankookTire/03_이미지/MotionGuide/0917"
OUT = os.path.dirname(os.path.abspath(__file__))

PARAMS = {
    2: dict(mode=2, x_res=0.1, max_shift=150, dx2=-1.0, dz2=0.0, dx3=-1.0, dz3=-1.0, ratio_k=0.1, med=31,
            fov_x_start=-97.0, safe_z_min=-35.0, safe_z_max=-35.0, safe_x_min=-97.0, safe_x_max=97.0,
            search_x_min=-97.0, search_x_max=60.0),
    1: dict(mode=1, x_res=0.1, max_shift=150, dx2=-1.0, dz2=0.0, dx3=-1.0, dz3=-1.0, ratio_k=0.1, med=31,
            fov_x_start=-97.0, safe_z_min=25.0, safe_z_max=25.0, safe_x_min=-97.0, safe_x_max=97.0,
            search_x_min=-50.0, search_x_max=50.0),
}


def load_profiles(folder_idx, mode):
    mats = []
    for i in range(3):
        a = tifffile.imread(os.path.join(M, str(folder_idx), f"TireInput_{i}_{mode}.mim")).astype(np.float32)
        if a.shape[0] > a.shape[1]:
            a = cv2.rotate(a, cv2.ROTATE_90_CLOCKWISE)
        mats.append(a)
    return mats


def valid(v):
    return ~np.isnan(v) & (v > -90.0)


def extract_mean(mat):
    m = valid(mat)
    cnt = m.sum(axis=0)
    s = np.where(m, mat, 0).sum(axis=0)
    out = np.full(mat.shape[1], np.nan)
    out[cnt > 0] = s[cnt > 0] / cnt[cnt > 0]
    return out


def extract_bottom(mat, k_ratio):
    out = np.full(mat.shape[1], np.nan)
    for i in range(mat.shape[1]):
        col = mat[:, i]
        col = col[valid(col)]
        if col.size:
            col = np.sort(col)
            k = max(1, int(np.ceil(col.size * k_ratio)))
            out[i] = col[:k].mean()
    return out


def extract_top(mat, k_ratio):
    out = np.full(mat.shape[1], np.nan)
    for i in range(mat.shape[1]):
        col = mat[:, i]
        col = col[valid(col)]
        if col.size:
            col = np.sort(col)
            k = max(1, int(np.ceil(col.size * k_ratio)))
            out[i] = col[-k:].mean()
    return out


def remove_small_chunks(d, min_size):
    res = d.copy()
    n = len(d)
    start = -1
    for i in range(n + 1):
        ok = i < n and not np.isnan(d[i]) and d[i] > -90.0
        if ok:
            if start == -1:
                start = i
        else:
            if start != -1:
                if i - start < min_size:
                    res[start:i] = np.nan
                start = -1
    return res


def median_filter(d, size):
    res = np.full_like(d, np.nan)
    pad = size // 2
    n = len(d)
    for i in range(n):
        if np.isnan(d[i]):
            continue
        w = d[max(0, i - pad):min(n, i + pad + 1)]
        w = np.sort(w[~np.isnan(w)])
        if w.size:
            res[i] = w[w.size // 2]
    return res


def moving_average(d, size):
    res = np.full_like(d, np.nan)
    pad = size // 2
    n = len(d)
    for i in range(n):
        if np.isnan(d[i]):
            continue
        w = d[max(0, i - pad):min(n, i + pad + 1)]
        w = w[~np.isnan(w)]
        if w.size:
            res[i] = w.mean()
    return res


def find_optimal_shift(base, target, max_shift, x_res):
    n = len(base)
    best = (np.inf, 0, 0.0)
    for sx in range(-max_shift, max_shift + 1):
        s = max(0, -sx)
        e = min(n, n - sx)
        b = base[s:e]
        t = target[s + sx:e + sx]
        m = ~np.isnan(b) & ~np.isnan(t)
        if m.sum() < 50:
            continue
        diffs = np.sort(t[m] - b[m])
        k = diffs.size
        sz = (diffs[k // 2 - 1] + diffs[k // 2]) / 2 if k % 2 == 0 else diffs[k // 2]
        mae = np.abs((t[m] - sz) - b[m]).mean()
        if mae < best[0]:
            best = (mae, sx, sz)
    return best[1] * x_res, best[2], best[0]


def minimal_shift(v, lo, hi):
    if v < lo:
        return lo - v
    if v > hi:
        return hi - v
    return 0.0


def fit_parabola_robust(x, y, max_iter=3, threshold=1.5):
    x = np.asarray(x); y = np.asarray(y)
    inl = np.ones(len(x), bool)
    a = b = c = 0.0
    for it in range(max_iter):
        if inl.sum() < 3:
            return None
        A = np.stack([x[inl] ** 2, x[inl], np.ones(inl.sum())], 1)
        coef, *_ = np.linalg.lstsq(A, y[inl], rcond=None)
        a, b, c = coef
        if it == max_iter - 1:
            break
        inl = np.abs(y - (a * x * x + b * x + c)) <= threshold
    return a, b, c


def run(folder_idx, mode, prof_override=None, tag=""):
    p = PARAMS[mode]
    mats = load_profiles(folder_idx, mode)
    xs = p["fov_x_start"] + np.arange(mats[0].shape[1]) * p["x_res"]
    mean_profs, raw_profs = [], []
    for mat in mats:
        mp = extract_mean(mat)
        raw_profs.append(mp.copy())
        mp[(xs < p["search_x_min"]) | (xs > p["search_x_max"])] = np.nan
        mp = remove_small_chunks(mp, 30)
        mp = median_filter(mp, p["med"])
        mp = moving_average(mp, max(3, p["med"] // 2))
        mean_profs.append(mp)
    dx2, dz2, mae2 = find_optimal_shift(mean_profs[0], mean_profs[1], p["max_shift"], p["x_res"])
    dx3, dz3, mae3 = find_optimal_shift(mean_profs[0], mean_profs[2], p["max_shift"], p["x_res"])
    dM = np.array([[p["dx2"], p["dx3"]], [p["dz2"], p["dz3"]]])
    dP = np.array([[dx2, dx3], [dz2, dz3]])
    J = dP @ np.linalg.inv(dM)
    J_inv = np.linalg.inv(J)
    res = dict(folder=folder_idx, mode=mode, tag=tag, dx2=dx2, dz2=dz2, mae2=mae2, dx3=dx3, dz3=dz3, mae3=mae3,
               J=J.tolist(), J_inv=J_inv.tolist())
    if mode == 2:
        feat = extract_bottom(mats[0], p["ratio_k"]) if prof_override is None else prof_override
        clean = remove_small_chunks(feat, 30)
        sm = moving_average(median_filter(clean, p["med"]), max(3, p["med"] // 2))
        inr = (xs >= p["search_x_min"]) & (xs <= p["search_x_max"]) & ~np.isnan(sm) & (sm < 90.0)
        idx = np.where(inr)[0]
        i_min = idx[np.argmin(sm[idx])]
        y_min = sm[i_min]; x_at = xs[i_min]
        res.update(raw_y_min=float(y_min), raw_x_at_ymin=float(x_at))
        roi = idx[sm[idx] <= y_min + 15.0]
        fit = fit_parabola_robust(xs[roi], sm[roi]) if roi.size >= 15 else None
        res["fit"] = None
        if fit is not None:
            a, b, c = fit
            vx = -b / (2 * a) if a > 0.0001 else None
            vy = a * vx * vx + b * vx + c if vx is not None else None
            applied = vx is not None and vy < y_min - 1.0 and p["search_x_min"] <= vx <= p["search_x_max"]
            res["fit"] = dict(a=a, b=b, c=c, vertex_x=vx, vertex_y=vy, applied=bool(applied), n_roi=int(roi.size))
            if applied:
                x_at, y_min = vx, vy
        shift_z = minimal_shift(y_min, p["safe_z_min"], p["safe_z_max"])
        shift_x = minimal_shift(x_at, p["safe_x_min"], p["safe_x_max"])
        smooth = sm
    else:
        feat = extract_top(mats[0], p["ratio_k"]) if prof_override is None else prof_override
        clean = remove_small_chunks(feat, 30)
        sm = moving_average(median_filter(clean, p["med"]), max(3, p["med"] // 2))
        inr = (xs >= p["search_x_min"]) & (xs <= p["search_x_max"]) & ~np.isnan(sm) & (sm < 90.0)
        idx = np.where(inr)[0]
        valley_x = xs[idx[np.argmin(sm[idx])]]
        idx2 = np.where((xs >= p["search_x_min"]) & (xs <= valley_x) & ~np.isnan(sm) & (sm > -90.0))[0]
        i_pk = idx2[np.argmax(sm[idx2])]
        y_min = sm[i_pk]; x_at = xs[i_pk]
        res.update(valley_x=float(valley_x), peak_z=float(y_min), peak_x=float(x_at))
        shift_z = minimal_shift(y_min, p["safe_z_min"], p["safe_z_max"])
        shift_x = minimal_shift(x_at, p["safe_x_min"], p["safe_x_max"])
        smooth = sm
    clipped = False
    for k in ("shift_x", "shift_z"):
        pass
    osx, osz = shift_x, shift_z
    if abs(shift_x) > 15:
        shift_x = 15.0 * np.sign(shift_x); clipped = True
    if abs(shift_z) > 15:
        shift_z = 15.0 * np.sign(shift_z); clipped = True
    if abs(shift_x) <= 2.5 and abs(shift_z) <= 2.5:
        mach = (0.0, 0.0)
    else:
        mv = J_inv @ np.array([shift_x, shift_z])
        mach = (float(mv[0]), float(mv[1]))
    res.update(feature_x=float(x_at), feature_z=float(y_min), shift_x=float(shift_x), shift_z=float(shift_z),
               shift_x_unclipped=float(osx), shift_z_unclipped=float(osz), clipped=clipped, mach_x=mach[0], mach_z=mach[1])
    return res, xs, mats, raw_profs, mean_profs, feat, smooth


if __name__ == "__main__":
    out = {}
    for folder, mode in [(3, 2), (2, 1)]:
        r, xs, mats, raw, mean, feat, sm = run(folder, mode)
        out[f"{folder}_{mode}"] = r
        print(f"\n=== folder {folder} mode {mode} ===")
        print(json.dumps({k: v for k, v in r.items() if k not in ('J', 'J_inv')}, indent=1, default=float))
        print("J    =", np.round(np.array(r['J']), 4).tolist())
        print("J_inv=", np.round(np.array(r['J_inv']), 4).tolist())
        np.save(os.path.join(OUT, f"feat_{folder}_{mode}.npy"), np.stack([xs, feat, sm]))
        np.save(os.path.join(OUT, f"mats_{folder}_{mode}.npy"), np.stack(mats))
    json.dump(out, open(os.path.join(OUT, "guide_sim.json"), "w"), indent=1, default=float)
