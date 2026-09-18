import numpy as np, tifffile, cv2, os
SRC = r"H:\000. PJT\01. HankookTire\03_이미지\PC3\1639390184_2000026091709423060"
OUT = r"C:\Users\AIV\AppData\Local\Temp\claude\E--\03deaf05-d57c-4433-a88b-e1c14195f06c\scratchpad\depthnull"
def load(n): return tifffile.imread(os.path.join(SRC, n + ".mim")).astype(np.float32)
for raw_n, proc_n in [("7_SWU_Sh_R_D","12_SWU_Sh_R_D_Proc"),("19_SWD_Sh_R_D","24_SWD_Sh_R_D_Proc"),("5_SWU_Sh_L_D","11_SWU_Sh_L_D_Proc"),("17_SWD_Sh_L_D","23_SWD_Sh_L_D_Proc")]:
    raw = load(raw_n); proc = load(proc_n).astype(np.uint8)
    null = raw <= -900; valid = ~null
    n_lab, lab, stats, _ = cv2.connectedComponentsWithStats(valid.astype(np.uint8), connectivity=8)
    big = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
    band = (lab == big)
    black_in_band = (proc == 0) & band
    white_in_band = (proc == 255) & band
    rows = np.where(band.any(axis=1))[0]; r0, r1 = rows.min(), rows.max()
    print(f"\n=== {proc_n}: tire band rows {r0}..{r1} ({r1-r0+1} rows); band px={band.sum()}; black-in-band={black_in_band.sum()} ({100*black_in_band.sum()/band.sum():.2f}% of band); white-in-band={white_in_band.sum()} ({100*white_in_band.sum()/band.sum():.2f}%)")
    # row profile of black-in-band in 20 bins over band rows
    bh = black_in_band[r0:r1+1].sum(axis=1) / np.maximum(band[r0:r1+1].sum(axis=1),1)
    wh = white_in_band[r0:r1+1].sum(axis=1) / np.maximum(band[r0:r1+1].sum(axis=1),1)
    nb = 20; L = len(bh)
    print("black% by band-row bin (top->bottom):", [f"{100*bh[i*L//nb:(i+1)*L//nb].mean():.1f}" for i in range(nb)])
    print("white% by band-row bin (top->bottom):", [f"{100*wh[i*L//nb:(i+1)*L//nb].mean():.1f}" for i in range(nb)])
    # rows where black fraction > 30%
    hot = np.where(bh > 0.3)[0] + r0
    if len(hot): print(f"rows with >30% black: {len(hot)} rows, range {hot.min()}..{hot.max()}")
    # interior null holes within band bbox (null pixels enclosed in band rows), count/sizes
    inner_null = null.copy(); inner_null[:r0] = False; inner_null[r1+1:] = False
    # exclude null columns fully outside band (the fringe) : keep nulls that have band pixels both above and below in same column
    above = np.cumsum(band, axis=0) > 0
    below = np.cumsum(band[::-1], axis=0)[::-1] > 0
    enclosed = inner_null & above & below
    n2, l2, s2, _ = cv2.connectedComponentsWithStats(enclosed.astype(np.uint8), connectivity=8)
    a2 = s2[1:, cv2.CC_STAT_AREA]
    print(f"enclosed null holes in band: {n2-1} comps, total px={enclosed.sum()}, >50px={int((a2>50).sum()) if len(a2) else 0}, largest={int(a2.max()) if len(a2) else 0}")
    for k in (np.argsort(-a2)[:5] if len(a2) else []):
        x,y,bw,bhh,ar = s2[k+1]; print(f"   hole area={ar} bbox x={x} y={y} w={bw} h={bhh}")
    # crops 671x341 at band top edge and at band bottom edge, x=0 and x=4000
    raw8 = np.zeros_like(proc); lo, hi = np.percentile(raw[valid],[1,99]); raw8[valid] = np.clip((raw[valid]-lo)/(hi-lo)*255,0,255)
    rawc = cv2.cvtColor(raw8, cv2.COLOR_GRAY2BGR); rawc[null] = (0,0,255)
    for tag, y0 in [("top", max(0, r0-60)), ("bot", min(raw.shape[0]-341, r1-280))]:
        for x0 in (0, 4000):
            cv2.imwrite(os.path.join(OUT, f"crop_{proc_n}_{tag}_x{x0}.png"), proc[y0:y0+341, x0:x0+671])
            cv2.imwrite(os.path.join(OUT, f"crop_{raw_n}_{tag}_x{x0}.png"), rawc[y0:y0+341, x0:x0+671])
    # hottest black row region crop
    if len(hot):
        yc = int(np.median(hot)); y0 = max(0, yc-170)
        cv2.imwrite(os.path.join(OUT, f"crop_{proc_n}_hot_x2000.png"), proc[y0:y0+341, 2000:2671])
        cv2.imwrite(os.path.join(OUT, f"crop_{raw_n}_hot_x2000.png"), rawc[y0:y0+341, 2000:2671])
print("DONE")
