#include "pch.h"
#include "3dDepthProcessing.h"

extern ITalosUtilFactories* g_pTalosUtilFactories;
extern MIL_ID               g_milSystem;

#define _AIV_LOG_START(msg) AIVLOG(g_pTalosUtilFactories->GetLogFactory(), msg, L"depthpreproc.log", AIV::eLogLevel::debug)

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

    //--------------------------------------------------------------------------
    std::pair<cv::Mat, cv::Mat> incenter_processing(const cv::Mat& data_16u, const std::string& opt = "vertical")
    {

        cv::Mat data_int32;
        data_16u.convertTo(data_int32, CV_32S);

        int rows = data_16u.rows;
        int cols = data_16u.cols;

        cv::Mat basis_plane;
        if (opt == "vertical")
        {
            basis_plane = cv::Mat::zeros(1, cols, CV_32F);
            for (int c = 0; c < cols; c++)
            {
                long long sum_col = 0;
                for (int r = 0; r < rows; r++)
                    sum_col += data_int32.at<int32_t>(r, c);

                float avg_col = static_cast<float>((double)sum_col / (double)rows);
                basis_plane.at<float>(0, c) = avg_col;
            }
        }
        else if (opt == "horizontal")
        {
            basis_plane = cv::Mat::zeros(rows, 1, CV_32F);
            for (int r = 0; r < rows; r++)
            {
                long long sum_row = 0;
                for (int c = 0; c < cols; c++)
                    sum_row += data_int32.at<int32_t>(r, c);

                float avg_row = static_cast<float>((double)sum_row / (double)cols);
                basis_plane.at<float>(r, 0) = avg_row;
            }
        }
        else
        {
            // 기본 vertical
            basis_plane = cv::Mat::zeros(1, cols, CV_32F);
        }

        // row_correct_plane = data_int32 - basis_plane
        cv::Mat row_correct_plane(rows, cols, CV_16SC1);
        if (opt == "vertical")
        {
            for (int r = 0; r < rows; r++)
            {
                for (int c = 0; c < cols; c++)
                {
                    int32_t val = data_int32.at<int32_t>(r, c)
                        - static_cast<int32_t>(basis_plane.at<float>(0, c));
                    if (val < -32768) val = -32768;
                    if (val > 32767)  val = 32767;
                    row_correct_plane.at<int16_t>(r, c) = static_cast<int16_t>(val);
                }
            }
        }
        else
        {
            for (int r = 0; r < rows; r++)
            {
                for (int c = 0; c < cols; c++)
                {
                    int32_t val = data_int32.at<int32_t>(r, c)
                        - static_cast<int32_t>(basis_plane.at<float>(r, 0));
                    if (val < -32768) val = -32768;
                    if (val > 32767)  val = 32767;
                    row_correct_plane.at<int16_t>(r, c) = static_cast<int16_t>(val);
                }
            }
        }

        // 여기서는 polynomial_fitting 대신 그대로 사용
        cv::Mat correct_depth_map_s16 = row_correct_plane.clone();

        // 10%,90% 및 20%,80% percentile 계산
        std::vector<int16_t> flatVals;
        flatVals.reserve(rows * cols);
        for (int r = 0; r < rows; r++)
        {
            for (int c = 0; c < cols; c++)
            {
                flatVals.push_back(correct_depth_map_s16.at<int16_t>(r, c));
            }
        }
        std::sort(flatVals.begin(), flatVals.end());

        auto getPercentile = [&](double pct) -> double
        {
            if (flatVals.empty()) return 0.0;
            double idx = pct / 100.0 * (flatVals.size() - 1);
            size_t iLo = static_cast<size_t>(std::floor(idx));
            size_t iHi = static_cast<size_t>(std::ceil(idx));
            double frac = idx - iLo;
            double valLo = static_cast<double>(flatVals[std::min(iLo, flatVals.size() - 1)]);
            double valHi = static_cast<double>(flatVals[std::min(iHi, flatVals.size() - 1)]);
            return valLo + frac * (valHi - valLo);
        };

        double top10 = getPercentile(10.0);
        double top90 = getPercentile(90.0);
        double top20 = getPercentile(20.0);
        double top80 = getPercentile(80.0);

        // clip
        cv::Mat clipped1(rows, cols, CV_16SC1);
        cv::Mat clipped2(rows, cols, CV_16SC1);
        for (int r = 0; r < rows; r++)
        {
            for (int c = 0; c < cols; c++)
            {
                int16_t v = correct_depth_map_s16.at<int16_t>(r, c);
                int16_t v1 = static_cast<int16_t>(std::max(std::min((double)v, top90), top10));
                int16_t v2 = static_cast<int16_t>(std::max(std::min((double)v, top80), top20));

                clipped1.at<int16_t>(r, c) = v1;
                clipped2.at<int16_t>(r, c) = v2;
            }
        }

        // 16S -> 8U 변환 (min-max scaling)
        auto mat_to_8U = [&](const cv::Mat& mat_s16)
        {
            cv::Mat out(rows, cols, CV_8UC1);
            double mn, mx;
            cv::minMaxLoc(mat_s16, &mn, &mx);
            double rng = (mx - mn) + 1e-5;
            for (int rr = 0; rr < rows; rr++)
            {
                for (int cc = 0; cc < cols; cc++)
                {
                    double val = static_cast<double>(mat_s16.at<int16_t>(rr, cc));
                    val = (val - mn) / rng * 255.0;
                    if (std::isnan(val)) val = 0.0;
                    if (val < 0.0) val = 0.0;
                    if (val > 255.0) val = 255.0;
                    out.at<uchar>(rr, cc) = static_cast<uchar>(val);

                    // 원본이 0이면 0으로 유지
                    if (data_16u.at<uint16_t>(rr, cc) == 0)
                        out.at<uchar>(rr, cc) = 0;
                }
            }
            return out;
        };

        cv::Mat normalized1 = mat_to_8U(clipped1);
        cv::Mat normalized2 = mat_to_8U(clipped2);

        return { normalized1, normalized2 };
    }

    std::pair<cv::Mat, cv::Mat> shoulder_processing(const cv::Mat& data_16u, const std::array<int, 4>& ROI)
    {
        int s_x = ROI[0], s_y = ROI[1];
        int e_x = ROI[2], e_y = ROI[3];

        int h = data_16u.rows, w = data_16u.cols;
        if (s_x < 0 || s_y < 0 || e_x > w || e_y > h)
            return { cv::Mat(), cv::Mat() };

        int roi_height = e_y - s_y;
        int roi_width = e_x - s_x;
        cv::Mat roi_data_f(roi_height, roi_width, CV_32F, cv::Scalar(0));

        for (int rr = 0; rr < roi_height; rr++) {
            for (int cc = 0; cc < roi_width; cc++) {
                uint16_t val = data_16u.at<uint16_t>(s_y + rr, s_x + cc);
                roi_data_f.at<float>(rr, cc) = static_cast<float>(val);
            }
        }

        // 각 열별 평균 계산
        std::vector<float> col_means(roi_width, 0.0f);
        for (int cc = 0; cc < roi_width; cc++) {
            double sum = 0.0;
            for (int rr = 0; rr < roi_height; rr++) {
                sum += roi_data_f.at<float>(rr, cc);
            }
            col_means[cc] = static_cast<float>(sum / roi_height);
        }

        // depth_map = |pix - colmean|
        cv::Mat depth_map(roi_height, roi_width, CV_32F);
        for (int rr = 0; rr < roi_height; rr++) {
            for (int cc = 0; cc < roi_width; cc++) {
                float v = roi_data_f.at<float>(rr, cc);
                depth_map.at<float>(rr, cc) = std::fabs(v - col_means[cc]);
            }
        }

        cv::Mat gradient_x(roi_height, roi_width, CV_32F, cv::Scalar(0));
        for (int rr = 0; rr < roi_height; rr++) {
            if (roi_width > 1)
                gradient_x.at<float>(rr, 0) = std::fabs(roi_data_f.at<float>(rr, 1) - roi_data_f.at<float>(rr, 0));

            for (int cc = 1; cc < roi_width - 1; cc++) {
                float v_prev = roi_data_f.at<float>(rr, cc - 1);
                float v_next = roi_data_f.at<float>(rr, cc + 1);
                gradient_x.at<float>(rr, cc) = std::fabs(v_next - v_prev) / 2.0f;
            }
            if (roi_width > 1)
                gradient_x.at<float>(rr, roi_width - 1) = std::fabs(roi_data_f.at<float>(rr, roi_width - 1) - roi_data_f.at<float>(rr, roi_width - 2));
        }

        auto flattenMat = [&](const cv::Mat& mat) {
            std::vector<float> vec;
            vec.reserve(mat.total());
            for (int r = 0; r < mat.rows; r++) {
                for (int c = 0; c < mat.cols; c++) {
                    vec.push_back(mat.at<float>(r, c));
                }
            }
            return vec;
        };
        auto percentile_f = [&](std::vector<float>& arr, double pct) -> float {
            if (arr.empty()) return 0.0f;
            std::sort(arr.begin(), arr.end());
            double idx = pct / 100.0 * (arr.size() - 1);
            size_t iLo = static_cast<size_t>(std::floor(idx));
            size_t iHi = static_cast<size_t>(std::ceil(idx));
            double frac = idx - iLo;
            float vLo = arr[std::min(iLo, arr.size() - 1)];
            float vHi = arr[std::min(iHi, arr.size() - 1)];
            return vLo + static_cast<float>(frac) * (vHi - vLo);
        };
        auto clip_inplace = [&](cv::Mat& mat, float lo, float hi) {
            for (int r = 0; r < mat.rows; r++) {
                for (int c = 0; c < mat.cols; c++) {
                    float v = mat.at<float>(r, c);
                    if (v < lo) v = lo;
                    if (v > hi) v = hi;
                    mat.at<float>(r, c) = v;
                }
            }
        };

        auto dvals = flattenMat(depth_map);
        float d10 = percentile_f(dvals, 10.0f);
        float d90 = percentile_f(dvals, 90.0f);
        clip_inplace(depth_map, d10, d90);

        auto gvals = flattenMat(gradient_x);
        float g20 = percentile_f(gvals, 20.0f);
        float g80 = percentile_f(gvals, 80.0f);
        clip_inplace(gradient_x, g20, g80);

        auto to8U = [&](const cv::Mat& fm) {
            cv::Mat out8(fm.size(), CV_8UC1);
            double mn, mx;
            cv::minMaxLoc(fm, &mn, &mx);
            double rng = (mx - mn) + 1e-5;
            for (int rr = 0; rr < fm.rows; rr++) {
                for (int cc = 0; cc < fm.cols; cc++) {
                    double val = fm.at<float>(rr, cc);
                    double sc = (val - mn) / rng * 255.0;
                    if (std::isnan(sc)) sc = 0.0;
                    if (sc < 0.0) sc = 0.0;
                    if (sc > 255.0) sc = 255.0;
                    out8.at<uchar>(rr, cc) = static_cast<uchar>(sc);
                    if (roi_data_f.at<float>(rr, cc) == 0.0f)
                        out8.at<uchar>(rr, cc) = 0;
                }
            }
            return out8;
        };

        cv::Mat n1 = to8U(depth_map);
        cv::Mat n2 = to8U(gradient_x);

        return { n1, n2 };
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


    cv::Mat polynomialFitting(const cv::Mat& data, double sample_ratio = 0.25) {
        CV_Assert(data.type() == CV_32F);
        int image_height = data.rows;
        int image_width = data.cols;
        int num_samples = static_cast<int>(image_width * sample_ratio);
        if (num_samples < 1) num_samples = 1;

        std::vector<int> cols(image_width);
        std::iota(cols.begin(), cols.end(), 0);
        std::mt19937 rng(42);
        std::shuffle(cols.begin(), cols.end(), rng);
        std::vector<int> representative(cols.begin(), cols.begin() + num_samples);
        sort(representative.begin(), representative.end());

        int N = image_height * num_samples;
        cv::Mat A(N, 6, CV_64F);
        cv::Mat b(N, 1, CV_64F);
        int index = 0;
        for (int i = 0; i < image_height; i++) {
            for (int j : representative) {
                double x = static_cast<double>(j);
                double y = static_cast<double>(i);
                double z = static_cast<double>(data.at<float>(i, j));
                A.at<double>(index, 0) = 1.0;
                A.at<double>(index, 1) = x;
                A.at<double>(index, 2) = y;
                A.at<double>(index, 3) = x * x;
                A.at<double>(index, 4) = x * y;
                A.at<double>(index, 5) = y * y;
                b.at<double>(index, 0) = z;
                index++;
            }
        }
        cv::Mat beta;
        bool solved = cv::solve(A, b, beta, cv::DECOMP_SVD);
        if (!solved) {
            throw std::runtime_error("Polynomial fitting failed to solve linear system.");
        }

        cv::Mat predicted(image_height, image_width, CV_64F);
        for (int i = 0; i < image_height; i++) {
            for (int j = 0; j < image_width; j++) {
                double x = static_cast<double>(j);
                double y = static_cast<double>(i);
                double pred = beta.at<double>(0, 0)
                    + beta.at<double>(1, 0) * x
                    + beta.at<double>(2, 0) * y
                    + beta.at<double>(3, 0) * x * x
                    + beta.at<double>(4, 0) * x * y
                    + beta.at<double>(5, 0) * y * y;
                predicted.at<double>(i, j) = pred;
            }
        }
        cv::Mat predicted32;
        predicted.convertTo(predicted32, CV_32F);
        cv::Mat diff;
        cv::absdiff(data, predicted32, diff);
        cv::Mat correct_depth_map;
        diff.convertTo(correct_depth_map, CV_16S);
        return correct_depth_map;
    }

    cv::Mat processInnerCenter(const cv::Mat& data) {
        CV_Assert(data.type() == CV_32F);
        cv::Mat null_mask;
        compare(data, -999, null_mask, cv::CMP_EQ);
        cv::Mat scaled = scaleTo16bitIgnoreNull(data, null_mask);

        cv::Mat basis_plane_horz;
        reduce(scaled, basis_plane_horz, 0, cv::REDUCE_AVG);
        cv::Mat basis_plane_full;
        repeat(basis_plane_horz, scaled.rows, 1, basis_plane_full);
        cv::Mat row_correct_plane = scaled - basis_plane_full;

        cv::Mat poly_fit = polynomialFitting(row_correct_plane, 0.25);

        cv::Mat poly_fit_f32;
        poly_fit.convertTo(poly_fit_f32, CV_32F);
        cv::Mat normalized = postClipNormalize(poly_fit_f32, 5, 95);

        normalized.setTo(0, null_mask);
        return normalized;
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

cv::Mat DeepCopyMil2OpencvFloat(MIL_ID milImage)
{
    if (milImage == M_NULL) {
        return cv::Mat();
    }
    MIL_INT width = 0, height = 0, milType = 0;
    MbufInquire(milImage, M_SIZE_X, &width);
    MbufInquire(milImage, M_SIZE_Y, &height);
    MbufInquire(milImage, M_TYPE, &milType);
    cv::Mat out(height, width, CV_32FC1);
    MbufGet(milImage, out.data);
    return out;
}

cv::Mat C3DPreprocess::DeepCopyMil2OpencvNormalize(MIL_ID milImage)
{
    if (milImage == M_NULL) {
        return cv::Mat();
    }

    MIL_INT width = 0, height = 0, milType = 0;
    MbufInquire(milImage, M_SIZE_X, &width);
    MbufInquire(milImage, M_SIZE_Y, &height);
    MbufInquire(milImage, M_TYPE, &milType);

    bool isFloat = ((milType & M_FLOAT) == M_FLOAT);
    bool isSigned = ((milType & M_SIGNED) == M_SIGNED);
    bool isUnsign = ((milType & M_UNSIGNED) == M_UNSIGNED);
    int bitDepth = (milType & 0xFF);

    cv::Mat temp32f(height, width, CV_32FC1);

    if (bitDepth == 32 && isFloat) {

        MbufGet(milImage, temp32f.data);
    }
    else if (bitDepth == 32 && (isSigned || isUnsign)) {
        cv::Mat temp32s(height, width, CV_32SC1);
        MbufGet(milImage, temp32s.data);
        temp32s.convertTo(temp32f, CV_32F);
    }
    else if (bitDepth == 16) {
        cv::Mat temp16u(height, width, CV_16UC1);
        MbufGet(milImage, temp16u.data);
        temp16u.convertTo(temp32f, CV_32F);
    }
    else if (bitDepth == 8) {
        cv::Mat temp8u(height, width, CV_8UC1);
        MbufGet(milImage, temp8u.data);
        temp8u.convertTo(temp32f, CV_32F);
    }
    else {
        g_pTalosUtilFactories->GetLogFactory()->PrintConsoleAsync(
            L"[ERROR] Not implemented for this milType in DeepCopyMil2OpencvNormalize");
        return cv::Mat();
    }

    double minVal = 0.0, maxVal = 0.0;
    cv::minMaxLoc(temp32f, &minVal, &maxVal);
    cv::Mat out16u(height, width, CV_16UC1);
    if (maxVal > minVal)
    {
        double range = maxVal - minVal;
        for (int r = 0; r < height; r++) {
            float* srcPtr = temp32f.ptr<float>(r);
            auto* dstPtr = out16u.ptr<uint16_t>(r);
            for (int c = 0; c < width; c++) {
                double val = static_cast<double>(srcPtr[c]);
                double scaled = (val - minVal) / range * 65535.0;
                if (std::isnan(scaled)) scaled = 0.0;
                if (scaled < 0.0) scaled = 0.0;
                if (scaled > 65535.0) scaled = 65535.0;
                dstPtr[c] = static_cast<uint16_t>(scaled);
            }
        }
    }
    else
    {
        out16u.setTo(0);
    }
    return out16u;
}

void C3DPreprocess::CopyOpencv2Mil_KeepBuffer_If32(const cv::Mat& cv8u, MIL_ID milOutput)
{
    if (milOutput == M_NULL || cv8u.empty())
        return;

    MIL_INT width = 0, height = 0, milType = 0;
    MbufInquire(milOutput, M_SIZE_X, &width);
    MbufInquire(milOutput, M_SIZE_Y, &height);
    MbufInquire(milOutput, M_TYPE, &milType);

    if ((cv8u.cols != width) || (cv8u.rows != height)) {
        g_pTalosUtilFactories->GetLogFactory()->PrintConsoleAsync(
            L"[WARNING] MIL buffer size != CV image size. Using partial or ignoring.");
    }

    int bitDepth = (milType & 0xFF);
    bool isFloat = ((milType & M_FLOAT) == M_FLOAT);
    bool isSigned = ((milType & M_SIGNED) == M_SIGNED);
    bool isUnsign = ((milType & M_UNSIGNED) == M_UNSIGNED);

    if (bitDepth == 32)
    {
        if (isFloat)
        {
            cv::Mat temp32f;
            cv8u.convertTo(temp32f, CV_32FC1, 1.0);
            MbufPut(milOutput, temp32f.data);
        }
        else
        {
            cv::Mat temp32s;
            cv8u.convertTo(temp32s, CV_32SC1, 1.0);
            MbufPut(milOutput, temp32s.data);
        }
    }
    else
    {
        g_pTalosUtilFactories->GetLogFactory()->PrintConsoleAsync(
            L"[INFO] milOutput is not 32-bit, putting raw 8-bit data.");
        MbufPut(milOutput, cv8u.data);
    }
}

int C3DPreprocess::INSPECT(UINT_PTR dwInspThreadID, UINT nMsgType) {
    _AIV_LOG_START(L"C3DPreprocess::INSPECT");

    m_dwInspThreadID = dwInspThreadID;
    m_nMsgType = nMsgType;

    if (m_vecInput.empty() || m_vecOutput.empty()) {
        g_pTalosUtilFactories->GetLogFactory()->PrintConsoleAsync(
            L"[ERROR] No input or no output buffer!");
        PostAivThreadMessage(m_dwInspThreadID, m_nMsgType,
            (UINT_PTR)m_nOutputImgIdx, (UINT_PTR)-1);
        return -1;
    }

    //MbufSave(L"E:\\debug_input_after_proc.mim", m_vecInput[0]);
    MIL_INT milType = 0;
    MbufInquire(m_vecInput[0], M_TYPE, &milType);
    int bitDepth = (milType & 0xFF);
    bool isFloat = ((milType & M_FLOAT) == M_FLOAT);

    // 1) MIL -> OpenCV (Raw 데이터)
    cv::Mat src32f = DeepCopyMil2OpencvFloat(m_vecInput[0]);

    cv::Size originalSize = src32f.size();
    cv::Rect roi(0, 0, 0, 0);
    bool useRoi = (m_roi[2] > m_roi[0] && m_roi[3] > m_roi[1]);

    if (useRoi) {
        roi = cv::Rect(m_roi[0], m_roi[1], m_roi[2] - m_roi[0], m_roi[3] - m_roi[1]);
        src32f = src32f(roi & cv::Rect(0, 0, src32f.cols, src32f.rows));
    }

    //DepthProcessor::removeStageFromRawData(src32f, m_nStagePos, 9);
    // =========================================================================

    // 2) 전처리 (결과는 8비트)
    cv::Mat result8u;
    switch (m_eDepthType) {
    case eDepthPreprocType::INNERCENTER:
    {
        //DepthProcessor::removeStageFromRawDataBead(src32f, m_nStagePos);
        result8u = DepthProcessor::processHighCurvature(src32f, m_PatchSize, m_overlap);
        break;
    }
    case eDepthPreprocType::BEAD:
    {
        DepthProcessor::removeStageFromRawDataBead(src32f, m_nStagePos);
        result8u = DepthProcessor::processHighCurvature(src32f, m_PatchSize, m_overlap);
        break;
    }
    case eDepthPreprocType::INSHOULDER:
    {
        DepthProcessor::removeStageFromRawData(src32f, m_nStagePos, 3);
        result8u = DepthProcessor::processHighCurvature(src32f, m_PatchSize, m_overlap);
        break;
    }
    default: {
        g_pTalosUtilFactories->GetLogFactory()->PrintConsoleAsync(
            L"[ERROR] Unknown eDepthPreprocType");
        PostAivThreadMessage(m_dwInspThreadID, m_nMsgType,
            (UINT_PTR)m_nOutputImgIdx, (UINT_PTR)-1);
        return -1;
    }
    }

    if (result8u.empty()) {
        g_pTalosUtilFactories->GetLogFactory()->PrintConsoleAsync(
            L"[ERROR] result8u is empty. Possibly invalid ROI, etc.");
        PostAivThreadMessage(m_dwInspThreadID, m_nMsgType,
            (UINT_PTR)m_nOutputImgIdx, (UINT_PTR)-1);
        return -1;
    }

    cv::Mat finalImage = result8u;
    if (useRoi) {
        finalImage = cv::Mat::zeros(originalSize, CV_8UC1);
        result8u.copyTo(finalImage(roi));
    }

    CopyOpencv2Mil_KeepBuffer_If32(finalImage, m_vecOutput[0]);

    PostAivThreadMessage(m_dwInspThreadID, m_nMsgType,
        (UINT_PTR)m_nOutputImgIdx, (UINT_PTR)0);
    return 0;
}

void C3DPreprocess::ReadInfo(std::shared_ptr<AiV::Utils::IIniReaderManager> pReader)
{
    if (m_vecIniIdx.size() > 1)
    {
        g_pTalosUtilFactories->GetLogFactory()->PrintConsoleAsync(
            L"[ERROR] multiple sections not supported in example.");
        return;
    }

    for (auto nItemIdx : m_vecIniIdx)
    {
        std::wstringstream ss;
        ss << L"CAL" << std::setw(4) << std::setfill(L'0') << nItemIdx;
        auto pSec = pReader->GetSection(ss.str().c_str());
        ReadSection(pSec);
    }
}

void C3DPreprocess::ReadSection(std::shared_ptr<AiV::Utils::IIniSectionData> pSection)
{
    m_strName = pSection->GetString(L"Name", L"", false);
    if (m_strName.empty())
        throw std::runtime_error("'Name' key is missing in configuration");

    std::wstring strDepthType = pSection->GetString(L"DepthPreprocType", L"", false);
    if (strDepthType.empty())
        throw std::runtime_error("'DepthPreprocType' key is missing in configuration");
    if (strDepthType == L"INNERCENTER")
        m_eDepthType = eDepthPreprocType::INNERCENTER;
    else if (strDepthType == L"INSHOULDER")
        m_eDepthType = eDepthPreprocType::INSHOULDER;
    else if (strDepthType == L"BEAD")
        m_eDepthType = eDepthPreprocType::BEAD;
    else
        throw std::runtime_error("Invalid 'DepthPreprocType' value");

    std::wstring strStagePos = pSection->GetString(L"StagePosition", L"AUTO", false);
    if (strStagePos == L"TOP")          m_nStagePos = 1;
    else if (strStagePos == L"BOTTOM")   m_nStagePos = 2;
    else if (strStagePos == L"NONE")     m_nStagePos = 3;
    else                                m_nStagePos = 0; // AUTO

    std::wstring strPatchSize = pSection->GetString(L"PatchSize", L"", false);
    if (!strPatchSize.empty())
    {
        int width = 0, height = 0;
        if (2 == swscanf_s(strPatchSize.c_str(), L"%d,%d", &width, &height))
        {
            m_PatchSize = cv::Size(width, height);
        }
        else
        {
            std::wstring wmsg = L"[Error] C3DPreprocess::ReadSection: Unable to parse PatchSize from '";
            wmsg += strPatchSize;
            wmsg += L"'; expected format: width,height";

            std::string errMsg(wmsg.begin(), wmsg.end());
            throw std::runtime_error(errMsg);
        }
    }
    else
    {
        throw std::runtime_error("'PatchSize' key is missing in configuration; expected format: width,height");
    }

    std::wstring strLowPct = pSection->GetString(L"Lower Percentage", L"", false);
    if (!strLowPct.empty())
        m_lowerPct = _wtof(strLowPct.c_str());
    else
        throw std::runtime_error("'Lower Percentage' key is missing in configuration");

    std::wstring strUppPct = pSection->GetString(L"Upper Percentage", L"", false);
    if (!strUppPct.empty())
        m_upper_pct = _wtof(strUppPct.c_str());
    else
        throw std::runtime_error("'Upper Percentage' key is missing in configuration");

    std::wstring strOverlap = pSection->GetString(L"Overlap", L"", false);
    if (!strOverlap.empty())
    {
        float val = static_cast<float>(_wtof(strOverlap.c_str()));
        if (val <= 0.0f || val >= 1.0f)
        {
            std::wstring wmsg = L"[Error] Invalid Overlap value: " + strOverlap + L". Expected range: (0, 1).";
            std::string errMsg(wmsg.begin(), wmsg.end());
            throw std::runtime_error(errMsg);
        }
        m_overlap = val;
    }
    else
    {
        m_overlap = 0.5f;
    }

    std::wstring strRoi = pSection->GetString(L"ROI", L"", false);
    if (!strRoi.empty())
    {
        int x1, y1, x2, y2;
        if (4 == swscanf_s(strRoi.c_str(), L"%d,%d,%d,%d", &x1, &y1, &x2, &y2))
        {
            m_roi[0] = x1;
            m_roi[1] = y1;
            m_roi[2] = x2;
            m_roi[3] = y2;
        }
        else
        {
            std::wstring wmsg = L"[Error] C3DPreprocess::ReadSection: Unable to parse ROI from '";
            wmsg += strRoi;
            wmsg += L"'; expected format: x1,y1,x2,y2";
            std::string errMsg(wmsg.begin(), wmsg.end());
            throw std::runtime_error(errMsg);
        }
    }
}

void C3DPreprocess::IncenterProcessing(const cv::Mat& src16u, cv::Mat& out8u)
{
    int roiX1 = m_roi[0];
    int roiY1 = m_roi[1];
    int roiX2 = m_roi[2];
    int roiY2 = m_roi[3];

    int roiWidth = roiX2 - roiX1;
    int roiHeight = roiY2 - roiY1;

    if (roiWidth > 0 && roiHeight > 0 &&
        roiX1 >= 0 && roiY1 >= 0 &&
        (roiX1 + roiWidth) <= src16u.cols &&
        (roiY1 + roiHeight) <= src16u.rows)
    {
        cv::Rect roiRect(roiX1, roiY1, roiWidth, roiHeight);
        cv::Mat cropped = src16u(roiRect).clone();

        auto pair_ = DepthProcessor::incenter_processing(cropped, "vertical");
        cv::Mat merged;
        cv::addWeighted(pair_.first, 0.5, pair_.second, 0.5, 0.0, merged);

        if (merged.cols != roiWidth || merged.rows != roiHeight) {
            cv::resize(merged, merged, cv::Size(roiWidth, roiHeight), 0, 0, cv::INTER_LINEAR);
        }

        out8u = cv::Mat::zeros(src16u.size(), CV_8UC1);
        merged.copyTo(out8u(roiRect));
    }
    else
    {
        auto pair_ = DepthProcessor::incenter_processing(src16u, "vertical");
        cv::Mat merged;
        cv::addWeighted(pair_.first, 0.5, pair_.second, 0.5, 0.0, merged);

        if (merged.cols > 1940) {
            cv::resize(merged, out8u, cv::Size(1940, merged.rows), 0, 0, cv::INTER_LINEAR);
        }
        else {
            out8u = merged.clone();
        }
    }
}

void C3DPreprocess::InShoulderProcessing(const cv::Mat& src16u, cv::Mat& out8u)
{
    auto pair_ = DepthProcessor::shoulder_processing(src16u, m_roi);
    if (pair_.first.empty() || pair_.second.empty()) {
        out8u = cv::Mat();
        return;
    }
    cv::Mat merged;
    cv::addWeighted(pair_.first, 0.5, pair_.second, 0.5, 0.0, merged);

    int roiWidth = m_roi[2] - m_roi[0];
    int roiHeight = m_roi[3] - m_roi[1];

    if (merged.cols != roiWidth || merged.rows != roiHeight) {
        cv::resize(merged, merged, cv::Size(roiWidth, roiHeight), 0, 0, cv::INTER_LINEAR);
    }

    out8u = cv::Mat::zeros(src16u.size(), CV_8UC1);
    cv::Rect roiRect(m_roi[0], m_roi[1], roiWidth, roiHeight);
    merged.copyTo(out8u(roiRect));
}

void C3DPreprocess::Initial()
{
    _AIV_LOG_START(L"C3DPreprocess::Initial");
    m_milTempMax = M_NULL;
}

void C3DPreprocess::SetParam(const wchar_t* szParamIniPath, int* pParams, size_t nParamLen)
{
    _AIV_LOG_START(L"C3DPreprocess::SetParam");

    for (size_t i = 0; i < nParamLen; i++) {
        m_vecIniIdx.push_back(pParams[i]);
    }

    auto pReader = g_pTalosUtilFactories->GetIniReaderFactory()->GetReader(szParamIniPath);
    ReadInfo(pReader);
}

int C3DPreprocess::Final()
{
    _AIV_LOG_START(L"C3DPreprocess::Final");

    if (m_milTempMax != M_NULL) {
        MbufFree(m_milTempMax);
        m_milTempMax = M_NULL;
    }
    return 0;
}

int C3DPreprocess::Reset(const char* szInnerID, const wchar_t* szProductID)
{
    _AIV_LOG_START(L"C3DPreprocess::Reset");

    m_vecInput.clear();
    m_vecOutput.clear();

    return 0;
}

int C3DPreprocess::SetInputImg(int nFovIdx, MIL_ID milInputImg)
{
    _AIV_LOG_START(L"C3DPreprocess::SetInputImg");

    m_vecInput.push_back(milInputImg);

    if (m_milTempMax == M_NULL) {
        MbufClone(milInputImg, M_DEFAULT, M_DEFAULT,
            M_DEFAULT, M_DEFAULT, M_DEFAULT,
            M_DEFAULT, &m_milTempMax);
    }
    return 0;
}

int C3DPreprocess::SetOutputImg(int nFovIdx, MIL_ID milOutputImg)
{
    _AIV_LOG_START(L"C3DPreprocess::SetOutputImg");
    if (!m_vecOutput.empty()) {
        g_pTalosUtilFactories->GetLogFactory()->PrintConsoleAsync(
            L"[Warning] C3DPreprocess::SetOutputImg can only handle one output image!");
        return -1;
    }
    m_nOutputImgIdx = nFovIdx;
    m_vecOutput.push_back(milOutputImg);
    return 0;
}