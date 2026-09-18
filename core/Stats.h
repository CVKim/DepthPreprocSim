#pragma once
// Statistics, reference comparison and hand-written JSON serialization.

#include "DepthPreprocCore.h"
#include <string>
#include <vector>

struct RefStats
{
    bool present = false;
    std::string path_utf8;
    double exact_pct = 0, within1_pct = 0, within2_pct = 0;
    int max_abs = 0;
    long long mismatch_count = 0;
    double black_mask_iou = 0;
    long long ref_black_not_result = 0, result_black_not_ref = 0;
};

struct RunStats
{
    long long null_count = 0;           double null_pct = 0;
    long long stage_removed_count = 0;  double stage_removed_pct = 0;
    long long valid_after_stage = 0;
    int valid_components = 0;           double largest_component_pct_of_valid = 0;
    int first_valid_row = -1, last_valid_row = -1;
    double z_min = 0, z_max = 0, unit_mm_per_scaled_unit = 0;
    double clip_low = 0, clip_high = 0, low_mm = 0, high_mm = 0, norm_min = 0, norm_max = 0, zero_diff_pct = 0;
    long long black_count = 0;          double black_pct = 0;
    long long black_in_valid = 0;       double black_in_valid_pct = 0;
    long long white_in_valid = 0;       double white_in_valid_pct = 0;
    static const int kRowBins = 20;
    std::vector<double> black_pct_by_row_bin, white_pct_by_row_bin;
    static const int kDiffBins = 256;
    std::vector<long long> diff_hist;   // over [-1000,1000], valid pixels only, clamped into edge bins
    RefStats ref;
};

RunStats ComputeRunStats(const cv::Mat& raw32f, const SimResult& r);
// Both 8U, same size. Returns false on size/type mismatch.
bool CompareWithRef(const cv::Mat& result8u, const cv::Mat& ref8u, RefStats& out);

struct StatsContext
{
    std::string tool_version;
    std::string input_path_utf8;
    std::string out_dir_utf8;       // resolved output folder (absolute)
    int width = 0, height = 0;
    std::string dtype;
    std::string type;               // display only
    SimParams params;
    const SimResult* result = nullptr;
    const RunStats* stats = nullptr;
    double read_ms = 0;
    double px_x_um = 300, px_y_um = 100;
    bool px_specified = false;
};

std::string BuildStatsJson(const StatsContext& ctx);

// JSON helpers (shared with the CLI).
std::string JsonEscape(const std::string& s);
std::string JsonNum(double v);
std::string JsonNum(long long v);
std::string JsonNum(int v);
