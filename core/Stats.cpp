#include "Stats.h"
#include "IniReader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    double Pct(long long a, long long b) { return b > 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; }
}

RunStats ComputeRunStats(const cv::Mat& raw32f, const SimResult& r)
{
    RunStats s;
    const long long total = static_cast<long long>(raw32f.total());
    const cv::Rect roi = r.roi_rect;
    const long long roiTotal = static_cast<long long>(roi.area());

    s.null_count = cv::countNonZero(r.null_mask_before);
    s.null_pct = Pct(s.null_count, total);
    s.stage_removed_count = cv::countNonZero(r.stage_removed_mask);
    s.stage_removed_pct = Pct(s.stage_removed_count, roiTotal);

    cv::Mat validAfter;
    cv::compare(r.null_mask_after, 0, validAfter, cv::CMP_EQ);
    s.valid_after_stage = cv::countNonZero(validAfter);

    // Components of the valid mask before stage removal (inside the ROI), as removeStage sees them.
    {
        cv::Mat validBefore;
        cv::compare(r.null_mask_before(roi), 0, validBefore, cv::CMP_EQ);
        const long long validBeforeCount = cv::countNonZero(validBefore);
        if (validBeforeCount > 0)
        {
            cv::Mat labels, stats, centroids;
            const int n = cv::connectedComponentsWithStats(validBefore, labels, stats, centroids, 8, CV_32S);
            s.valid_components = n - 1;
            int maxArea = 0;
            for (int i = 1; i < n; ++i) maxArea = std::max(maxArea, stats.at<int>(i, cv::CC_STAT_AREA));
            s.largest_component_pct_of_valid = Pct(maxArea, validBeforeCount);
        }
    }

    // Valid row range after stage removal.
    std::vector<long long> validPerRow(validAfter.rows, 0);
    for (int y = 0; y < validAfter.rows; ++y)
    {
        validPerRow[y] = cv::countNonZero(validAfter.row(y));
        if (validPerRow[y] > 0) { if (s.first_valid_row < 0) s.first_valid_row = y; s.last_valid_row = y; }
    }

    s.z_min = r.z_min; s.z_max = r.z_max;
    s.unit_mm_per_scaled_unit = r.unit_mm_override > 0 ? r.unit_mm_override : (r.z_max - r.z_min) / 65536.0;
    s.clip_low = r.clip_low; s.clip_high = r.clip_high;
    s.low_mm = r.clip_low * s.unit_mm_per_scaled_unit;
    s.high_mm = r.clip_high * s.unit_mm_per_scaled_unit;
    s.norm_min = r.norm_min; s.norm_max = r.norm_max;

    // diff histogram and zero-diff share over valid pixels.
    s.diff_hist.assign(RunStats::kDiffBins, 0);
    long long zeroDiff = 0;
    if (!r.diff_f32.empty())
    {
        for (int y = 0; y < r.diff_f32.rows; ++y)
        {
            const float* d = r.diff_f32.ptr<float>(y);
            const uchar* v = validAfter.ptr<uchar>(y);
            for (int x = 0; x < r.diff_f32.cols; ++x)
            {
                if (!v[x]) continue;
                if (d[x] == 0.f) ++zeroDiff;
                int bin = static_cast<int>(std::floor((static_cast<double>(d[x]) + 1000.0) / 2000.0 * RunStats::kDiffBins));
                bin = std::min(std::max(bin, 0), RunStats::kDiffBins - 1);
                ++s.diff_hist[bin];
            }
        }
    }
    s.zero_diff_pct = Pct(zeroDiff, s.valid_after_stage);

    // Output blacks/whites.
    cv::Mat black, white;
    cv::compare(r.result8u, 0, black, cv::CMP_EQ);
    cv::compare(r.result8u, 255, white, cv::CMP_EQ);
    s.black_count = cv::countNonZero(black);
    s.black_pct = Pct(s.black_count, total);
    cv::Mat blackValid, whiteValid;
    cv::bitwise_and(black, validAfter, blackValid);
    cv::bitwise_and(white, validAfter, whiteValid);
    s.black_in_valid = cv::countNonZero(blackValid);
    s.white_in_valid = cv::countNonZero(whiteValid);
    s.black_in_valid_pct = Pct(s.black_in_valid, s.valid_after_stage);
    s.white_in_valid_pct = Pct(s.white_in_valid, s.valid_after_stage);

    s.black_pct_by_row_bin.assign(RunStats::kRowBins, 0.0);
    s.white_pct_by_row_bin.assign(RunStats::kRowBins, 0.0);
    if (s.first_valid_row >= 0)
    {
        const int span = s.last_valid_row - s.first_valid_row + 1;
        std::vector<long long> vb(RunStats::kRowBins, 0), bb(RunStats::kRowBins, 0), wb(RunStats::kRowBins, 0);
        for (int y = s.first_valid_row; y <= s.last_valid_row; ++y)
        {
            int bin = static_cast<int>(static_cast<long long>(y - s.first_valid_row) * RunStats::kRowBins / span);
            bin = std::min(bin, RunStats::kRowBins - 1);
            vb[bin] += validPerRow[y];
            bb[bin] += cv::countNonZero(blackValid.row(y));
            wb[bin] += cv::countNonZero(whiteValid.row(y));
        }
        for (int b = 0; b < RunStats::kRowBins; ++b)
        {
            s.black_pct_by_row_bin[b] = Pct(bb[b], vb[b]);
            s.white_pct_by_row_bin[b] = Pct(wb[b], vb[b]);
        }
    }
    return s;
}

bool CompareWithRef(const cv::Mat& result8u, const cv::Mat& ref8u, RefStats& out)
{
    if (result8u.empty() || ref8u.empty() || result8u.size() != ref8u.size() || result8u.type() != CV_8UC1 || ref8u.type() != CV_8UC1)
        return false;
    const long long total = static_cast<long long>(result8u.total());
    cv::Mat ad;
    cv::absdiff(result8u, ref8u, ad);
    double mx = 0;
    cv::minMaxLoc(ad, nullptr, &mx);
    out.max_abs = static_cast<int>(mx);
    const long long exact = cv::countNonZero(ad == 0);
    const long long w1 = cv::countNonZero(ad <= 1);
    const long long w2 = cv::countNonZero(ad <= 2);
    out.exact_pct = Pct(exact, total);
    out.within1_pct = Pct(w1, total);
    out.within2_pct = Pct(w2, total);
    out.mismatch_count = total - exact;

    cv::Mat rb = result8u == 0, fb = ref8u == 0, inter, uni;
    cv::bitwise_and(rb, fb, inter);
    cv::bitwise_or(rb, fb, uni);
    const long long ni = cv::countNonZero(inter), nu = cv::countNonZero(uni);
    out.black_mask_iou = nu > 0 ? static_cast<double>(ni) / static_cast<double>(nu) : 1.0;
    out.ref_black_not_result = cv::countNonZero(fb & ~rb);
    out.result_black_not_ref = cv::countNonZero(rb & ~fb);
    out.present = true;
    return true;
}

std::string JsonEscape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += static_cast<char>(c);
        }
    }
    return o;
}

std::string JsonNum(double v)
{
    if (!std::isfinite(v)) return "null";
    char b[64];
    snprintf(b, sizeof(b), "%.15g", v);
    return b;
}
std::string JsonNum(long long v) { return std::to_string(v); }
std::string JsonNum(int v) { return std::to_string(v); }

namespace
{
    std::string Q(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }
    std::string B(bool b) { return b ? "true" : "false"; }
    template <class T> std::string Arr(const std::vector<T>& v)
    {
        std::string o = "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += JsonNum(v[i]); }
        return o + "]";
    }
}

std::string BuildStatsJson(const StatsContext& c)
{
    const SimParams& p = c.params;
    const SimResult& r = *c.result;
    const RunStats& s = *c.stats;
    std::string o;
    o.reserve(8192);
    o += "{\n";
    o += " \"tool_version\": " + Q(c.tool_version) + ",\n";
    o += " \"input\": {\"path\": " + Q(c.input_path_utf8) + ", \"width\": " + JsonNum(c.width) + ", \"height\": " + JsonNum(c.height) + ", \"dtype\": " + Q(c.dtype) + "},\n";
    o += " \"out_dir\": " + Q(c.out_dir_utf8) + ",\n";

    o += " \"params_effective\": {\"patch_w\": " + JsonNum(p.patch_w) + ", \"patch_h\": " + JsonNum(p.patch_h)
        + ", \"overlap\": " + JsonNum(p.overlap) + ", \"step_w\": " + JsonNum(r.step_w) + ", \"step_h\": " + JsonNum(r.step_h)
        + ", \"lower\": " + JsonNum(p.lower) + ", \"upper\": " + JsonNum(p.upper)
        + ", \"stage\": " + Q(StageToString(p.stage)) + ", \"roi\": ";
    if (p.UseRoi()) o += "[" + JsonNum(p.roi[0]) + "," + JsonNum(p.roi[1]) + "," + JsonNum(p.roi[2]) + "," + JsonNum(p.roi[3]) + "]";
    else o += "null";
    o += ", \"type\": " + Q(c.type) + ", \"break_kernel\": " + JsonNum(p.break_kernel);
    o += ", \"exp\": {\"use_ini_pct\": " + B(p.exp.use_ini_pct) + ", \"valid_pct\": " + B(p.exp.valid_pct)
        + ", \"masked_median\": " + B(p.exp.masked_median) + ", \"null_value\": " + JsonNum(p.exp.null_value)
        + ", \"fill_holes\": " + JsonNum(p.exp.fill_holes) + ", \"stage_restore\": " + B(p.exp.stage_restore)
        + ", \"abs_mm\": " + JsonNum(p.exp.abs_mm)
        + ", \"v2\": {\"on\": " + B(p.exp.v2.on) + ", \"range_mm\": " + JsonNum(p.exp.v2.range_mm) + ", \"edge\": " + JsonNum(p.exp.v2.edge)
        + ", \"slope\": " + B(p.exp.v2.slope) + ", \"fill\": " + B(p.exp.v2.fill) + ", \"env_median\": " + JsonNum(p.exp.v2.env_median) + ", \"mode\": " + JsonNum(p.exp.v2.mode)
        + ", \"zone_thr\": " + JsonNum(p.exp.v2.zone_thr) + ", \"zone_out_mm\": " + JsonNum(p.exp.v2.zout_mm) + ", \"zone_out2_mm\": " + JsonNum(p.exp.v2.zout2_mm)
        + ", \"rm_win\": " + JsonNum(p.exp.v2.rm_win) + "}}";
    o += ", \"dll_identical\": " + B(r.dll_identical) + ", \"capture_identical\": " + B(r.capture_identical)
        + ", \"v2_zone_pct\": " + JsonNum(r.v2_zone_pct) + ", \"v2_rejected_pct\": " + JsonNum(r.v2_rejected_pct) + ", \"v2_filled_pct\": " + JsonNum(r.v2_filled_pct) + "},\n";

    const SimTiming& t = r.timing;
    o += " \"timing_ms\": {\"read\": " + JsonNum(c.read_ms) + ", \"remove_stage\": " + JsonNum(t.remove_stage) + ", \"scale\": " + JsonNum(t.scale)
        + ", \"basis\": " + JsonNum(t.basis) + ", \"diff\": " + JsonNum(t.diff) + ", \"clip_normalize\": " + JsonNum(t.clip_normalize)
        + ", \"total\": " + JsonNum(c.read_ms + t.total) + "},\n";

    o += " \"null\": {\"count\": " + JsonNum(s.null_count) + ", \"pct\": " + JsonNum(s.null_pct)
        + ", \"stage_removed_count\": " + JsonNum(s.stage_removed_count) + ", \"stage_removed_pct\": " + JsonNum(s.stage_removed_pct)
        + ", \"valid_after_stage\": " + JsonNum(s.valid_after_stage) + ", \"valid_components\": " + JsonNum(s.valid_components)
        + ", \"largest_component_pct_of_valid\": " + JsonNum(s.largest_component_pct_of_valid) + "},\n";

    o += " \"valid_rows\": {\"first\": " + JsonNum(s.first_valid_row) + ", \"last\": " + JsonNum(s.last_valid_row) + "},\n";
    o += " \"z\": {\"min\": " + JsonNum(s.z_min) + ", \"max\": " + JsonNum(s.z_max) + ", \"unit_mm_per_scaled_unit\": " + JsonNum(s.unit_mm_per_scaled_unit) + "},\n";
    o += " \"clip\": {\"low\": " + JsonNum(s.clip_low) + ", \"high\": " + JsonNum(s.clip_high) + ", \"low_mm\": " + JsonNum(s.low_mm)
        + ", \"high_mm\": " + JsonNum(s.high_mm) + ", \"norm_min\": " + JsonNum(s.norm_min) + ", \"norm_max\": " + JsonNum(s.norm_max)
        + ", \"zero_diff_pct\": " + JsonNum(s.zero_diff_pct) + "},\n";

    o += " \"output\": {\"black_count\": " + JsonNum(s.black_count) + ", \"black_pct\": " + JsonNum(s.black_pct)
        + ", \"black_in_valid\": " + JsonNum(s.black_in_valid) + ", \"black_in_valid_pct\": " + JsonNum(s.black_in_valid_pct)
        + ", \"white_in_valid\": " + JsonNum(s.white_in_valid) + ", \"white_in_valid_pct\": " + JsonNum(s.white_in_valid_pct)
        + ", \"row_profile_bins\": " + JsonNum(RunStats::kRowBins)
        + ", \"black_pct_by_valid_row_bin\": " + Arr(s.black_pct_by_row_bin)
        + ", \"white_pct_by_valid_row_bin\": " + Arr(s.white_pct_by_row_bin) + "},\n";

    o += " \"diff_hist\": {\"min\": -1000, \"max\": 1000, \"bins\": " + JsonNum(RunStats::kDiffBins) + ", \"counts\": " + Arr(s.diff_hist) + ", \"valid_only\": true},\n";

    o += " \"pixel\": {\"px_x_um\": " + JsonNum(c.px_x_um) + ", \"px_y_um\": " + JsonNum(c.px_y_um) + ", \"specified\": " + B(c.px_specified)
        + ", \"patch_w_mm\": " + JsonNum(p.patch_w * c.px_x_um / 1000.0) + ", \"patch_h_mm\": " + JsonNum(p.patch_h * c.px_y_um / 1000.0)
        + ", \"step_w_mm\": " + JsonNum(r.step_w * c.px_x_um / 1000.0) + ", \"step_h_mm\": " + JsonNum(r.step_h * c.px_y_um / 1000.0) + "},\n";

    if (s.ref.present)
    {
        const RefStats& f = s.ref;
        o += " \"ref\": {\"path\": " + Q(f.path_utf8) + ", \"exact_pct\": " + JsonNum(f.exact_pct) + ", \"within1_pct\": " + JsonNum(f.within1_pct)
            + ", \"within2_pct\": " + JsonNum(f.within2_pct) + ", \"max_abs\": " + JsonNum(f.max_abs) + ", \"mismatch_count\": " + JsonNum(f.mismatch_count)
            + ", \"black_mask_iou\": " + JsonNum(f.black_mask_iou) + ", \"ref_black_not_result\": " + JsonNum(f.ref_black_not_result)
            + ", \"result_black_not_ref\": " + JsonNum(f.result_black_not_ref) + "}\n";
    }
    else
        o += " \"ref\": null\n";
    o += "}\n";
    return o;
}
