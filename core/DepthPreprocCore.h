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

struct SimExp
{
    bool use_ini_pct = false;
    bool valid_pct = false;
    bool masked_median = false;
    int  null_value = -1;   // -1 = off (null stays 0 like the DLL)
    int  fill_holes = 0;    // 0 = off, else max component area in px
    bool Any() const { return use_ini_pct || valid_pct || masked_median || null_value >= 0 || fill_holes > 0; }
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

namespace DepthProcessorExp
{
    double  computePercentileMasked(const cv::Mat& img, double pct, const cv::Mat& valid_mask);
    cv::Mat patchBasedMedianMasked(const cv::Mat& data, const cv::Mat& null_mask, const cv::Size& patch_size, float overlap);
    void    fillSmallHoles(cv::Mat& src32f, int maxArea);
}
