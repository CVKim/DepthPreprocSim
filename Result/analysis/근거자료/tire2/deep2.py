import numpy as np, tifffile, cv2, os, json
SRC = r"H:\000. PJT\01. HankookTire\03_이미지\PC3\2000026091715281105"
SRC1 = r"H:\000. PJT\01. HankookTire\03_이미지\PC3\1639390184_2000026091709423060"
OUT = r"C:\Users\AIV\AppData\Local\Temp\claude\E--\03deaf05-d57c-4433-a88b-e1c14195f06c\scratchpad\depthnull\tire2"
os.makedirs(OUT, exist_ok=True)


def load(folder, n):
    return tifffile.imread(os.path.join(folder, n + ".mim")).astype(np.float32)


def analyze(folder, raw_n, proc_n, tag):
    raw = load(folder, raw_n)
    proc = load(folder, proc_n).astype(np.uint8)
    h, w = raw.shape
    null = raw <= -900
    valid = ~null
    vals = raw[valid]
    zmin, zmax = float(vals.min()), float(vals.max())
    # saturation at measurement range limits
    sat_hi = (valid & (raw >= zmax - 1e-4)).sum()
    sat_lo = (valid & (raw <= zmin + 1e-4)).sum()
    n, lab, stats, cent = cv2.connectedComponentsWithStats(valid.astype(np.uint8), connectivity=8)
    areas = stats[1:, cv2.CC_STAT_AREA]
    order = np.argsort(-areas)
    big = 1 + int(order[0])
    band = lab == big
    rows = np.where(band.any(axis=1))[0]
    r0, r1 = int(rows.min()), int(rows.max())
    # band centre per column (largest component)
    colvalid = band.sum(axis=0)
    cy = np.zeros(w)
    for x in range(w):
        ys = np.where(band[:, x])[0]
        cy[x] = ys.mean() if len(ys) else np.nan
    cy_s = cy[~np.isnan(cy)]
    wob = float(cy_s.max() - cy_s.min()) if len(cy_s) else 0.0
    # removed components overlapping band rows (likely tire islands, not stage)
    removed_total = 0
    islands = []
    for k in order[1:]:
        x, y, bw, bh, a = stats[k + 1]
        removed_total += a
        if y + bh > r0 and y < r1:  # overlaps band rows
            # is it inside the band's vertical envelope at its columns? compare with cy at its centre col
            xc = int(x + bw / 2)
            c = cy[xc] if not np.isnan(cy[xc]) else -1
            inside = c >= 0 and (y + bh / 2) > (c - 500) and (y + bh / 2) < (c + 500)
            islands.append(dict(area=int(a), x=int(x), y=int(y), w=int(bw), h=int(bh), band_center_here=round(float(c), 1), inside_band_env=bool(inside)))
    islands.sort(key=lambda d: -d["area"])
    inside_px = sum(d["area"] for d in islands if d["inside_band_env"])
    # black composition inside band envelope (band ∪ islands inside): null vs clipped
    env = band.copy()
    for d in islands:
        if d["inside_band_env"]:
            pass
    black = proc == 0
    blk_band = black & band
    nul_band_holes = 0  # nulls enclosed by band columns between band top/bottom
    above = np.cumsum(band, axis=0) > 0
    below = np.cumsum(band[::-1], axis=0)[::-1] > 0
    enclosed_null = null & above & below
    # row-bin profile relative to band centre: use rows offset from cy per column -> bin by (y - cy)
    yy = np.arange(h)[:, None] - cy[None, :]
    bins = np.linspace(-600, 600, 25)
    prof = []
    for i in range(len(bins) - 1):
        m = (yy >= bins[i]) & (yy < bins[i + 1]) & ~np.isnan(yy)
        tot = m.sum()
        if tot == 0:
            prof.append(None)
            continue
        prof.append(dict(off=[float(bins[i]), float(bins[i + 1])], null_pct=round(100 * (null & m).sum() / tot, 1),
                         black_pct=round(100 * (black & m).sum() / tot, 1), black_valid_pct=round(100 * (black & valid & m).sum() / tot, 1),
                         sat_hi_pct=round(100 * (valid & (raw >= zmax - 1e-4) & m).sum() / tot, 2),
                         sat_lo_pct=round(100 * (valid & (raw <= zmin + 1e-4) & m).sum() / tot, 2)))
    res = dict(tag=tag, raw=raw_n, proc=proc_n, null_pct=round(100 * null.mean(), 2), z_min=zmin, z_max=zmax,
               sat_hi_px=int(sat_hi), sat_lo_px=int(sat_lo), band_rows=[r0, r1], band_center_wobble_px=round(wob, 1),
               band_center_min=round(float(cy_s.min()), 1), band_center_max=round(float(cy_s.max()), 1),
               valid_components=int(n - 1), largest_pct_of_valid=round(100 * areas[order[0]] / valid.sum(), 2),
               removed_valid_px=int(removed_total), removed_pct_of_image=round(100 * removed_total / raw.size, 2),
               removed_inside_band_envelope_px=int(inside_px), islands_top10=islands[:10],
               black_in_band_pct=round(100 * blk_band.sum() / band.sum(), 2), enclosed_null_px=int(enclosed_null.sum()),
               enclosed_null_pct_of_band=round(100 * enclosed_null.sum() / band.sum(), 3), profile_by_offset=prof)
    print(f"\n=== {tag} {raw_n}->{proc_n}: null {res['null_pct']}%, z [{zmin:.3f},{zmax:.3f}] sat_hi={sat_hi} sat_lo={sat_lo}")
    print(f"  band rows {r0}..{r1}, centre wobble {wob:.0f} px ({res['band_center_min']}..{res['band_center_max']}), comps={n-1}, largest={res['largest_pct_of_valid']}% of valid")
    print(f"  removed valid px={removed_total} ({res['removed_pct_of_image']}% of image); inside band envelope={inside_px} px")
    for d in islands[:6]:
        print("   island", d)
    print(f"  black in band {res['black_in_band_pct']}%, enclosed null holes {enclosed_null.sum()} px ({res['enclosed_null_pct_of_band']}% of band)")
    print("  offset-from-centre profile (off, null%, black%, black_valid%, sat_hi%, sat_lo%):")
    for p in prof:
        if p: print(f"   {p['off'][0]:6.0f}..{p['off'][1]:6.0f}  null {p['null_pct']:5.1f}  black {p['black_pct']:5.1f}  black_valid {p['black_valid_pct']:5.1f}  satHi {p['sat_hi_pct']:5.2f} satLo {p['sat_lo_pct']:5.2f}")
    # visuals: band centre curve overlay on proc preview, and crops at the speckle line
    prev = cv2.resize(proc, (w // 8, h // 8), interpolation=cv2.INTER_AREA)
    prevc = cv2.cvtColor(prev, cv2.COLOR_GRAY2BGR)
    for x in range(0, w, 8):
        if not np.isnan(cy[x]):
            cv2.circle(prevc, (x // 8, int(cy[x] / 8)), 0, (0, 255, 255), -1)
    cv2.imwrite(os.path.join(OUT, f"{tag}_{proc_n}_centre.png"), prevc)
    raw8 = np.zeros((h, w), np.uint8)
    p1, p99 = np.percentile(vals, [1, 99])
    raw8[valid] = np.clip((raw[valid] - p1) / (p99 - p1) * 255, 0, 255)
    rawc = cv2.cvtColor(raw8, cv2.COLOR_GRAY2BGR)
    rawc[null] = (0, 0, 255)
    rawc[valid & (raw >= zmax - 1e-4)] = (255, 0, 255)  # saturated high = magenta
    rawc[valid & (raw <= zmin + 1e-4)] = (255, 255, 0)  # saturated low = cyan
    cv2.imwrite(os.path.join(OUT, f"{tag}_{raw_n}_raw_sat.png"), cv2.resize(rawc, (w // 8, h // 8), interpolation=cv2.INTER_AREA))
    # crop where black_valid is highest along the centre-relative profile: choose offset bin with max null within band
    inb = [p for p in prof if p and -450 < p['off'][0] < 450]
    if inb:
        pb = max(inb, key=lambda p: p['null_pct'])
        off = (pb['off'][0] + pb['off'][1]) / 2
        x0 = 3000
        yc = int(cy[x0] + off) if not np.isnan(cy[x0]) else h // 2
        y0 = max(0, min(h - 341, yc - 170))
        cv2.imwrite(os.path.join(OUT, f"{tag}_{proc_n}_crop_speckle.png"), proc[y0:y0 + 341, x0:x0 + 671])
        cv2.imwrite(os.path.join(OUT, f"{tag}_{raw_n}_crop_speckle.png"), rawc[y0:y0 + 341, x0:x0 + 671])
        res["speckle_crop"] = dict(x0=x0, y0=int(y0), offset_from_centre=off)
    return res


allres = []
for raw_n, proc_n in [("5_SWU_Sh_L_D", "11_SWU_Sh_L_D_Proc"), ("7_SWU_Sh_R_D", "12_SWU_Sh_R_D_Proc"), ("17_SWD_Sh_L_D", "23_SWD_Sh_L_D_Proc"), ("19_SWD_Sh_R_D", "24_SWD_Sh_R_D_Proc")]:
    allres.append(analyze(SRC, raw_n, proc_n, "tire2"))
# tire1 comparison for wobble/saturation on two images
for raw_n, proc_n in [("7_SWU_Sh_R_D", "12_SWU_Sh_R_D_Proc"), ("5_SWU_Sh_L_D", "11_SWU_Sh_L_D_Proc")]:
    allres.append(analyze(SRC1, raw_n, proc_n, "tire1"))
json.dump(allres, open(os.path.join(OUT, "deep2.json"), "w"), indent=1, ensure_ascii=False)
print("DONE")
