# Python re-implementation of alg_depth_preproc (PREPROC_3D) for independent cross-checks.
# Reference: SITE (PC3) build of 3dDepthProcessing.cpp, 2026-07-08, rc 1.0.2.0.38cf930_HT
# (copy in ../../reference/site_pc3_20260708/). Stage removal depends on DepthPreprocType:
#   INNERCENTER -> none, BEAD -> remove_stage_bead, INSHOULDER -> remove_stage(break_kernel=3)
import numpy as np, tifffile, cv2, os, sys, time
SRC = r"H:\000. PJT\01. HankookTire\03_이미지\PC3\1639390184_2000026091709423060"
OUT = os.path.dirname(os.path.abspath(__file__))


def load(n, folder=SRC):
    p = n if os.path.isabs(n) else os.path.join(folder, n + ".mim")
    return tifffile.imread(p).astype(np.float32)


def remove_stage_bead(src):
    """site removeStageFromRawDataBead: keep the largest 8-connected valid component."""
    mask = (src > -900.0).astype(np.uint8)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8, ltype=cv2.CV_32S)
    if n <= 1:
        return src
    best = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
    out = src.copy()
    out[lab != best] = -999.0
    return out


def remove_stage(src, break_kernel=3, restore=False):
    """site removeStageFromRawData(src, stagePos, breakKernel).

    Site source: erode -> largest component -> dilate -> AND mask. BUT in the site C++ `cv::Mat labelMask = mask;`
    is a shallow copy and `cv::erode(mask, labelMask, kernel)` erodes IN PLACE, so `mask` itself is the eroded mask
    when the final AND runs. Effective behaviour (verified 100 % against the site output and depth_sim.exe):
    keep = dilate(largest eroded component) & eroded mask == the eroded largest component only.
    restore=False reproduces the site build; restore=True is what the comments in the site code intended.
    """
    mask = (src > -900.0).astype(np.uint8)
    label_mask = mask
    kernel = None
    if break_kernel >= 3:
        kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (break_kernel, break_kernel))
        label_mask = cv2.erode(mask, kernel)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(label_mask, connectivity=8, ltype=cv2.CV_32S)
    if n <= 1:
        return src
    best = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
    keep = (lab == best).astype(np.uint8) * 255
    if break_kernel >= 3:
        keep = cv2.dilate(keep, kernel)
        and_mask = mask if restore else label_mask      # site build: label_mask aliases mask (already eroded)
        keep = cv2.bitwise_and(keep, and_mask * 255)
    out = src.copy()
    out[keep == 0] = -999.0
    return out


def remove_stage_for_type(src, type_name, break_kernel=3):
    t = (type_name or "INSHOULDER").upper()
    if t == "INNERCENTER":
        return src.copy()
    if t == "BEAD":
        return remove_stage_bead(src)
    return remove_stage(src, break_kernel)


def scale16_ignore_null(data, null_mask):
    valid = ~null_mask
    if valid.sum() == 0:
        return np.zeros_like(data)
    mn, mx = float(data[valid].min()), float(data[valid].max())
    if mx == mn:
        scaled = np.zeros_like(data)
    else:
        scaled = ((data - mn) / (mx - mn) * 65536.0).astype(np.float32)   # cv float32 arithmetic
    scaled[null_mask] = 0
    return scaled


def patch_median(data, ph, pw, overlap):
    h, w = data.shape
    sh = max(1, int(ph * (1.0 - overlap)))
    sw = max(1, int(pw * (1.0 - overlap)))
    oh = (h - ph) // sh + 1
    ow = (w - pw) // sw + 1
    from numpy.lib.stride_tricks import sliding_window_view
    win = sliding_window_view(data, (ph, pw))[::sh, ::sw][:oh, :ow]   # (oh, ow, ph, pw)
    flat = win.reshape(oh, ow, -1)
    med = np.median(flat, axis=2).astype(np.float32)  # numpy median: even n -> avg of two middle, same as C++
    basis = cv2.resize(med, (w, h), interpolation=cv2.INTER_LINEAR)
    return basis


def percentile_cpp(img, pct):
    vals = np.sort(img.ravel())
    pos = (pct / 100.0) * (vals.size - 1)
    lo = int(np.floor(pos))
    hi = int(np.ceil(pos))
    frac = pos - lo
    return float(vals[lo]) + frac * (float(vals[hi]) - float(vals[lo]))


def post_clip_normalize(img, lower=5, upper=95, abs_limit=1000):
    low = percentile_cpp(img, lower)
    high = percentile_cpp(img, upper)
    low = max(low, -abs_limit)
    high = min(high, abs_limit)
    clipped = img.copy()
    clipped[clipped < low] = low
    clipped[clipped > high] = high
    mn, mx = float(clipped.min()), float(clipped.max())
    scale = 255.0 / (mx - mn) if mx != mn else 0.0
    shift = -mn * scale
    normed = clipped * np.float32(scale) + np.float32(shift)
    out = np.clip(np.rint(normed), 0, 255).astype(np.uint8)
    return out, low, high


def process_high_curvature(data, patch=(15, 15), overlap=0.25, lower=5, upper=95):
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
    type_name = sys.argv[3] if len(sys.argv) > 3 else "INSHOULDER"
    raw = load(raw_n)
    proc = load(proc_n).astype(np.uint8)
    t = time.time()
    src = remove_stage_for_type(raw, type_name)
    out, low, high, diff_f, nm = process_high_curvature(src)
    eq = (out == proc)
    d = np.abs(out.astype(int) - proc.astype(int))
    print(f"[{type_name}] {raw_n}->{proc_n}: exact-match {100*eq.mean():.3f}%  |diff|<=1: {100*(d<=1).mean():.3f}%  max|diff|={d.max()}  low/high={low:.1f}/{high:.1f}  nulls={nm.sum()}  ({time.time()-t:.1f}s)")
