// DepthPreprocCore.cpp
//
// Section 1 (namespace DepthProcessor) is copied VERBATIM from the SITE (PC3) source of
//   alg_depth_preproc\3dDepthProcessing.cpp, 2026-07-08 16:55, rc 1.0.2.0.38cf930_HT
//   (copy kept in reference/site_pc3_20260708/; origin H:\000. PJT\01. HankookTire\04_코드\PC3_Platform).
//   Site line ranges: 11-65 (removeStageFromRawData with breakKernel), 67-111 (removeStageFromRawDataBead),
//   388-503 (computePercentile, postClipNormalize, scaleTo16bitIgnoreNull, patchBasedMedian),
//   590-633 (processHighCurvature).
// Relation to talos-platform commit 0a6814f2 (feat/ALG-288-stage-ignore-preproc): identical except that the
//   site adds the erosion/dilation variant of removeStageFromRawData, keeps the old one as
//   removeStageFromRawDataBead, applies them per type in INSPECT (INNERCENTER none, BEAD bead variant,
//   INSHOULDER breakKernel 3) and has the debug MbufSave commented out.
// Dead code that is not on the INSPECT path was omitted on purpose:
//   incenter_processing, shoulder_processing, polynomialFitting, processInnerCenter.
// Do not edit section 1; bit-identity with the DLL depends on it.

#include "DepthPreprocCore.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

// ===== Section 1: verbatim DLL code ==========================================
namespace DepthProcessor
{
    void removeStageFromRawData(cv::Mat& src32f, int stagePosMode = 0, int breakKernel = 0) {
        if (src32f.empty() || stagePosMode == 3) return;

        int h = src32f.rows;
        int w = src32f.cols;

        // 1) 유효 픽셀 마스크 (기존과 동일)
        cv::Mat mask = cv::Mat::zeros(h, w, CV_8UC1);
        for (int r = 0; r < h; ++r) {
            const float* ptr = src32f.ptr<float>(r);
            uchar* mptr = mask.ptr<uchar>(r);
            for (int c = 0; c < w; ++c)
                if (ptr[c] > -900.0f) mptr[c] = 1;
        }

        // 2) 라벨링 전에 얇은 목 끊기 (opening의 erode 단계)
        //    breakKernel 은 '다리 폭보다 큰 홀수' 로. 다리 7px -> 9 권장.
        //    breakKernel <= 0 이면 기존 동작(그대로) 유지.
        cv::Mat labelMask = mask;
        cv::Mat kernel;
        if (breakKernel >= 3) {
            kernel = cv::getStructuringElement(cv::MORPH_RECT,
                cv::Size(breakKernel, breakKernel));
            cv::erode(mask, labelMask, kernel);   // 침식 -> 목 소멸, 덩어리 분리
        }

        // 3) 연결요소 라벨링
        cv::Mat labels, stats, centroids;
        int num_labels = cv::connectedComponentsWithStats(
            labelMask, labels, stats, centroids, 8, CV_32S);
        if (num_labels <= 1) return;

        // 4) 가장 큰 덩어리 선택 (기존과 동일)
        int max_area = 0, best_label = -1;
        for (int i = 1; i < num_labels; ++i) {
            int area = stats.at<int>(i, cv::CC_STAT_AREA);
            if (area > max_area) { max_area = area; best_label = i; }
        }
        if (best_label == -1) return;

        // 5) 선택 덩어리를 침식한 만큼 다시 팽창(원래 크기 복원) 후 원본 마스크와 교집합
        cv::Mat keep = (labels == best_label);     // CV_8U, 0/255
        if (breakKernel >= 3) {
            cv::dilate(keep, keep, kernel);        // 줄어든 경계 복원
            cv::bitwise_and(keep, mask, keep);     // 원래 유효영역 밖으로는 안 나가게
        }

        // 6) keep 밖의 유효 픽셀은 ignore(-999)
        for (int r = 0; r < h; ++r) {
            float* ptr = src32f.ptr<float>(r);
            const uchar* kptr = keep.ptr<uchar>(r);
            for (int c = 0; c < w; ++c)
                if (kptr[c] == 0) ptr[c] = -999.0f;
        }
    }

    void removeStageFromRawDataBead(cv::Mat& src32f, int stagePosMode = 0) {
        if (src32f.empty() || stagePosMode == 3) return;

        int h = src32f.rows;
        int w = src32f.cols;

        cv::Mat mask = cv::Mat::zeros(h, w, CV_8UC1);
        for (int r = 0; r < h; ++r) {
            float* ptr = src32f.ptr<float>(r);
            uchar* mptr = mask.ptr<uchar>(r);
            for (int c = 0; c < w; ++c) {
                if (ptr[c] > -900.0f) {
                    mptr[c] = 1;
                }
            }
        }

        cv::Mat labels, stats, centroids;
        int num_labels = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8, CV_32S);

        if (num_labels <= 1) return;

        int max_area = 0;
        int best_label = -1;

        for (int i = 1; i < num_labels; ++i) {
            int area = stats.at<int>(i, cv::CC_STAT_AREA);
            if (area > max_area) {
                max_area = area;
                best_label = i;
            }
        }

        if (best_label != -1) {
            for (int r = 0; r < h; ++r) {
                float* ptr = src32f.ptr<float>(r);
                int* lptr = labels.ptr<int>(r);
                for (int c = 0; c < w; ++c) {
                    if (lptr[c] != best_label) {
                        ptr[c] = -999.0f;
                    }
                }
            }
        }
    }

    double computePercentile(const cv::Mat& img, double pct)
    {
        std::vector<float> vals;
        vals.assign((float*)img.datastart, (float*)img.dataend);
        if (vals.empty())
            return 0.0;
        sort(vals.begin(), vals.end());
        double pos = (pct / 100.0) * (vals.size() - 1);
        size_t idx_low = static_cast<size_t>(floor(pos));
        size_t idx_high = static_cast<size_t>(ceil(pos));
        double fraction = pos - idx_low;
        double low_val = vals[idx_low];
        double high_val = vals[idx_high];
        return low_val + fraction * (high_val - low_val);
    }

    cv::Mat postClipNormalize(const cv::Mat& img, double lower_pct = 5, double upper_pct = 95, double abs_limit = 1000)
    {
        double low = computePercentile(img, lower_pct);
        double high = computePercentile(img, upper_pct);

        low = std::max(low, -abs_limit);
        high = std::min(high, abs_limit);

        cv::Mat clipped = img.clone();
        clipped.setTo(low, clipped < low);
        clipped.setTo(high, clipped > high);

        cv::Mat normed;
        normalize(clipped, normed, 0, 255, cv::NORM_MINMAX);
        normed.convertTo(normed, CV_8U);

        return normed;
    }

    cv::Mat scaleTo16bitIgnoreNull(const cv::Mat& data, const cv::Mat& null_mask) {
        CV_Assert(data.type() == CV_32F);
        cv::Mat valid_mask;
        compare(null_mask, 0, valid_mask, cv::CMP_EQ);
        if (countNonZero(valid_mask) == 0)
            return cv::Mat::zeros(data.size(), data.type());
        double minVal, maxVal;
        minMaxLoc(data, &minVal, &maxVal, nullptr, nullptr, valid_mask);
        cv::Mat scaled;
        if (maxVal == minVal) {
            scaled = cv::Mat::zeros(data.size(), data.type());
        }
        else {
            scaled = (data - minVal) / (maxVal - minVal) * 65536.0;
        }
        scaled.setTo(0, null_mask);
        return scaled;
    }

    cv::Mat patchBasedMedian(const cv::Mat& data, const cv::Size& patch_size, float overlap = 0.1f)
    {
        CV_Assert(data.type() == CV_32F);
        int h = data.rows;
        int w = data.cols;

        int patch_h = patch_size.height;
        int patch_w = patch_size.width;

        int step_h = static_cast<int>(patch_h * (1.0f - overlap));
        int step_w = static_cast<int>(patch_w * (1.0f - overlap));

        if (step_h < 1) step_h = 1;
        if (step_w < 1) step_w = 1;

        int out_h = (h - patch_h) / step_h + 1;
        int out_w = (w - patch_w) / step_w + 1;

        cv::Mat sampled(out_h, out_w, CV_32F, cv::Scalar(0));

        for (int i = 0; i < out_h; i++)
        {
            for (int j = 0; j < out_w; j++)
            {
                int r0 = i * step_h;
                int c0 = j * step_w;

                std::vector<float> vals;
                vals.reserve(patch_h * patch_w);

                for (int rr = 0; rr < patch_h; rr++)
                {
                    const float* rowPtr = data.ptr<float>(r0 + rr) + c0;
                    for (int cc = 0; cc < patch_w; cc++)
                    {
                        vals.push_back(rowPtr[cc]);
                    }
                }

                std::sort(vals.begin(), vals.end());
                size_t n = vals.size();
                float medianVal;
                if (n % 2 == 0)
                {
                    float v1 = vals[n / 2 - 1];
                    float v2 = vals[n / 2];
                    medianVal = 0.5f * (v1 + v2);
                }
                else
                {
                    medianVal = vals[n / 2];
                }

                sampled.at<float>(i, j) = medianVal;
            }
        }

        cv::Mat basis_plane;
        cv::resize(sampled, basis_plane, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);

        return basis_plane;
    }

    cv::Mat processHighCurvature(const cv::Mat& data, cv::Size patch_size, double overlap)
    {
        CV_Assert(data.type() == CV_32F);
        cv::Mat null_mask;
        compare(data, -999, null_mask, cv::CMP_EQ);

        cv::Mat scaled = scaleTo16bitIgnoreNull(data, null_mask);

        cv::Mat basis_plane = patchBasedMedian(scaled, patch_size, overlap);

        cv::Mat diff_f32;
        cv::subtract(scaled, basis_plane, diff_f32, cv::noArray(), CV_32F);

        cv::Mat diff_int16;
        diff_int16.create(diff_f32.size(), CV_16S);

        const int rows = diff_f32.rows;
        const int cols = diff_f32.cols;

        for (int i = 0; i < rows; i++)
        {
            const float* inPtr = diff_f32.ptr<float>(i);
            short* outPtr = diff_int16.ptr<short>(i);

            for (int j = 0; j < cols; j++)
            {
                double val = static_cast<double>(inPtr[j]);

                if (val < -32768.0)
                    val = -32768.0;
                else if (val > 32767.0)
                    val = 32767.0;

                outPtr[j] = static_cast<short>(val);
            }
        }

        cv::Mat diff_float;
        diff_int16.convertTo(diff_float, CV_32F);

        cv::Mat anomaly = postClipNormalize(diff_float, 5, 95);
        anomaly.setTo(0, null_mask);
        return anomaly;
    }
} // end namespace DepthProcessor
// ===== End of verbatim section ===============================================

namespace
{
    using Clock = std::chrono::steady_clock;
    double MsSince(Clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }

    struct Intermediates
    {
        cv::Mat null_mask, scaled, basis, diff_float, clipped;
        double low = 0, high = 0, nmin = 0, nmax = 0, zmin = 0, zmax = 0;
    };

    void ValidMinMax(const cv::Mat& data, const cv::Mat& null_mask, double& mn, double& mx)
    {
        cv::Mat valid_mask;
        cv::compare(null_mask, 0, valid_mask, cv::CMP_EQ);
        mn = mx = 0;
        if (cv::countNonZero(valid_mask) > 0)
            cv::minMaxLoc(data, &mn, &mx, nullptr, nullptr, valid_mask);
    }

    // Shared tail of processHighCurvature: subtract -> int16 clamp -> float.
    cv::Mat DiffInt16AsFloat(const cv::Mat& scaled, const cv::Mat& basis_plane)
    {
        cv::Mat diff_f32;
        cv::subtract(scaled, basis_plane, diff_f32, cv::noArray(), CV_32F);

        cv::Mat diff_int16;
        diff_int16.create(diff_f32.size(), CV_16S);
        const int rows = diff_f32.rows;
        const int cols = diff_f32.cols;
        for (int i = 0; i < rows; i++)
        {
            const float* inPtr = diff_f32.ptr<float>(i);
            short* outPtr = diff_int16.ptr<short>(i);
            for (int j = 0; j < cols; j++)
            {
                double val = static_cast<double>(inPtr[j]);
                if (val < -32768.0)
                    val = -32768.0;
                else if (val > 32767.0)
                    val = 32767.0;
                outPtr[j] = static_cast<short>(val);
            }
        }
        cv::Mat diff_float;
        diff_int16.convertTo(diff_float, CV_32F);
        return diff_float;
    }

    // Expanded postClipNormalize body with low/high already computed.
    cv::Mat ClipNormalize(const cv::Mat& img, double low, double high, double abs_limit, Intermediates& im)
    {
        low = std::max(low, -abs_limit);
        high = std::min(high, abs_limit);

        cv::Mat clipped = img.clone();
        clipped.setTo(low, clipped < low);
        clipped.setTo(high, clipped > high);

        cv::Mat normed;
        normalize(clipped, normed, 0, 255, cv::NORM_MINMAX);
        normed.convertTo(normed, CV_8U);

        im.low = low;
        im.high = high;
        im.clipped = clipped;
        cv::minMaxLoc(clipped, &im.nmin, &im.nmax);
        return normed;
    }

    // Capture-only replica of DepthProcessor::processHighCurvature: same public
    // sub-functions, same order, same arguments. The official result is always
    // produced by the verbatim function; this one only exposes intermediates.
    cv::Mat CapturePipeline(const cv::Mat& data, const SimParams& p, Intermediates& im, SimTiming& t)
    {
        const cv::Size patch_size(p.patch_w, p.patch_h);
        const double overlap = p.overlap;   // narrowed to float inside patchBasedMedian, as in the DLL

        CV_Assert(data.type() == CV_32F);
        cv::Mat null_mask;
        compare(data, -999, null_mask, cv::CMP_EQ);

        auto t0 = Clock::now();
        cv::Mat scaled = DepthProcessor::scaleTo16bitIgnoreNull(data, null_mask);
        t.scale = MsSince(t0);

        t0 = Clock::now();
        cv::Mat basis_plane = DepthProcessor::patchBasedMedian(scaled, patch_size, overlap);
        t.basis = MsSince(t0);

        t0 = Clock::now();
        cv::Mat diff_float = DiffInt16AsFloat(scaled, basis_plane);
        t.diff = MsSince(t0);

        t0 = Clock::now();
        double low = DepthProcessor::computePercentile(diff_float, 5);
        double high = DepthProcessor::computePercentile(diff_float, 95);
        cv::Mat anomaly = ClipNormalize(diff_float, low, high, 1000, im);
        anomaly.setTo(0, null_mask);
        t.clip_normalize = MsSince(t0);

        im.null_mask = null_mask;
        im.scaled = scaled;
        im.basis = basis_plane;
        im.diff_float = diff_float;
        ValidMinMax(data, null_mask, im.zmin, im.zmax);
        return anomaly;
    }

    // Experimental pipeline: every deviation from the DLL is behind a SimExp flag.
    cv::Mat ExpPipeline(const cv::Mat& data, const SimParams& p, Intermediates& im, SimTiming& t)
    {
        const cv::Size patch_size(p.patch_w, p.patch_h);
        const double overlap = p.overlap;

        CV_Assert(data.type() == CV_32F);
        cv::Mat null_mask;
        compare(data, -999, null_mask, cv::CMP_EQ);
        cv::Mat valid_mask;
        cv::compare(null_mask, 0, valid_mask, cv::CMP_EQ);

        auto t0 = Clock::now();
        cv::Mat scaled = DepthProcessor::scaleTo16bitIgnoreNull(data, null_mask);
        t.scale = MsSince(t0);

        t0 = Clock::now();
        cv::Mat basis_plane = p.exp.masked_median
            ? DepthProcessorExp::patchBasedMedianMasked(scaled, null_mask, patch_size, static_cast<float>(overlap))
            : DepthProcessor::patchBasedMedian(scaled, patch_size, overlap);
        t.basis = MsSince(t0);

        t0 = Clock::now();
        cv::Mat diff_float = DiffInt16AsFloat(scaled, basis_plane);
        t.diff = MsSince(t0);

        t0 = Clock::now();
        const double lower = p.exp.use_ini_pct ? p.lower : 5.0;
        const double upper = p.exp.use_ini_pct ? p.upper : 95.0;
        double low, high;
        if (p.exp.valid_pct)
        {
            low = DepthProcessorExp::computePercentileMasked(diff_float, lower, valid_mask);
            high = DepthProcessorExp::computePercentileMasked(diff_float, upper, valid_mask);
        }
        else
        {
            low = DepthProcessor::computePercentile(diff_float, lower);
            high = DepthProcessor::computePercentile(diff_float, upper);
        }
        cv::Mat anomaly;
        if (p.exp.abs_mm > 0)
        {
            // Fixed physical scale: 1 scaled unit = (zmax - zmin) / 65536 mm. Map +-abs_mm to 1..255 with 128 = basis plane;
            // 0 stays reserved for null so holes and deep valleys are distinguishable.
            double zmn = 0, zmx = 0;
            ValidMinMax(data, null_mask, zmn, zmx);
            const double unit_mm = (zmx > zmn) ? (zmx - zmn) / 65536.0 : 0.0;
            const double lim = (unit_mm > 0) ? p.exp.abs_mm / unit_mm : 1.0;
            cv::Mat clipped = diff_float.clone();
            clipped.setTo(-lim, clipped < -lim);
            clipped.setTo(lim, clipped > lim);
            clipped.convertTo(anomaly, CV_8U, 127.0 / lim, 128.0);
            anomaly.setTo(1, anomaly < 1);
            im.low = -lim; im.high = lim; im.clipped = clipped; im.nmin = -lim; im.nmax = lim;
        }
        else
        {
            anomaly = ClipNormalize(diff_float, low, high, 1000, im);
        }
        anomaly.setTo(0, null_mask);
        if (p.exp.null_value >= 0)
            anomaly.setTo(std::min(p.exp.null_value, 255), null_mask);
        t.clip_normalize = MsSince(t0);

        im.null_mask = null_mask;
        im.scaled = scaled;
        im.basis = basis_plane;
        im.diff_float = diff_float;
        ValidMinMax(data, null_mask, im.zmin, im.zmax);
        return anomaly;
    }

    cv::Mat PlaceFull(const cv::Mat& roiMat, cv::Size full, cv::Rect rect, bool useRoi, const cv::Scalar& fill = cv::Scalar::all(0))
    {
        if (!useRoi || roiMat.empty()) return roiMat;
        cv::Mat out(full, roiMat.type(), fill);
        roiMat.copyTo(out(rect));
        return out;
    }
} // namespace

int SimParams::StepW() const
{
    int s = static_cast<int>(patch_w * (1.0f - static_cast<float>(overlap)));
    return s < 1 ? 1 : s;
}

int SimParams::StepH() const
{
    int s = static_cast<int>(patch_h * (1.0f - static_cast<float>(overlap)));
    return s < 1 ? 1 : s;
}

namespace SimPipeline
{
    void Run(const cv::Mat& src32f, const SimParams& p, SimResult& r)
    {
        if (src32f.empty() || src32f.type() != CV_32FC1)
            throw std::runtime_error("input must be a non-empty CV_32FC1 image");

        r = SimResult();
        r.original_size = src32f.size();
        r.step_w = p.StepW();
        r.step_h = p.StepH();

        // --- INSPECT: ROI view (roi & imageRect) ---
        cv::Mat work = src32f.clone();
        cv::Rect roi(0, 0, 0, 0);
        r.use_roi = p.UseRoi();
        cv::Mat view = work;
        if (r.use_roi)
        {
            roi = cv::Rect(p.roi[0], p.roi[1], p.roi[2] - p.roi[0], p.roi[3] - p.roi[1]);
            cv::Rect clipped = roi & cv::Rect(0, 0, work.cols, work.rows);
            if (clipped.area() <= 0)
                throw std::runtime_error("ROI does not intersect the image");
            view = work(clipped);
            r.roi_rect = clipped;
        }
        else
        {
            r.roi_rect = cv::Rect(0, 0, work.cols, work.rows);
        }

        // --- experimental algorithm v2 replaces the whole pipeline (never DLL-identical) ---
        if (p.exp.v2.on)
        {
            auto tv = Clock::now();
            DepthProcessorV2::Debug dbg;
            cv::Mat out8 = DepthProcessorV2::Process(view, p, dbg);
            r.timing.clip_normalize = MsSince(tv);
            r.timing.total = r.timing.clip_normalize;
            cv::Mat finalImage = out8;
            if (r.use_roi)
            {
                if (roi != r.roi_rect)
                    throw std::runtime_error("ROI exceeds the image; the DLL fails at result8u.copyTo(finalImage(roi))");
                finalImage = cv::Mat::zeros(r.original_size, CV_8UC1);
                out8.copyTo(finalImage(roi));
            }
            r.result8u = finalImage;
            compare(src32f, -999, r.null_mask_before, cv::CMP_EQ);
            r.src_after_stage = work;
            cv::Mat outside = ~dbg.band;
            r.null_mask_after = PlaceFull(outside, r.original_size, r.roi_rect, r.use_roi, cv::Scalar::all(255));
            r.stage_removed_mask = PlaceFull(dbg.stage_removed, r.original_size, r.roi_rect, r.use_roi);
            r.band_mask = PlaceFull(dbg.band, r.original_size, r.roi_rect, r.use_roi);
            r.hole_mask = PlaceFull(dbg.holes, r.original_size, r.roi_rect, r.use_roi);
            if (!dbg.zone.empty())
            {
                r.zone_mask = PlaceFull(dbg.zone, r.original_size, r.roi_rect, r.use_roi);
                r.rejected_mask = PlaceFull(dbg.rejected, r.original_size, r.roi_rect, r.use_roi);
                r.surface = PlaceFull(dbg.surface, r.original_size, r.roi_rect, r.use_roi);
                r.v2_zone_pct = dbg.zone_pct; r.v2_rejected_pct = dbg.rejected_pct; r.v2_filled_pct = dbg.filled_pct;
            }
            r.scaled = PlaceFull(dbg.zf, r.original_size, r.roi_rect, r.use_roi);
            r.basis = PlaceFull(dbg.basis, r.original_size, r.roi_rect, r.use_roi);
            r.diff_f32 = PlaceFull(dbg.rn_um, r.original_size, r.roi_rect, r.use_roi);
            r.clipped = PlaceFull(dbg.clipped_um, r.original_size, r.roi_rect, r.use_roi);
            r.clip_low = dbg.low_mm * 1000.0;
            r.clip_high = dbg.high_mm * 1000.0;
            r.norm_min = r.clip_low; r.norm_max = r.clip_high;
            r.z_min = dbg.z_min; r.z_max = dbg.z_max;
            r.unit_mm_override = 0.001;          // diff / clip are reported in um
            r.capture_identical = true;
            r.dll_identical = false;
            return;
        }

        cv::Mat nullBefore;
        compare(view, -999, nullBefore, cv::CMP_EQ);

        // --- INSPECT (site build): stage removal depends on DepthPreprocType, in place on the view ---
        //   INNERCENTER : none (call commented out at the site)
        //   BEAD        : removeStageFromRawDataBead(src32f, m_nStagePos)
        //   INSHOULDER  : removeStageFromRawData(src32f, m_nStagePos, 3)
        auto t0 = Clock::now();
        switch (p.type)
        {
        case TYPE_INNERCENTER: break;
        case TYPE_BEAD: DepthProcessor::removeStageFromRawDataBead(view, p.stage); break;
        default:
            if (p.exp.stage_restore) DepthProcessorExp::removeStageRestored(view, p.stage, p.break_kernel);
            else DepthProcessor::removeStageFromRawData(view, p.stage, p.break_kernel);
            break;
        }
        r.timing.remove_stage = MsSince(t0);

        if (p.exp.fill_holes > 0)
            DepthProcessorExp::fillSmallHoles(view, p.exp.fill_holes);

        cv::Mat nullAfter;
        compare(view, -999, nullAfter, cv::CMP_EQ);
        cv::Mat stageRemoved;
        cv::bitwise_and(nullAfter, ~nullBefore, stageRemoved);

        // --- INSPECT: processHighCurvature(src32f, m_PatchSize, m_overlap) ---
        Intermediates im;
        cv::Mat result8u;
        if (!p.exp.Any())
        {
            result8u = DepthProcessor::processHighCurvature(view, cv::Size(p.patch_w, p.patch_h), p.overlap);
            cv::Mat captured = CapturePipeline(view, p, im, r.timing);
            r.capture_identical = (captured.size() == result8u.size() && captured.type() == result8u.type()
                && cv::countNonZero(captured != result8u) == 0);
            assert(r.capture_identical && "captured pipeline must reproduce processHighCurvature byte-for-byte");
            r.dll_identical = (p.type != TYPE_INSHOULDER) || (p.break_kernel == kSiteBreakKernel);
        }
        else
        {
            result8u = ExpPipeline(view, p, im, r.timing);
            r.capture_identical = true;
            r.dll_identical = false;
        }
        if (result8u.empty())
            throw std::runtime_error("result8u is empty");

        // --- INSPECT: place back into zeros(originalSize) using the UNCLIPPED roi ---
        cv::Mat finalImage = result8u;
        if (r.use_roi)
        {
            finalImage = cv::Mat::zeros(r.original_size, CV_8UC1);
            if (roi != r.roi_rect)
                throw std::runtime_error("ROI exceeds the image; the DLL fails at result8u.copyTo(finalImage(roi))");
            result8u.copyTo(finalImage(roi));
        }
        r.result8u = finalImage;

        // --- intermediates in the full frame ---
        compare(src32f, -999, r.null_mask_before, cv::CMP_EQ);
        r.src_after_stage = work;
        r.null_mask_after = PlaceFull(nullAfter, r.original_size, r.roi_rect, r.use_roi, cv::Scalar::all(255));
        r.stage_removed_mask = PlaceFull(stageRemoved, r.original_size, r.roi_rect, r.use_roi);
        r.scaled = PlaceFull(im.scaled, r.original_size, r.roi_rect, r.use_roi);
        r.basis = PlaceFull(im.basis, r.original_size, r.roi_rect, r.use_roi);
        r.diff_f32 = PlaceFull(im.diff_float, r.original_size, r.roi_rect, r.use_roi);
        r.clipped = PlaceFull(im.clipped, r.original_size, r.roi_rect, r.use_roi);
        r.clip_low = im.low;
        r.clip_high = im.high;
        r.norm_min = im.nmin;
        r.norm_max = im.nmax;
        r.z_min = im.zmin;
        r.z_max = im.zmax;
        r.timing.total = r.timing.remove_stage + r.timing.scale + r.timing.basis + r.timing.diff + r.timing.clip_normalize;
    }
}

// ===== Experimental variants (never called on the default path) ==============
namespace DepthProcessorExp
{
    double computePercentileMasked(const cv::Mat& img, double pct, const cv::Mat& valid_mask)
    {
        CV_Assert(img.type() == CV_32F && valid_mask.type() == CV_8U && img.size() == valid_mask.size());
        std::vector<float> vals;
        vals.reserve(static_cast<size_t>(cv::countNonZero(valid_mask)));
        for (int r = 0; r < img.rows; ++r)
        {
            const float* p = img.ptr<float>(r);
            const uchar* m = valid_mask.ptr<uchar>(r);
            for (int c = 0; c < img.cols; ++c)
                if (m[c]) vals.push_back(p[c]);
        }
        if (vals.empty())
            return 0.0;
        std::sort(vals.begin(), vals.end());
        double pos = (pct / 100.0) * (vals.size() - 1);
        size_t idx_low = static_cast<size_t>(std::floor(pos));
        size_t idx_high = static_cast<size_t>(std::ceil(pos));
        double fraction = pos - idx_low;
        double low_val = vals[idx_low];
        double high_val = vals[idx_high];
        return low_val + fraction * (high_val - low_val);
    }

    cv::Mat patchBasedMedianMasked(const cv::Mat& data, const cv::Mat& null_mask, const cv::Size& patch_size, float overlap)
    {
        CV_Assert(data.type() == CV_32F && null_mask.type() == CV_8U);
        const int h = data.rows, w = data.cols;
        const int patch_h = patch_size.height, patch_w = patch_size.width;
        int step_h = static_cast<int>(patch_h * (1.0f - overlap));
        int step_w = static_cast<int>(patch_w * (1.0f - overlap));
        if (step_h < 1) step_h = 1;
        if (step_w < 1) step_w = 1;
        const int out_h = (h - patch_h) / step_h + 1;
        const int out_w = (w - patch_w) / step_w + 1;
        CV_Assert(out_h > 0 && out_w > 0);

        cv::Mat sampled(out_h, out_w, CV_32F, cv::Scalar(0));
        cv::Mat empty(out_h, out_w, CV_8U, cv::Scalar(0));
        std::vector<float> vals;
        vals.reserve(static_cast<size_t>(patch_h) * patch_w);
        for (int i = 0; i < out_h; i++)
        {
            for (int j = 0; j < out_w; j++)
            {
                const int r0 = i * step_h, c0 = j * step_w;
                vals.clear();
                for (int rr = 0; rr < patch_h; rr++)
                {
                    const float* rowPtr = data.ptr<float>(r0 + rr) + c0;
                    const uchar* mPtr = null_mask.ptr<uchar>(r0 + rr) + c0;
                    for (int cc = 0; cc < patch_w; cc++)
                        if (mPtr[cc] == 0) vals.push_back(rowPtr[cc]);
                }
                if (vals.empty())
                {
                    empty.at<uchar>(i, j) = 255;
                    continue;
                }
                std::sort(vals.begin(), vals.end());
                const size_t n = vals.size();
                sampled.at<float>(i, j) = (n % 2 == 0) ? 0.5f * (vals[n / 2 - 1] + vals[n / 2]) : vals[n / 2];
            }
        }

        const int emptyCount = cv::countNonZero(empty);
        if (emptyCount > 0 && emptyCount < out_h * out_w)
        {
            // Nearest valid grid cell via distance transform labels.
            cv::Mat dist, labels;
            cv::distanceTransform(empty, dist, labels, cv::DIST_L2, cv::DIST_MASK_PRECISE, cv::DIST_LABEL_PIXEL);
            std::vector<float> labelValue(static_cast<size_t>(out_h) * out_w + 1, 0.f);
            for (int i = 0; i < out_h; i++)
                for (int j = 0; j < out_w; j++)
                    if (!empty.at<uchar>(i, j))
                        labelValue[labels.at<int>(i, j)] = sampled.at<float>(i, j);
            for (int i = 0; i < out_h; i++)
                for (int j = 0; j < out_w; j++)
                    if (empty.at<uchar>(i, j))
                        sampled.at<float>(i, j) = labelValue[labels.at<int>(i, j)];
        }

        cv::Mat basis_plane;
        cv::resize(sampled, basis_plane, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
        return basis_plane;
    }

    void removeStageRestored(cv::Mat& src32f, int stagePosMode, int breakKernel)
    {
        if (src32f.empty() || stagePosMode == 3) return;
        const int h = src32f.rows, w = src32f.cols;
        cv::Mat mask = cv::Mat::zeros(h, w, CV_8UC1);
        for (int r = 0; r < h; ++r)
        {
            const float* ptr = src32f.ptr<float>(r);
            uchar* mptr = mask.ptr<uchar>(r);
            for (int c = 0; c < w; ++c)
                if (ptr[c] > -900.0f) mptr[c] = 1;
        }
        cv::Mat labelMask = mask;
        cv::Mat kernel;
        if (breakKernel >= 3)
        {
            kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(breakKernel, breakKernel));
            labelMask = cv::Mat();                 // own buffer: the only difference from the site code
            cv::erode(mask, labelMask, kernel);
        }
        cv::Mat labels, stats, centroids;
        const int num_labels = cv::connectedComponentsWithStats(labelMask, labels, stats, centroids, 8, CV_32S);
        if (num_labels <= 1) return;
        int max_area = 0, best_label = -1;
        for (int i = 1; i < num_labels; ++i)
        {
            const int area = stats.at<int>(i, cv::CC_STAT_AREA);
            if (area > max_area) { max_area = area; best_label = i; }
        }
        if (best_label == -1) return;
        cv::Mat keep = (labels == best_label);
        if (breakKernel >= 3)
        {
            cv::dilate(keep, keep, kernel);
            cv::bitwise_and(keep, mask, keep);     // mask is intact here -> border really restored
        }
        for (int r = 0; r < h; ++r)
        {
            float* ptr = src32f.ptr<float>(r);
            const uchar* kptr = keep.ptr<uchar>(r);
            for (int c = 0; c < w; ++c)
                if (kptr[c] == 0) ptr[c] = -999.0f;
        }
    }

    void fillSmallHoles(cv::Mat& src32f, int maxArea)
    {
        if (src32f.empty() || maxArea <= 0) return;
        cv::Mat nullMask;
        cv::compare(src32f, -999, nullMask, cv::CMP_EQ);
        if (cv::countNonZero(nullMask) == 0) return;

        cv::Mat labels, stats, centroids;
        const int n = cv::connectedComponentsWithStats(nullMask, labels, stats, centroids, 8, CV_32S);
        std::vector<uchar> small(static_cast<size_t>(n), 0);
        bool any = false;
        for (int i = 1; i < n; ++i)
        {
            if (stats.at<int>(i, cv::CC_STAT_AREA) <= maxArea) { small[i] = 1; any = true; }
        }
        if (!any) return;

        cv::Mat fill(src32f.size(), CV_8U, cv::Scalar(0));
        for (int r = 0; r < src32f.rows; ++r)
        {
            const int* l = labels.ptr<int>(r);
            uchar* f = fill.ptr<uchar>(r);
            for (int c = 0; c < src32f.cols; ++c)
                if (l[c] > 0 && small[l[c]]) f[c] = 255;
        }

        cv::Mat validMask;
        cv::compare(nullMask, 0, validMask, cv::CMP_EQ);
        double mn = 0, mx = 0;
        cv::minMaxLoc(src32f, &mn, &mx, nullptr, nullptr, validMask);
        if (!(mx > mn)) return;

        // Inpaint through a normalized 8U image, then map back to depth units.
        cv::Mat img8(src32f.size(), CV_8U, cv::Scalar(0));
        const double scale = 255.0 / (mx - mn);
        for (int r = 0; r < src32f.rows; ++r)
        {
            const float* s = src32f.ptr<float>(r);
            const uchar* v = validMask.ptr<uchar>(r);
            uchar* d = img8.ptr<uchar>(r);
            for (int c = 0; c < src32f.cols; ++c)
                if (v[c]) d[c] = cv::saturate_cast<uchar>((s[c] - mn) * scale);
        }
        cv::Mat out8;
        cv::inpaint(img8, fill, out8, 3.0, cv::INPAINT_TELEA);
        for (int r = 0; r < src32f.rows; ++r)
        {
            float* s = src32f.ptr<float>(r);
            const uchar* f = fill.ptr<uchar>(r);
            const uchar* o = out8.ptr<uchar>(r);
            for (int c = 0; c < src32f.cols; ++c)
                if (f[c]) s[c] = static_cast<float>(mn + o[c] / scale);
        }
    }
}
