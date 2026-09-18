import numpy as np, tifffile, cv2, os, sys, json
SRC = r"H:\000. PJT\01. HankookTire\03_이미지\PC3\2000026091715281105"
OUT = r"C:\Users\AIV\AppData\Local\Temp\claude\E--\03deaf05-d57c-4433-a88b-e1c14195f06c\scratchpad\depthnull"
pairs = [("5_SWU_Sh_L_D","11_SWU_Sh_L_D_Proc","6_SWU_Sh_L_I"),
         ("7_SWU_Sh_R_D","12_SWU_Sh_R_D_Proc","8_SWU_Sh_R_I"),
         ("17_SWD_Sh_L_D","23_SWD_Sh_L_D_Proc","18_SWD_Sh_L_I"),
         ("19_SWD_Sh_R_D","24_SWD_Sh_R_D_Proc","20_SWD_Sh_R_I"),
         ("3_SWU_Bead_D","10_SWU_Bead_D_Proc","4_SWU_Bead_I"),
         ("1_SWU_Cen_D","9_SWU_Cen_D_Proc","2_SWU_Cen_I")]
def load(name):
    p = os.path.join(SRC, name + ".mim")
    try:
        a = tifffile.imread(p)
    except Exception as e:
        print("tifffile fail", name, e); 
        with tifffile.TiffFile(p) as t:
            pg = t.pages[0]; print(pg.shape, pg.dtype, pg.tags)
        raise
    return a
def ds(img8, f=8):
    return cv2.resize(img8, (img8.shape[1]//f, img8.shape[0]//f), interpolation=cv2.INTER_AREA)
res = {}
for raw_n, proc_n, int_n in pairs:
    raw = load(raw_n); proc = load(proc_n)
    print(f"\n=== {raw_n} shape={raw.shape} dtype={raw.dtype} | {proc_n} shape={proc.shape} dtype={proc.dtype}")
    raw = raw.astype(np.float32); 
    null = (raw <= -900)
    valid = ~null
    n_null = int(null.sum()); tot = null.size
    vmin, vmax = float(raw[valid].min()), float(raw[valid].max())
    print(f"raw null(-999) count={n_null} ({100*n_null/tot:.3f}%), valid min/max={vmin:.3f}/{vmax:.3f}, unique sentinel vals={np.unique(raw[null])[:5]}")
    # rows/cols profile of nulls
    rowfrac = null.mean(axis=1); colfrac = null.mean(axis=0)
    print(f"rows with >50% null: {int((rowfrac>0.5).sum())}/{raw.shape[0]}, cols with >50% null: {int((colfrac>0.5).sum())}/{raw.shape[1]}")
    # bands of columns (image is rotated: width 8192 = scan direction?) -> show col-null fraction in 20 bins along each axis
    bins_r = [f"{rowfrac[i*raw.shape[0]//20:(i+1)*raw.shape[0]//20].mean()*100:.1f}" for i in range(20)]
    bins_c = [f"{colfrac[i*raw.shape[1]//20:(i+1)*raw.shape[1]//20].mean()*100:.1f}" for i in range(20)]
    print("null% by row-band (top->bottom):", bins_r)
    print("null% by col-band (left->right):", bins_c)
    # connected components of valid region (what removeStageFromRawData does)
    n_lab, lab, stats, cent = cv2.connectedComponentsWithStats(valid.astype(np.uint8), connectivity=8)
    areas = stats[1:, cv2.CC_STAT_AREA]
    order = np.argsort(-areas)
    print(f"valid CC count={n_lab-1}; top5 areas={areas[order[:5]].tolist()}; largest covers {100*areas[order[0]]/valid.sum():.3f}% of valid")
    # proc zeros vs raw nulls
    pz = (proc == 0)
    print(f"proc==0 count={int(pz.sum())} ({100*pz.mean():.3f}%); proc==0 & raw valid = {int((pz & valid).sum())}; raw null & proc!=0 = {int((null & ~pz).sum())}")
    print(f"proc==255 count={int((proc==255).sum())} ({100*(proc==255).mean():.3f}%)")
    # null in interior vs edge: null pixels whose row in [100, h-100] and col in [100, w-100]
    h, w = raw.shape
    inner = null[100:h-100, 100:w-100]
    print(f"interior null count={int(inner.sum())} ({100*inner.mean():.3f}% of interior)")
    # null component sizes (holes)
    n_lab2, lab2, stats2, _ = cv2.connectedComponentsWithStats(null.astype(np.uint8), connectivity=8)
    a2 = stats2[1:, cv2.CC_STAT_AREA]
    if len(a2):
        print(f"null CC count={n_lab2-1}; largest={int(a2.max())}; >1000px: {int((a2>1000).sum())}; >100px: {int((a2>100).sum())}; median={float(np.median(a2)):.0f}")
        # bbox of largest null comps
        for k in np.argsort(-a2)[:3]:
            x,y,bw,bh,ar = stats2[k+1]
            print(f"   null comp area={ar} bbox x={x} y={y} w={bw} h={bh}")
    # previews
    raw8 = np.zeros((h,w), np.uint8)
    lo, hi = np.percentile(raw[valid], [1,99])
    raw8[valid] = np.clip((raw[valid]-lo)/(hi-lo+1e-6)*255, 0, 255).astype(np.uint8)
    rawc = cv2.cvtColor(raw8, cv2.COLOR_GRAY2BGR); rawc[null] = (0,0,255)
    cv2.imwrite(os.path.join(OUT, raw_n + "_raw_nullred.png"), ds(rawc))
    proc8 = proc.astype(np.uint8) if proc.dtype != np.uint8 else proc
    cv2.imwrite(os.path.join(OUT, proc_n + "_proc.png"), ds(proc8))
    # zoom crops of largest interior null comp region in both raw and proc (full res)
    if len(a2):
        k = np.argsort(-a2)[0]; x,y,bw,bh,ar = stats2[k+1]
        cx, cy = x+bw//2, y+bh//2
        x0, y0 = max(0,cx-400), max(0,cy-200)
        crop_r = rawc[y0:y0+400, x0:x0+800]; crop_p = proc8[y0:y0+400, x0:x0+800]
        cv2.imwrite(os.path.join(OUT, raw_n + "_zoom_raw.png"), crop_r)
        cv2.imwrite(os.path.join(OUT, proc_n + "_zoom_proc.png"), crop_p)
    res[raw_n] = dict(null=n_null, null_pct=100*n_null/tot, cc_valid=int(n_lab-1), proc_zero=int(pz.sum()), proc_zero_valid=int((pz&valid).sum()))
    try:
        inten = load(int_n).astype(np.float32)
        i8 = np.clip(inten,0,255).astype(np.uint8)
        cv2.imwrite(os.path.join(OUT, int_n + "_int.png"), ds(i8))
        print(f"intensity min/max={inten.min():.1f}/{inten.max():.1f}; intensity==0 count={int((inten==0).sum())}; intensity==0 & raw null={int(((inten==0)&null).sum())}")
    except Exception as e:
        print("intensity load fail", e)
json.dump(res, open(os.path.join(OUT,"summary.json"),"w"), indent=1)
print("\nDONE")
