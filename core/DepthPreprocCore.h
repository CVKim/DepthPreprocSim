#pragma once
// Offline simulator core for alg_depth_preproc.dll (PREPROC_3D).
// DepthProcessor = verbatim DLL functions, SimPipeline = C3DPreprocess::INSPECT replica,
// DepthProcessorExp = experimental variants (only used when a flag is on).

#include <opencv2/opencv.hpp>
#include <array>
#include <string>

namespace DepthProcessor
{
    // Verbatim copies from the SITE (PC3) build of 3dDepthProcessing.cpp, 2026-07-08, rc 1.0.2.0.38cf930_HT
    // (reference/site_pc3_20260708/). It differs from talos-platform commit 0a6814f2 only in the
    // stage-removal step. Default arguments are declared on the definitions, exactly as in the original file.
    void removeStageFromRawData(cv::Mat& src32f, int stagePosMode, int breakKernel);   // site: INSHOULDER, breakKernel 3
    void removeStageFromRawDataBead(cv::Mat& src32f, int stagePosMode);                // site: BEAD (plain largest component)
    double computePercentile(const cv::Mat& img, double pct);
    cv::Mat postClipNormalize(const cv::Mat& img, double lower_pct, double upper_pct, double abs_limit);
    cv::Mat scaleTo16bitIgnoreNull(const cv::Mat& data, const cv::Mat& null_mask);
    cv::Mat patchBasedMedian(const cv::Mat& data, const cv::Size& patch_size, float overlap);
    cv::Mat processHighCurvature(const cv::Mat& data, cv::Size patch_size, double overlap);
}

enum StageMode { STAGE_AUTO = 0, STAGE_TOP = 1, STAGE_BOTTOM = 2, STAGE_NONE = 3 };

// DepthPreprocType of the [CALxxxx] section. In the site build the type selects the stage-removal step:
//   INNERCENTER -> none, BEAD -> removeStageFromRawDataBead, INSHOULDER -> removeStageFromRawData(.., 3).
enum DepthType { TYPE_INNERCENTER = 0, TYPE_BEAD = 1, TYPE_INSHOULDER = 2 };
const int kSiteBreakKernel = 3;   // literal used by the site build for INSHOULDER

// Experimental algorithm "v2" (not in any DLL): band-aware null handling + slope-compensated fixed-scale residual.
//   1. tire band = largest component (erode-break -> dilate -> AND original mask), per-column envelope gives the band region
//   2. unreliable valid pixels next to nulls are dropped, nulls inside the band are filled (multi-scale normalized convolution)
//   3. reference surface = null-aware patch median (mm), residual r = z - reference
//   4. r * cos(theta) (theta = inclination of the reference surface) -> depth along the surface normal, so steep zones are not amplified
//   5. fixed scale: +-range_mm -> 1..255, 128 = on the reference surface, 0 only outside the band
struct SimV2
{
    bool   on = false;
    int    mode = 0;            // 0 = neutral (unreliable pixels -> 128), 1 = restore (nulls interpolated, measured pixels kept),
                                // 2 = restore2 (restore + dropout-zone handling: wrong-valued clusters are rejected against a
                                //     running-median surface along the rotation axis and refilled in the residual domain)
    bool   pct_norm = false;    // restore mode: percentile contrast (lower/upper on measured pixels) instead of the fixed range
    double spike_mm = 1.0;      // restore mode: gross spikes inside dropout zones are re-interpolated (0 = off)
    double range_mm = 0.4;
    int    edge = 2;            // px of valid data dropped around nulls (mixed pixels); 0 = keep
    bool   slope = true;        // multiply the residual by cos(theta)
    bool   fill = true;         // interpolate small holes (<= small_hole px); larger holes are always neutral
    int    small_hole = 60;     // px
    int    env_median = 101;    // 1-D median window (columns) for the band envelope
    int    rho_win = 15;        // window for the local null density
    double rho_thr = 0.03;      // density above which residual outliers are rejected
    double rho_kill = 0.25;     // density above which every pixel is treated as unreliable
    double out_mm = 0.3;        // residual outlier threshold inside dropout zones
    double feather = 1.5;       // px, texture fades to neutral next to unusable pixels (0 = off)
    double taper = 0.0;         // px, optional fade at the band border (0 = off)
    double px_x_mm = 0.3, px_y_mm = 0.1;   // pixel pitch along columns / rows
    // restore2 only
    int    rm_win = 301;        // columns of the row-wise running median (robust surface along the rotation axis)
    int    rm_step = 16;        // the running median is evaluated every rm_step columns and interpolated in between
    int    zone_w = 401;        // dropout zone = null density over a zone_w x zone_h window (elongated along the rotation axis)
    int    zone_h = 21;
    double zone_thr = 0.04;     // density above which a pixel belongs to a dropout zone
    int    zone_margin = 30;    // px, the zone is grown by this margin (closes the gaps between neighbouring zone strips)
    double zout_mm = 1.2;       // zone only: measured pixels further than this from the running-median surface are rejected
    double zout2_mm = 0.5;      // zone only: second pass, deviation from the local 5x5 median of the residual (0 = off)
    double zseed_mm = 0.6;      // zone only: |residual| up to this is on the surface (seed of the continuity growth)
    double zgrow_mm = 0.35;     // zone only: residual step per pixel that still counts as continuous (0 = plain zout_mm threshold)
    bool   zone_norm = true;    // percentile contrast is computed on measured pixels outside the zones only
};

struct SimExp
{
    bool use_ini_pct = false;
    bool valid_pct = false;
    bool masked_median = false;
    int  null_value = -1;   // -1 = off (null stays 0 like the DLL)
    int  fill_holes = 0;    // 0 = off, else max component area in px
    bool stage_restore = false; // INSHOULDER: erode on a separate buffer so dilate AND original mask restores the 1 px border
                                // (what the site comments intend; the site build ANDs with the eroded mask)
    double abs_mm = 0.0;    // > 0: fixed physical scale, diff of +-abs_mm -> 1..255 (128 = on the basis plane), 0 reserved for null
    SimV2 v2;               // v2.on replaces the whole pipeline
    bool Any() const { return use_ini_pct || valid_pct || masked_median || null_value >= 0 || fill_holes > 0 || stage_restore || abs_mm > 0 || v2.on; }
};

struct SimParams
{
    int    patch_w = 15;
    int    patch_h = 15;
    double overlap = 0.25;          // DLL stores float(_wtof(str)) widened to double
    double lower = 5.0;
    double upper = 95.0;
    int    stage = STAGE_AUTO;
    int    type = TYPE_INSHOULDER;      // selects the stage-removal step (see DepthType)
    int    break_kernel = kSiteBreakKernel; // erosion kernel for INSHOULDER; != 3 is not DLL-identical
    std::array<int, 4> roi{ 9999, 9999, 0, 0 };  // x1,y1,x2,y2
    SimExp exp;

    bool UseRoi() const { return roi[2] > roi[0] && roi[3] > roi[1]; }
    // Same arithmetic as patchBasedMedian: int(patch * (1.0f - float(overlap))), min 1.
    int StepW() const;
    int StepH() const;
};

struct SimTiming
{
    double remove_stage = 0, scale = 0, basis = 0, diff = 0, clip_normalize = 0, total = 0;
};

// Every image below is in the full input frame (zeros outside the ROI, masks = 255 outside).
struct SimResult
{
    cv::Size original_size;
    cv::Rect roi_rect;              // effective ROI (roi & imageRect), full image when unused
    bool     use_roi = false;
    int      step_w = 0, step_h = 0;

    cv::Mat result8u;               // official 8U result (what the DLL writes)
    cv::Mat src_after_stage;        // 32F input after removeStage (and hole fill when enabled)
    cv::Mat null_mask_before;       // 8U 255 where raw == -999
    cv::Mat null_mask_after;        // 8U 255 where == -999 after stage removal, or outside ROI
    cv::Mat stage_removed_mask;     // 8U 255 where removeStage set -999
    cv::Mat scaled;                 // 32F output of scaleTo16bitIgnoreNull
    cv::Mat basis;                  // 32F patchBasedMedian plane
    cv::Mat diff_f32;               // 32F diff after int16 clamp (input of postClipNormalize)
    cv::Mat clipped;                // 32F after clipping, before normalization

    cv::Mat band_mask;              // v2 only: 8U 255 inside the tire band region (holes included)
    cv::Mat hole_mask;              // v2 only: 8U 255 where a null / unreliable pixel inside the band was filled
    cv::Mat zone_mask;              // v2 restore2: 8U 255 inside dropout zones
    cv::Mat rejected_mask;          // v2 restore2: 8U 255 where a measured pixel was rejected as wrong and refilled
    cv::Mat surface;                // v2 restore2: 32F robust surface R in mm (dumped as surface.f32 with --dump all)
    double v2_zone_pct = 0, v2_rejected_pct = 0, v2_filled_pct = 0;   // share of the band (%)
    double unit_mm_override = 0;    // v2: clip_low/high and diff are in um -> 0.001
    double clip_low = 0, clip_high = 0, norm_min = 0, norm_max = 0;
    double z_min = 0, z_max = 0;    // valid min/max used by scaleTo16bitIgnoreNull
    bool   capture_identical = true;// captured pipeline == official processHighCurvature output
    bool   dll_identical = true;    // false when any experimental option is on
    SimTiming timing;
};

namespace SimPipeline
{
    // Reproduces C3DPreprocess::INSPECT on a CV_32FC1 image. Throws on failure.
    void Run(const cv::Mat& src32f, const SimParams& params, SimResult& out);
}

namespace DepthProcessorV2
{
    struct Debug
    {
        cv::Mat band, holes, stage_removed, zf, basis, rn_um, clipped_um;
        cv::Mat zone, rejected;         // restore2: dropout zones / measured pixels rejected as wrong
        cv::Mat surface;                // restore2: robust surface R (mm)
        bool surface_model = false;     // restore2: R is built on the wobble-registered cross-section
        double zone_pct = 0, rejected_pct = 0, filled_pct = 0;   // share of the band (%)
        double z_min = 0, z_max = 0, low_mm = 0, high_mm = 0;
    };
    // src32f: CV_32FC1 depth in mm with -999 nulls. Returns the 8U result (0 outside the band).
    cv::Mat Process(const cv::Mat& src32f, const SimParams& p, Debug& dbg);
}

namespace DepthProcessorExp
{
    double  computePercentileMasked(const cv::Mat& img, double pct, const cv::Mat& valid_mask);
    cv::Mat patchBasedMedianMasked(const cv::Mat& data, const cv::Mat& null_mask, const cv::Size& patch_size, float overlap);
    void    fillSmallHoles(cv::Mat& src32f, int maxArea);
    // Same as the site removeStageFromRawData but the erosion runs on its own buffer (mask stays intact),
    // so dilate(largest) AND original mask really restores the border.
    void    removeStageRestored(cv::Mat& src32f, int stagePosMode, int breakKernel);
}
