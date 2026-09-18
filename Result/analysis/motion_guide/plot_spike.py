import numpy as np, os, sys, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import guide_sim as g

OUT = r"E:\Dev\DepthPreprocSim\Result\analysis\motion_guide"
os.makedirs(OUT, exist_ok=True)
fp = "C:/Windows/Fonts/malgun.ttf"
if os.path.exists(fp):
    font_manager.fontManager.addfont(fp)
    plt.rcParams["font.family"] = font_manager.FontProperties(fname=fp).get_name()
plt.rcParams["axes.unicode_minus"] = False

mats = g.load_profiles(3, 2)
p = g.PARAMS[2]
xs = p["fov_x_start"] + np.arange(mats[0].shape[1]) * p["x_res"]
bottom = g.extract_bottom(mats[0], 0.1)
med = np.array([np.median(c[g.valid(c)]) if g.valid(c).any() else np.nan for c in mats[0].T])
sm_b = g.moving_average(g.median_filter(g.remove_small_chunks(bottom, 30), 31), 15)
sm_m = g.moving_average(g.median_filter(g.remove_small_chunks(med, 30), 31), 15)

fig, axes = plt.subplots(2, 1, figsize=(14, 9), gridspec_kw=dict(height_ratios=[1.3, 1]))
ax = axes[0]
for r in range(10):
    v = mats[0][r]; ok = g.valid(v)
    ax.plot(xs[ok], v[ok], ".", ms=1.5, color="#9aa", alpha=0.6, label="10개 행 원시 점" if r == 0 else None)
ax.plot(xs, sm_b, color="#d33", lw=2, label="현장: bottom k=0.1 (열별 10점 중 최소) → 스무딩")
ax.plot(xs, sm_m, color="#27a", lw=2, label="대안: 열별 중앙값 → 스무딩")
ax.axvspan(-12.5, -1.0, color="#fc6", alpha=0.35, label="벽(x~0) 인접 이상치 구간")
ax.axhline(-35, color="#e90", ls="--", lw=1.2, label="Safe Zone z=-35")
ax.plot([-10.9], [-20.06], "o", color="#d33", ms=9)
ax.annotate("현장 feature (-10.9, -20.06)\n→ dZ -14.94", (-10.9, -20.06), (8, -30), arrowprops=dict(arrowstyle="->"), fontsize=10)
ax.plot([-22.0], [-15.37], "o", color="#27a", ms=9)
ax.annotate("중앙값 기준 valley (-22.0, -15.37)\n→ dZ -19.63 → 클립 -15", (-22.0, -15.37), (-60, -32), arrowprops=dict(arrowstyle="->"), fontsize=10)
ax.set_xlim(-70, 25); ax.set_ylim(-40, 40); ax.grid(alpha=.3)
ax.set_title("Shoulder R 모션 가이드 원본 스캔 0 (TireInput_0_2.mim, 10행×1940열): 특징 프로파일 비교", fontsize=12)
ax.set_xlabel("x (mm)"); ax.set_ylabel("z (mm)"); ax.legend(loc="upper right", fontsize=9)

ax = axes[1]
spread = np.array([(c[g.valid(c)].max() - c[g.valid(c)].min()) if g.valid(c).sum() >= 2 else np.nan for c in mats[0].T])
nval = np.array([g.valid(c).sum() for c in mats[0].T])
ax.plot(xs, spread, color="#555", lw=1.2, label="열 내 10점 z 범위(max-min)")
ax.axvspan(-12.5, -1.0, color="#fc6", alpha=0.35)
ax.set_xlim(-70, 25); ax.set_ylim(0, 30); ax.grid(alpha=.3)
ax.set_ylabel("z 범위 (mm)"); ax.set_xlabel("x (mm)")
ax2 = ax.twinx(); ax2.plot(xs, nval, color="#3a3", lw=1, alpha=.7, label="유효 행 수(10 중)"); ax2.set_ylim(0, 11); ax2.set_ylabel("유효 행 수")
ax.set_title("열별 산포: 평탄면 ≤0.5 mm, 벽 인접(x -12..-1) 5~27 mm → 최소값 채택 시 튐", fontsize=11)
h1, l1 = ax.get_legend_handles_labels(); h2, l2 = ax2.get_legend_handles_labels(); ax.legend(h1 + h2, l1 + l2, loc="upper left", fontsize=9)
plt.tight_layout()
plt.savefig(os.path.join(OUT, "shoulderR_guide_spike_analysis.png"), dpi=110)
print("saved", os.path.join(OUT, "shoulderR_guide_spike_analysis.png"))
