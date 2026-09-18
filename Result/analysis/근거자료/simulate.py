# Faithful re-implementation of alg_depth_preproc INSHOULDER path (processHighCurvature) for A/B against site output
import numpy as np, tifffile, cv2, os, sys, time
SRC = r"H:\000. PJT\01. HankookTire\03_이미지\PC3\1639390184_2000026091709423060"
OUT = r"C:\Users\AIV\AppData\Local\Temp\claude\E--\03deaf05-d57c-4433-a88b-e1c14195f06c\scratchpad\depthnull"
def load(n): return tifffile.imread(os.path.join(SRC, n + ".mim")).astype(np.float32)

def remove_stage(src):
    mask = (src > -900.0).astype(np.uint8)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8, ltype=cv2.CV_32S)
    if n <= 1: return src
    best = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
    out = src.copy(); out[lab != best] = -999.0
    return out

def scale16_ignore_null(data, null_mask):
    valid = ~null_mask
    if valid.sum() == 0: return np.zeros_like(data)
    mn, mx = float(data[valid].min()), float(data[valid].max())
    if mx == mn: scaled = np.zeros_like(data)
    else: scaled = ((data - mn) / (mx - mn) * 65536.0).astype(np.float32)   # cv float32 arithmetic
    scaled[null_mask] = 0
    return scaled

def patch_median(data, ph, pw, overlap):
    h, w = data.shape
    sh = max(1, int(ph * (1.0 - overlap))); sw = max(1, int(pw * (1.0 - overlap)))
    oh = (h - ph) // sh + 1; ow = (w - pw) // sw + 1
    # gather patches
    from numpy.lib.stride_tricks import sliding_window_view
    win = sliding_window_view(data, (ph, pw))[::sh, ::sw][:oh, :ow]   # (oh, ow, ph, pw)
    flat = win.reshape(oh, ow, -1)
    med = np.median(flat, axis=2).astype(np.float32)  # numpy median: even n -> avg of two middle, same as C++
    basis = cv2.resize(med, (w, h), interpolation=cv2.INTER_LINEAR)
    return basis

def percentile_cpp(img, pct):
    vals = np.sort(img.ravel())
    pos = (pct / 100.0) * (vals.size - 1)
    lo = int(np.floor(pos)); hi = int(np.ceil(pos)); frac = pos - lo
    return float(vals[lo]) + frac * (float(vals[hi]) - float(vals[lo]))

def post_clip_normalize(img, lower=5, upper=95, abs_limit=1000):
    low = percentile_cpp(img, lower); high = percentile_cpp(img, upper)
    low = max(low, -abs_limit); high = min(high, abs_limit)
    clipped = img.copy(); clipped[clipped < low] = low; clipped[clipped > high] = high
    # cv::normalize NORM_MINMAX to 0..255 on float32 then convertTo CV_8U (round to nearest, saturate)
    mn, mx = float(clipped.min()), float(clipped.max())
    scale = 255.0 / (mx - mn) if mx != mn else 0.0
    shift = -mn * scale
    normed = clipped * np.float32(scale) + np.float32(shift)
    out = np.clip(np.rint(normed), 0, 255).astype(np.uint8)
    return out, low, high

def process_high_curvature(data, patch=(15,15), overlap=0.25, lower=5, upper=95):
    null_mask = (data == -999)
    scaled = scale16_ignore_null(data, null_mask)
    basis = patch_median(scaled, patch[1], patch[0], overlap)
    diff = (scaled - basis).astype(np.float32)
    diff16 = np.clip(diff.astype(np.float64), -32768, 32767).astype(np.int16)  # trunc toward zero like static_cast<short>
    diff_f = diff16.astype(np.float32)
    out, low, high = post_clip_normalize(diff_f, lower, upper)
    out[null_mask] = 0
    return out, low, high, diff_f, null_mask

if __name__ == "__main__":
    raw_n, proc_n = sys.argv[1], sys.argv[2]
    raw = load(raw_n); proc = load(proc_n).astype(np.uint8)
    for tag, src in [("noStage", raw), ("withStage", remove_stage(raw))]:
        t = time.time()
        out, low, high, diff_f, nm = process_high_curvature(src)
        eq = (out == proc); d = np.abs(out.astype(int) - proc.astype(int))
        print(f"[{tag}] {raw_n}->{proc_n}: exact-match {100*eq.mean():.3f}%  |diff|<=1: {100*(d<=1).mean():.3f}%  max|diff|={d.max()}  low/high={low:.1f}/{high:.1f}  nulls={nm.sum()}  ({time.time()-t:.1f}s)")
        cv2.imwrite(os.path.join(OUT, f"sim_{proc_n}_{tag}.png"), cv2.resize(out, (out.shape[1]//8, out.shape[0]//8), interpolation=cv2.INTER_AREA))
        np.save(os.path.join(OUT, f"sim_{proc_n}_{tag}.npy"), out)
