// Experimental algorithm v2 (see SimV2 in DepthPreprocCore.h). Not part of any DLL.
//   band-aware null handling + reliability gating + slope-compensated fixed-scale residual
//
//   1. band valid set  : erode-break -> largest component -> dilate -> AND original mask (mask stays intact)
//   2. band region     : per-column envelope of the band (median filtered) -> nulls inside it are "holes", outside = background
//   3. reliability     : valid pixels next to nulls are dropped (edge), small holes are interpolated, and inside dropout
//                        zones (local null density rho) pixels are dropped when rho is high or when they are residual outliers
//   4. reference       : null-aware patch median in mm, residual r = z - reference
//   5. slope           : r * cos(theta) -> deviation along the surface normal (steep zones are no longer amplified)
//   6. output          : +-range_mm -> 1..255, 128 = on the reference surface. Unreliable / hole pixels are neutral (128) and the
//                        texture fades to neutral over `feather` px next to them. 0 only outside the band.
#include "DepthPreprocCore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace DepthProcessorV2
{
    namespace
    {
        void Median1D(std::vector<float>& v, int win)
        {
            if (win < 3 || v.size() < 3) return;
            if (win % 2 == 0) ++win;
            const int n = static_cast<int>(v.size()), pad = win / 2;
            std::vector<float> src(v), buf;
            buf.reserve(static_cast<size_t>(win));
            for (int i = 0; i < n; ++i)
            {
                buf.clear();
                for (int k = std::max(0, i - pad); k <= std::min(n - 1, i + pad); ++k) buf.push_back(src[k]);
                std::nth_element(buf.begin(), buf.begin() + buf.size() / 2, buf.end());
                v[i] = buf[buf.size() / 2];
            }
        }

        // Interpolate `todo` pixels of z from the valid ones (normalized convolution, two fine levels only).
        void FillSmall(cv::Mat& z, const cv::Mat& valid8u, const cv::Mat& todo8u, cv::Mat& filled8u)
        {
            cv::Mat w0;
            valid8u.convertTo(w0, CV_32F, 1.0 / 255.0);
            cv::Mat zw0 = z.mul(w0);
            cv::Mat todo = todo8u.clone();
            filled8u = cv::Mat::zeros(z.size(), CV_8U);
            const int ks[3] = { 3, 7, 15 };
            for (int i = 0; i < 3 && cv::countNonZero(todo) > 0; ++i)
            {
                cv::Mat a, b;
                cv::blur(zw0, a, cv::Size(ks[i], ks[i]));
                cv::blur(w0, b, cv::Size(ks[i], ks[i]));
                cv::Mat ok = (b > 0.25) & todo;
                if (cv::countNonZero(ok) == 0) continue;
                cv::Mat den = cv::max(b, 1e-6), est;
                cv::divide(a, den, est);
                est.copyTo(z, ok);
                filled8u |= ok;
                todo &= ~ok;
            }
        }

        cv::Mat ReferenceAndResidual(const cv::Mat& z, const cv::Mat& usable8u, const SimParams& p, cv::Mat& basis)
        {
            cv::Mat nullForBasis = ~usable8u;
            basis = DepthProcessorExp::patchBasedMedianMasked(z, nullForBasis, cv::Size(p.patch_w, p.patch_h), static_cast<float>(p.overlap));
            cv::Mat res;
            cv::subtract(z, basis, res, cv::noArray(), CV_32F);
            if (p.exp.v2.slope)
            {
                cv::Mat bs, gx, gy;
                cv::GaussianBlur(basis, bs, cv::Size(0, 0), 5.0);
                cv::Sobel(bs, gx, CV_32F, 1, 0, 3, 1.0 / (8.0 * p.exp.v2.px_x_mm));
                cv::Sobel(bs, gy, CV_32F, 0, 1, 3, 1.0 / (8.0 * p.exp.v2.px_y_mm));
                cv::Mat g2 = gx.mul(gx) + gy.mul(gy) + 1.0f, c;
                cv::sqrt(g2, c);
                cv::divide(res, c, res);
            }
            res.setTo(0, ~usable8u);
            return res;
        }

        // Fill every `todo` pixel of z from the valid ones by multi-scale normalized convolution (fine scale first),
        // then soften the seams inside the filled area only. Measured pixels are never modified.
        void PushPullFill(cv::Mat& z, const cv::Mat& valid8u, const cv::Mat& todo8u, cv::Mat& filled8u)
        {
            cv::Mat w0;
            valid8u.convertTo(w0, CV_32F, 1.0 / 255.0);
            cv::Mat zw0 = z.mul(w0);
            cv::Mat todo = todo8u.clone();
            filled8u = cv::Mat::zeros(z.size(), CV_8U);
            const int h = z.rows, w = z.cols;
            for (int level = 0; level <= 9; ++level)
            {
                if (cv::countNonZero(todo) == 0) break;
                cv::Mat zwU, wU;
                double minW;
                if (level == 0)
                {
                    cv::blur(zw0, zwU, cv::Size(3, 3));
                    cv::blur(w0, wU, cv::Size(3, 3));
                    minW = 0.30;
                }
                else
                {
                    const int s = 1 << level;
                    const cv::Size ds(std::max(1, w / s), std::max(1, h / s));
                    if (ds.width < 4 || ds.height < 4) break;
                    cv::Mat zwL, wL;
                    cv::resize(zw0, zwL, ds, 0, 0, cv::INTER_AREA);
                    cv::resize(w0, wL, ds, 0, 0, cv::INTER_AREA);
                    cv::GaussianBlur(zwL, zwL, cv::Size(5, 5), 0);
                    cv::GaussianBlur(wL, wL, cv::Size(5, 5), 0);
                    cv::resize(zwL, zwU, z.size(), 0, 0, cv::INTER_LINEAR);
                    cv::resize(wL, wU, z.size(), 0, 0, cv::INTER_LINEAR);
                    minW = 0.05;
                }
                cv::Mat ok = (wU > minW) & todo;
                if (cv::countNonZero(ok) == 0) continue;
                cv::Mat den = cv::max(wU, 1e-6), est;
                cv::divide(zwU, den, est);
                est.copyTo(z, ok);
                filled8u |= ok;
                todo &= ~ok;
            }
            if (cv::countNonZero(filled8u) > 0)
            {
                cv::Mat known = valid8u | filled8u, wk, sm;
                known.convertTo(wk, CV_32F, 1.0 / 255.0);
                for (int it = 0; it < 2; ++it)
                {
                    cv::Mat zw = z.mul(wk), a, b;
                    cv::GaussianBlur(zw, a, cv::Size(0, 0), 1.5);
                    cv::GaussianBlur(wk, b, cv::Size(0, 0), 1.5);
                    cv::Mat den = cv::max(b, 1e-6);
                    cv::divide(a, den, sm);
                    sm.copyTo(z, filled8u);
                }
            }
        }

        double PercentileOf(std::vector<float>& v, double pct)
        {
            if (v.empty()) return 0.0;
            const size_t k = static_cast<size_t>(std::min<double>(v.size() - 1, std::max(0.0, pct / 100.0 * (v.size() - 1))));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        }

        // Restore mode: every measured pixel is kept as measured, nulls inside the band are interpolated from the
        // surrounding depth, and the output keeps the familiar percentile contrast (computed on measured pixels only).
        cv::Mat ProcessRestore(const cv::Mat& src, const cv::Mat& B, const cv::Mat& Vb, const cv::Mat& rho,
                               const SimParams& p, Debug& dbg)
        {
            const SimV2& o = p.exp.v2;
            cv::Mat valid = Vb.clone();
            if (o.edge > 0)       // mixed pixels right next to a null are re-interpolated
            {
                cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * o.edge + 1, 2 * o.edge + 1));
                cv::erode(Vb, valid, k);
            }
            cv::Mat zf = src.clone();
            zf.setTo(0, ~valid);
            cv::Mat filled, holes = B & ~valid;
            PushPullFill(zf, valid, holes, filled);

            if (o.spike_mm > 0)   // only gross measurement spikes inside dropout zones are re-interpolated
            {
                cv::Mat med, d;
                cv::medianBlur(zf, med, 5);
                cv::absdiff(zf, med, d);
                cv::Mat spike = (d > o.spike_mm) & valid & (rho > 0.01);
                if (cv::countNonZero(spike) > 0)
                {
                    valid &= ~spike;
                    zf = src.clone();
                    zf.setTo(0, ~valid);
                    holes = B & ~valid;
                    PushPullFill(zf, valid, holes, filled);
                }
            }
            cv::Mat known = valid | filled;
            cv::Mat basis;
            cv::Mat res = ReferenceAndResidual(zf, known, p, basis);

            double low, high;
            if (o.pct_norm)
            {
                std::vector<float> v;
                v.reserve(static_cast<size_t>(cv::countNonZero(valid)));
                for (int r = 0; r < res.rows; ++r)
                {
                    const float* a = res.ptr<float>(r);
                    const uchar* m = valid.ptr<uchar>(r);
                    for (int c = 0; c < res.cols; ++c) if (m[c]) v.push_back(a[c]);
                }
                std::vector<float> v2(v);
                low = PercentileOf(v, p.lower);
                high = PercentileOf(v2, p.upper);
                if (!(high > low)) { low = -o.range_mm; high = o.range_mm; }
            }
            else
            {
                low = -o.range_mm; high = o.range_mm;
            }
            cv::Mat clipped = res.clone();
            clipped.setTo(low, clipped < low);
            clipped.setTo(high, clipped > high);
            cv::Mat out;
            clipped.convertTo(out, CV_8U, 254.0 / (high - low), 1.0 - low * 254.0 / (high - low));   // 1..255 inside the band
            out.setTo(1, out < 1);
            out.setTo(p.exp.null_value >= 0 ? std::min(p.exp.null_value, 255) : 0, ~B);

            dbg.holes = B & ~valid;
            dbg.zf = zf;
            dbg.basis = basis;
            res.convertTo(dbg.rn_um, CV_32F, 1000.0);
            clipped.convertTo(dbg.clipped_um, CV_32F, 1000.0);
            dbg.low_mm = low; dbg.high_mm = high;
            return out;
        }

        // Robust surface along the rotation axis: for every selected row, the running median (window `win` columns) of the
        // usable pixels, evaluated every `step` columns and interpolated linearly in between. Windows cut by the image border
        // use two half windows and extrapolate linearly (a plain median is biased when the profile drifts with the wobble).
        void RowRunningMedian(const cv::Mat& z, const cv::Mat& use8u, int win, int step, int minCount,
                              const std::vector<uchar>* rowSel, cv::Mat& ref, cv::Mat& known8u)
        {
            const int h = z.rows, w = z.cols;
            win = std::max(9, win | 1);
            step = std::max(1, step);
            const int half = win / 2;
            const int ng = (w - 1) / step + 2;                    // grid columns: min(g * step, w - 1)
            if (ref.empty()) ref = cv::Mat::zeros(h, w, CV_32F);
            if (known8u.empty()) known8u = cv::Mat::zeros(h, w, CV_8U);
            cv::parallel_for_(cv::Range(0, h), [&](const cv::Range& rg)
            {
                std::vector<float> buf, gv(static_cast<size_t>(ng));
                std::vector<uchar> gk(static_cast<size_t>(ng));
                buf.reserve(static_cast<size_t>(win) + 1);
                for (int r = rg.start; r < rg.end; ++r)
                {
                    if (rowSel && !(*rowSel)[static_cast<size_t>(r)]) continue;
                    const float* zr = z.ptr<float>(r);
                    const uchar* m = use8u.ptr<uchar>(r);
                    auto med = [&](int a, int b, float& out) -> bool
                    {
                        buf.clear();
                        for (int k = std::max(0, a); k <= std::min(w - 1, b); ++k) if (m[k]) buf.push_back(zr[k]);
                        if (static_cast<int>(buf.size()) < minCount) return false;
                        std::nth_element(buf.begin(), buf.begin() + buf.size() / 2, buf.end());
                        out = buf[buf.size() / 2];
                        return true;
                    };
                    for (int g = 0; g < ng; ++g)
                    {
                        const int c = std::min(g * step, w - 1);
                        float v = 0.f;
                        bool ok = false;
                        if ((c - half < 0 || c + half > w - 1) && w > 3 * half)
                        {
                            const bool left = (c - half < 0);
                            const int a0 = left ? 0 : w - 1 - half, a1 = left ? half : w - 1;
                            const int b0 = left ? half + 1 : w - 2 - 2 * half, b1 = left ? 2 * half + 1 : w - 2 - half;
                            float m1 = 0.f, m2 = 0.f;
                            if (med(a0, a1, m1) && med(b0, b1, m2))
                            {
                                const float c1 = 0.5f * (a0 + a1), c2 = 0.5f * (b0 + b1);
                                v = m1 + (m2 - m1) * (static_cast<float>(c) - c1) / (c2 - c1);
                                ok = true;
                            }
                        }
                        if (!ok) ok = med(c - half, c + half, v);
                        gv[static_cast<size_t>(g)] = v;
                        gk[static_cast<size_t>(g)] = ok ? 1 : 0;
                    }
                    float* out = ref.ptr<float>(r);
                    uchar* kn = known8u.ptr<uchar>(r);
                    for (int c = 0; c < w; ++c)
                    {
                        const int g0 = c / step, g1 = std::min(g0 + 1, ng - 1);
                        const int c0 = std::min(g0 * step, w - 1), c1 = std::min(g1 * step, w - 1);
                        const bool k0 = gk[static_cast<size_t>(g0)] != 0, k1 = gk[static_cast<size_t>(g1)] != 0;
                        if (k0 && k1)
                        {
                            const float t = (c1 > c0) ? static_cast<float>(c - c0) / static_cast<float>(c1 - c0) : 0.f;
                            out[c] = gv[static_cast<size_t>(g0)] * (1.f - t) + gv[static_cast<size_t>(g1)] * t;
                            kn[c] = 255;
                        }
                        else if (k0) { out[c] = gv[static_cast<size_t>(g0)]; kn[c] = 255; }
                        else if (k1) { out[c] = gv[static_cast<size_t>(g1)]; kn[c] = 255; }
                        else { out[c] = 0.f; kn[c] = 0; }
                    }
                }
            });
        }

        // Measured pixels (inside `where`) whose residual deviates from the median of the usable residuals in their
        // 5x5 neighbourhood by more than `thr`.
        void LocalResidualOutliers(const cv::Mat& res, const cv::Mat& use8u, const cv::Mat& where8u, double thr, cv::Mat& outlier8u)
        {
            const int h = res.rows, w = res.cols;
            outlier8u = cv::Mat::zeros(h, w, CV_8U);
            const float T = static_cast<float>(thr);
            cv::parallel_for_(cv::Range(0, h), [&](const cv::Range& rg)
            {
                float buf[25];
                for (int r = rg.start; r < rg.end; ++r)
                {
                    const uchar* wh = where8u.ptr<uchar>(r);
                    const uchar* us = use8u.ptr<uchar>(r);
                    uchar* o = outlier8u.ptr<uchar>(r);
                    for (int c = 0; c < w; ++c)
                    {
                        if (!wh[c] || !us[c]) continue;
                        int n = 0;
                        for (int dr = -2; dr <= 2; ++dr)
                        {
                            const int rr = r + dr;
                            if (rr < 0 || rr >= h) continue;
                            const float* a = res.ptr<float>(rr);
                            const uchar* u = use8u.ptr<uchar>(rr);
                            for (int dc = -2; dc <= 2; ++dc)
                            {
                                const int cc = c + dc;
                                if (cc < 0 || cc >= w || !u[cc]) continue;
                                buf[n++] = a[cc];
                            }
                        }
                        if (n < 5) { o[c] = 255; continue; }      // isolated measured pixel inside a hole: not trustworthy
                        std::nth_element(buf, buf + n / 2, buf + n);
                        if (std::fabs(res.at<float>(r, c) - buf[n / 2]) > T) o[c] = 255;
                    }
                }
            });
        }

        // Remove the row-to-row jitter of a surface that was estimated row by row (inside refilled holes it would show up as
        // horizontal streaks): symmetric vertical median (outlier rows) followed by a masked local linear fit along the
        // vertical, which keeps ramps exactly - also next to unknown pixels.
        void SmoothAcrossRows(cv::Mat& R, const cv::Mat& known8u, int medHalf, int fitHalf)
        {
            cv::Mat Rt, Kt;
            cv::transpose(R, Rt);                 // every row of Rt is one column of R -> contiguous access
            cv::transpose(known8u, Kt);
            const int n = Rt.cols;
            if (medHalf > 0 && n > 2 * medHalf + 1)
            {
                cv::Mat src = Rt.clone();
                cv::parallel_for_(cv::Range(0, Rt.rows), [&](const cv::Range& rg)
                {
                    std::vector<float> buf(static_cast<size_t>(2 * medHalf + 1));
                    std::vector<int> pre(static_cast<size_t>(n) + 1);
                    for (int i = rg.start; i < rg.end; ++i)
                    {
                        const float* a = src.ptr<float>(i);
                        const uchar* k = Kt.ptr<uchar>(i);
                        float* o = Rt.ptr<float>(i);
                        pre[0] = 0;
                        for (int j = 0; j < n; ++j) pre[static_cast<size_t>(j) + 1] = pre[static_cast<size_t>(j)] + (k[j] ? 1 : 0);
                        for (int j = medHalf; j < n - medHalf; ++j)
                        {
                            if (pre[static_cast<size_t>(j + medHalf) + 1] - pre[static_cast<size_t>(j - medHalf)] != 2 * medHalf + 1) continue;
                            std::copy(a + j - medHalf, a + j + medHalf + 1, buf.begin());
                            std::nth_element(buf.begin(), buf.begin() + medHalf, buf.end());
                            o[j] = buf[static_cast<size_t>(medHalf)];
                        }
                    }
                });
            }
            if (fitHalf > 0)
            {
                cv::Mat W, Z, Y(Rt.size(), CV_64F);
                Kt.convertTo(W, CV_64F, 1.0 / 255.0);
                Rt.convertTo(Z, CV_64F);
                Z = Z.mul(W);
                for (int i = 0; i < Y.rows; ++i)
                {
                    double* y = Y.ptr<double>(i);
                    for (int j = 0; j < n; ++j) y[j] = j - 0.5 * n;
                }
                const cv::Size ks(2 * fitHalf + 1, 1);
                cv::Mat S0, S1, S2, T0, T1, WY = W.mul(Y);
                cv::boxFilter(W, S0, CV_64F, ks, cv::Point(-1, -1), false, cv::BORDER_CONSTANT);
                cv::boxFilter(WY, S1, CV_64F, ks, cv::Point(-1, -1), false, cv::BORDER_CONSTANT);
                cv::boxFilter(WY.mul(Y), S2, CV_64F, ks, cv::Point(-1, -1), false, cv::BORDER_CONSTANT);
                cv::boxFilter(Z, T0, CV_64F, ks, cv::Point(-1, -1), false, cv::BORDER_CONSTANT);
                cv::boxFilter(Z.mul(Y), T1, CV_64F, ks, cv::Point(-1, -1), false, cv::BORDER_CONSTANT);
                for (int i = 0; i < Rt.rows; ++i)
                {
                    const double* s0 = S0.ptr<double>(i); const double* s1 = S1.ptr<double>(i); const double* s2 = S2.ptr<double>(i);
                    const double* t0 = T0.ptr<double>(i); const double* t1 = T1.ptr<double>(i); const double* y = Y.ptr<double>(i);
                    const uchar* k = Kt.ptr<uchar>(i);
                    float* o = Rt.ptr<float>(i);
                    for (int j = 0; j < n; ++j)
                    {
                        if (!k[j] || s0[j] < 2.5) continue;
                        const double my = s1[j] / s0[j], mz = t0[j] / s0[j];
                        const double var = s2[j] / s0[j] - my * my, cov = t1[j] / s0[j] - my * mz;
                        o[j] = static_cast<float>(var > 1e-6 ? mz + cov / var * (y[j] - my) : mz);
                    }
                }
            }
            cv::transpose(Rt, R);
        }

        // Wobble-registered mean cross-section:  S(r, c) = P(r - s(c)) + d(c).
        // The cross-section of a tire is the same all around; during the rotation it only shifts along the profile axis (s)
        // and in height (d). P is the median over the whole image of the registered block profiles, so it stays correct inside
        // a dropout band where a large share of the measured pixels is wrong. Returns false when the registration is not possible.
        bool RegisteredSurface(const cv::Mat& z, const cv::Mat& use8u, int block, int maxShift, cv::Mat& S, cv::Mat& knownS)
        {
            const int h = z.rows, w = z.cols;
            const int nb = w / block;
            if (nb < 8 || h < 64) return false;
            const float NaN = std::numeric_limits<float>::quiet_NaN();
            // a) block profiles: median of the usable pixels of every row over `block` columns
            cv::Mat prof(nb, h, CV_32F, cv::Scalar(NaN));
            cv::parallel_for_(cv::Range(0, nb), [&](const cv::Range& rg)
            {
                std::vector<float> buf;
                buf.reserve(static_cast<size_t>(2 * block));
                for (int b = rg.start; b < rg.end; ++b)
                {
                    const int c0 = b * block, c1 = (b == nb - 1) ? w : c0 + block;
                    float* p = prof.ptr<float>(b);
                    for (int r = 0; r < h; ++r)
                    {
                        const float* zr = z.ptr<float>(r);
                        const uchar* m = use8u.ptr<uchar>(r);
                        buf.clear();
                        for (int c = c0; c < c1; ++c) if (m[c]) buf.push_back(zr[c]);
                        if (static_cast<int>(buf.size()) * 4 < (c1 - c0)) continue;
                        std::nth_element(buf.begin(), buf.begin() + buf.size() / 2, buf.end());
                        p[r] = buf[buf.size() / 2];
                    }
                }
            });
            // b) reference block: band centre closest to the median centre, among well populated blocks
            std::vector<float> centre(static_cast<size_t>(nb), NaN);
            std::vector<int> count(static_cast<size_t>(nb), 0);
            for (int b = 0; b < nb; ++b)
            {
                const float* p = prof.ptr<float>(b);
                int first = -1, last = -1, n = 0;
                for (int r = 0; r < h; ++r) if (p[r] == p[r]) { if (first < 0) first = r; last = r; ++n; }
                count[static_cast<size_t>(b)] = n;
                if (n > 0) centre[static_cast<size_t>(b)] = 0.5f * (first + last);
            }
            std::vector<int> cs(count);
            std::nth_element(cs.begin(), cs.begin() + nb / 2, cs.end());
            const int medCount = cs[static_cast<size_t>(nb / 2)];
            if (medCount < 100) return false;
            std::vector<float> cc;
            for (int b = 0; b < nb; ++b) if (count[static_cast<size_t>(b)] * 10 >= medCount * 8) cc.push_back(centre[static_cast<size_t>(b)]);
            if (cc.empty()) return false;
            std::nth_element(cc.begin(), cc.begin() + cc.size() / 2, cc.end());
            const float medCentre = cc[cc.size() / 2];
            int refB = -1;
            float bestD = 1e30f;
            for (int b = 0; b < nb; ++b)
            {
                if (count[static_cast<size_t>(b)] * 10 < medCount * 8) continue;
                const float d = std::fabs(centre[static_cast<size_t>(b)] - medCentre);
                if (d < bestD) { bestD = d; refB = b; }
            }
            if (refB < 0) return false;
            const float* ref = prof.ptr<float>(refB);
            // c) registration of every block against the reference (coarse step 4, then +-4 fine)
            std::vector<float> sh(static_cast<size_t>(nb), NaN), dz(static_cast<size_t>(nb), NaN);
            const int minOverlap = std::max(100, medCount / 2);
            cv::parallel_for_(cv::Range(0, nb), [&](const cv::Range& rg)
            {
                std::vector<float> diff;
                diff.reserve(static_cast<size_t>(h));
                for (int b = rg.start; b < rg.end; ++b)
                {
                    const float* p = prof.ptr<float>(b);
                    auto cost = [&](int s, float& off) -> float
                    {
                        diff.clear();
                        for (int r = std::max(0, -s); r < std::min(h, h - s); ++r)
                        {
                            const float a = p[r + s], q = ref[r];
                            if (a == a && q == q) diff.push_back(a - q);
                        }
                        if (static_cast<int>(diff.size()) < minOverlap) return 1e30f;
                        std::nth_element(diff.begin(), diff.begin() + diff.size() / 2, diff.end());
                        off = diff[diff.size() / 2];
                        double acc = 0;
                        for (float v : diff) acc += std::fabs(v - off);
                        return static_cast<float>(acc / diff.size());
                    };
                    float bestC = 1e30f, bestOff = 0.f;
                    int bestS = 0;
                    for (int s = -maxShift; s <= maxShift; s += 4)
                    {
                        float off = 0.f;
                        const float c = cost(s, off);
                        if (c < bestC) { bestC = c; bestS = s; bestOff = off; }
                    }
                    const int centreS = bestS;
                    for (int s = centreS - 4; s <= centreS + 4; ++s)
                    {
                        if (s < -maxShift || s > maxShift) continue;
                        float off = 0.f;
                        const float c = cost(s, off);
                        if (c < bestC) { bestC = c; bestS = s; bestOff = off; }
                    }
                    if (bestC < 1e29f) { sh[static_cast<size_t>(b)] = static_cast<float>(bestS); dz[static_cast<size_t>(b)] = bestOff; }
                }
            });
            // d) fill failed blocks from their neighbours and remove single mis-registrations (the wobble is smooth)
            auto fillAndSmooth = [&](std::vector<float>& v) -> bool
            {
                std::vector<int> idx;
                for (int b = 0; b < nb; ++b) if (v[static_cast<size_t>(b)] == v[static_cast<size_t>(b)]) idx.push_back(b);
                if (static_cast<int>(idx.size()) * 2 < nb) return false;
                for (int b = 0; b < nb; ++b)
                {
                    if (v[static_cast<size_t>(b)] == v[static_cast<size_t>(b)]) continue;
                    auto it = std::lower_bound(idx.begin(), idx.end(), b);
                    if (it == idx.begin()) v[static_cast<size_t>(b)] = v[static_cast<size_t>(*it)];
                    else if (it == idx.end()) v[static_cast<size_t>(b)] = v[static_cast<size_t>(idx.back())];
                    else
                    {
                        const int r1 = *it, r0 = *(it - 1);
                        const float t = static_cast<float>(b - r0) / static_cast<float>(r1 - r0);
                        v[static_cast<size_t>(b)] = v[static_cast<size_t>(r0)] * (1.f - t) + v[static_cast<size_t>(r1)] * t;
                    }
                }
                Median1D(v, 9);
                std::vector<float> s(v);
                for (int b = 0; b < nb; ++b)
                {
                    double acc = 0; int n = 0;
                    for (int k = std::max(0, b - 3); k <= std::min(nb - 1, b + 3); ++k) { acc += s[static_cast<size_t>(k)]; ++n; }
                    v[static_cast<size_t>(b)] = static_cast<float>(acc / n);
                }
                return true;
            };
            if (!fillAndSmooth(sh) || !fillAndSmooth(dz)) return false;
            // e) registered median profile P (index t = r - s + maxShift)
            const int H = h + 2 * maxShift;
            std::vector<float> P(static_cast<size_t>(H), NaN);
            cv::parallel_for_(cv::Range(0, H), [&](const cv::Range& rg)
            {
                std::vector<float> buf;
                buf.reserve(static_cast<size_t>(nb));
                for (int t = rg.start; t < rg.end; ++t)
                {
                    buf.clear();
                    for (int b = 0; b < nb; ++b)
                    {
                        const int r = t - maxShift + static_cast<int>(std::lround(sh[static_cast<size_t>(b)]));
                        if (r < 0 || r >= h) continue;
                        const float v = prof.at<float>(b, r);
                        if (v == v) buf.push_back(v - dz[static_cast<size_t>(b)]);
                    }
                    if (static_cast<int>(buf.size()) < std::max(8, nb / 10)) continue;
                    std::nth_element(buf.begin(), buf.begin() + buf.size() / 2, buf.end());
                    P[static_cast<size_t>(t)] = buf[buf.size() / 2];
                }
            });
            // f) S(r, c): s and d interpolated between block centres, P interpolated for the fractional shift
            S = cv::Mat::zeros(h, w, CV_32F);
            knownS = cv::Mat::zeros(h, w, CV_8U);
            cv::parallel_for_(cv::Range(0, w), [&](const cv::Range& rg)
            {
                for (int c = rg.start; c < rg.end; ++c)
                {
                    const float fb = (static_cast<float>(c) + 0.5f) / static_cast<float>(block) - 0.5f;
                    const int b0 = std::min(nb - 1, std::max(0, static_cast<int>(std::floor(fb))));
                    const int b1 = std::min(nb - 1, b0 + 1);
                    const float t = std::min(1.f, std::max(0.f, fb - static_cast<float>(b0)));
                    const float s = sh[static_cast<size_t>(b0)] * (1.f - t) + sh[static_cast<size_t>(b1)] * t;
                    const float d = dz[static_cast<size_t>(b0)] * (1.f - t) + dz[static_cast<size_t>(b1)] * t;
                    for (int r = 0; r < h; ++r)
                    {
                        const float ft = static_cast<float>(r) - s + static_cast<float>(maxShift);
                        const int t0 = static_cast<int>(std::floor(ft));
                        if (t0 < 0 || t0 + 1 >= H) continue;
                        const float p0 = P[static_cast<size_t>(t0)], p1 = P[static_cast<size_t>(t0) + 1];
                        if (!(p0 == p0) || !(p1 == p1)) continue;
                        const float a = ft - static_cast<float>(t0);
                        S.at<float>(r, c) = p0 * (1.f - a) + p1 * a + d;
                        knownS.at<uchar>(r, c) = 255;
                    }
                }
            });
            return true;
        }

        // Measured pixels that belong to the real surface: every pixel on the surface (|res| <= seed) and every pixel that can be
        // reached from one of them through measured 8-neighbours whose residual changes by at most `step` per pixel.
        // A real bump or rib has continuous flanks and is reached; a cluster of wrong depths floats above / below the surface
        // (jump of several mm, or an island inside nulls) and is not.
        cv::Mat GrowFromSurface(const cv::Mat& res, const cv::Mat& use8u, const cv::Mat& where8u, double seed, double step)
        {
            const int h = res.rows, w = res.cols;
            cv::Mat reached = cv::Mat::zeros(h, w, CV_8U);
            std::vector<int> q;
            q.reserve(static_cast<size_t>(1) << 22);
            const float S = static_cast<float>(seed), G = static_cast<float>(step);
            for (int r = 0; r < h; ++r)
            {
                const float* a = res.ptr<float>(r);
                const uchar* u = use8u.ptr<uchar>(r);
                const uchar* wh = where8u.ptr<uchar>(r);
                uchar* o = reached.ptr<uchar>(r);
                for (int c = 0; c < w; ++c)
                    if (wh[c] && u[c] && std::fabs(a[c]) <= S) { o[c] = 255; q.push_back(r * w + c); }
            }
            size_t head = 0;
            while (head < q.size())
            {
                const int idx = q[head++];
                const int r = idx / w, c = idx % w;
                const float v = res.ptr<float>(r)[c];
                for (int dr = -1; dr <= 1; ++dr)
                {
                    const int rr = r + dr;
                    if (rr < 0 || rr >= h) continue;
                    const float* a = res.ptr<float>(rr);
                    const uchar* u = use8u.ptr<uchar>(rr);
                    const uchar* wh = where8u.ptr<uchar>(rr);
                    uchar* o = reached.ptr<uchar>(rr);
                    for (int dc = -1; dc <= 1; ++dc)
                    {
                        const int cc = c + dc;
                        if (cc < 0 || cc >= w || o[cc] || !u[cc] || !wh[cc]) continue;
                        if (std::fabs(a[cc] - v) <= G) { o[cc] = 255; q.push_back(rr * w + cc); }
                    }
                }
            }
            return reached;
        }

        // Restore2: restore + dropout-zone handling. Inside a dropout zone (long band of nulls along the rotation axis) the
        // sensor also returns clusters of wrong depths (several mm off). They are rejected against the running-median
        // surface, and every hole is refilled in the residual domain (z = surface + interpolated residual), so the fill
        // follows the steep wall instead of ramping between wrong values. Outside the zones it behaves like restore.
        cv::Mat ProcessRestore2(const cv::Mat& src, const cv::Mat& B, const cv::Mat& Vb, const cv::Mat& rho,
                                const SimParams& p, Debug& dbg)
        {
            const SimV2& o = p.exp.v2;
            const int h = src.rows, w = src.cols;
            // 1) dropout zones: null density over a window elongated along the rotation axis
            cv::Mat zone;
            {
                cv::Mat nullInBand = B & ~Vb, f, rz;
                nullInBand.convertTo(f, CV_32F, 1.0 / 255.0);
                cv::blur(f, rz, cv::Size(std::max(3, o.zone_w | 1), std::max(3, o.zone_h | 1)));
                zone = rz > o.zone_thr;
                if (o.zone_margin > 0)
                {
                    cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * o.zone_margin + 1, 2 * o.zone_margin + 1));
                    cv::dilate(zone, zone, k);
                }
                zone &= B;
            }

            // Outside the zones: band valid set, minus the measured pixels next to a null (mixed pixels), as in restore.
            // Inside a zone the nulls are scattered: the erode-break of the band valid set and the edge erosion would throw away
            // nearly every measured pixel there, so every measured pixel of the band region is taken back and the tests below
            // (continuity with the surface, local median) decide which ones are wrong.
            cv::Mat valid = Vb.clone();
            if (o.edge > 0)
            {
                cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * o.edge + 1, 2 * o.edge + 1));
                cv::erode(Vb, valid, k);
            }
            {
                cv::Mat measured = src > -900.0f;
                valid = (measured & zone) | (valid & ~zone);
            }

            // 2) robust surface R along the rotation axis.
            //    skeleton  : wobble-registered median cross-section S (immune to the wrong depths inside a dropout band)
            //    correction: row-wise running median of (z - S) over the pixels that agree with the skeleton, de-jittered across rows
            //    fallback  : running median of z itself with two rejection rounds (when the registration is not possible)
            cv::Mat R, knownR, res0;
            cv::Mat S, knownS;
            if (RegisteredSurface(src, valid, 16, 160, S, knownS))
            {
                cv::Mat m;
                cv::subtract(src, S, m, cv::noArray(), CV_32F);
                const double gate = std::max(2.0 * o.zout_mm, 1.5);
                cv::Mat use0 = valid & knownS & ~(zone & (cv::abs(m) > gate));
                cv::Mat C, knownC;
                RowRunningMedian(m, use0, o.rm_win, o.rm_step, 40, nullptr, C, knownC);
                SmoothAcrossRows(C, knownC, 5, 4);
                R = S + C;
                knownR = knownS & knownC;
                dbg.surface_model = true;
            }
            else
            {
                RowRunningMedian(src, valid, o.rm_win, o.rm_step, 40, nullptr, R, knownR);
                SmoothAcrossRows(R, knownR, 5, 4);
                cv::subtract(src, R, res0, cv::noArray(), CV_32F);
                std::vector<uchar> rowSel(static_cast<size_t>(h), 0);
                int nSel = 0;
                for (int r = 0; r < h; ++r)
                    if (cv::countNonZero(zone.row(r)) > 0) { rowSel[static_cast<size_t>(r)] = 1; ++nSel; }
                const double rounds[2] = { 3.0 * o.zout_mm, o.zout_mm };
                for (int it = 0; it < 2 && nSel > 0; ++it)
                {
                    cv::Mat bad = zone & valid & knownR & (cv::abs(res0) > rounds[it]);
                    if (cv::countNonZero(bad) == 0) continue;
                    cv::Mat use1 = valid & ~bad;
                    RowRunningMedian(src, use1, o.rm_win, o.rm_step, 40, &rowSel, R, knownR);
                    SmoothAcrossRows(R, knownR, 5, 4);
                    cv::subtract(src, R, res0, cv::noArray(), CV_32F);
                }
            }
            cv::subtract(src, R, res0, cv::noArray(), CV_32F);
            {   // the surface is smooth: make it known on the whole band
                cv::Mat todo = B & ~knownR, f;
                if (cv::countNonZero(todo) > 0)
                {
                    R.setTo(0, ~knownR);
                    PushPullFill(R, knownR, todo, f);
                    knownR |= f;
                    cv::subtract(src, R, res0, cv::noArray(), CV_32F);
                }
            }

            // 3) reject wrong-valued measured pixels inside the zones. A pixel is kept when it lies on the surface or is connected
            //    to it through continuous measured neighbours (real bumps / ribs); floating clusters of wrong depths are rejected.
            //    Then a local test inside the zones; elsewhere only isolated spikes (as in restore).
            cv::Mat rejected;
            if (o.zgrow_mm > 0)
            {
                cv::Mat zoneG, k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(51, 51));
                cv::dilate(zone, zoneG, k);
                zoneG &= B;
                cv::Mat reached = GrowFromSurface(res0, valid & knownR, zoneG, o.zseed_mm, o.zgrow_mm);
                rejected = zone & valid & knownR & ~reached;
            }
            else
                rejected = zone & valid & knownR & (cv::abs(res0) > o.zout_mm);
            cv::Mat use = valid & ~rejected;
            if (o.zout2_mm > 0)
            {
                cv::Mat out2;
                LocalResidualOutliers(res0, use & knownR, zone, o.zout2_mm, out2);
                rejected |= out2;
                use &= ~out2;
            }
            if (o.spike_mm > 0)
            {
                cv::Mat where = (rho > 0.01) & ~zone, out3;
                LocalResidualOutliers(res0, use & knownR, where, o.spike_mm, out3);
                rejected |= out3;
                use &= ~out3;
            }

            // 4) refill in the residual domain
            cv::Mat useR = use & knownR;
            cv::Mat resF = res0.clone();
            resF.setTo(0, ~useR);
            cv::Mat holes = B & ~use, filled;
            PushPullFill(resF, useR, holes & knownR, filled);
            cv::Mat zf = src.clone();
            zf.setTo(0, ~use);
            {
                cv::Mat zr = R + resF;
                zr.copyTo(zf, filled);
            }
            cv::Mat known = use | filled;
            {
                cv::Mat rest = B & ~known, f2;
                if (cv::countNonZero(rest) > 0) { PushPullFill(zf, known, rest, f2); known |= f2; filled |= f2; }
            }

            // 5) reference / slope-compensated residual / contrast (percentiles on trustworthy measured pixels)
            cv::Mat basis;
            cv::Mat res = ReferenceAndResidual(zf, known, p, basis);
            double low = -o.range_mm, high = o.range_mm;
            if (o.pct_norm)
            {
                cv::Mat nm = o.zone_norm ? cv::Mat(use & ~zone) : use;
                if (cv::countNonZero(nm) < 1000) nm = use;
                std::vector<float> v;
                v.reserve(static_cast<size_t>(cv::countNonZero(nm)));
                for (int r = 0; r < h; ++r)
                {
                    const float* a = res.ptr<float>(r);
                    const uchar* m = nm.ptr<uchar>(r);
                    for (int c = 0; c < w; ++c) if (m[c]) v.push_back(a[c]);
                }
                std::vector<float> v2(v);
                const double lo = PercentileOf(v, p.lower), hi = PercentileOf(v2, p.upper);
                if (hi > lo) { low = lo; high = hi; }
            }
            cv::Mat clipped = res.clone();
            clipped.setTo(low, clipped < low);
            clipped.setTo(high, clipped > high);
            cv::Mat out;
            clipped.convertTo(out, CV_8U, 254.0 / (high - low), 1.0 - low * 254.0 / (high - low));
            out.setTo(1, out < 1);
            out.setTo(p.exp.null_value >= 0 ? std::min(p.exp.null_value, 255) : 0, ~B);

            const double band = std::max(1, cv::countNonZero(B));
            dbg.holes = B & ~use;
            dbg.zone = zone;
            dbg.surface = R;
            dbg.rejected = rejected;
            dbg.zone_pct = 100.0 * cv::countNonZero(zone) / band;
            dbg.rejected_pct = 100.0 * cv::countNonZero(rejected) / band;
            dbg.filled_pct = 100.0 * cv::countNonZero(filled) / band;
            dbg.zf = zf;
            dbg.basis = basis;
            res.convertTo(dbg.rn_um, CV_32F, 1000.0);
            clipped.convertTo(dbg.clipped_um, CV_32F, 1000.0);
            dbg.low_mm = low; dbg.high_mm = high;
            return out;
        }
    }

    cv::Mat Process(const cv::Mat& src32f, const SimParams& p, Debug& dbg)
    {
        CV_Assert(src32f.type() == CV_32FC1);
        const SimV2& o = p.exp.v2;
        const int h = src32f.rows, w = src32f.cols;
        cv::Mat src = src32f.isContinuous() ? src32f : src32f.clone();

        // 1) band valid set
        cv::Mat M0 = src > -900.0f;
        cv::Mat Vb = M0.clone();
        {
            const int bk = std::max(3, p.break_kernel);
            cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(bk, bk));
            cv::Mat er, labels, stats, cent;
            cv::erode(M0, er, kernel);
            const int n = cv::connectedComponentsWithStats(er, labels, stats, cent, 8, CV_32S);
            if (n > 1)
            {
                int best = 1, area = 0;
                for (int i = 1; i < n; ++i)
                {
                    const int a = stats.at<int>(i, cv::CC_STAT_AREA);
                    if (a > area) { area = a; best = i; }
                }
                cv::Mat keep = (labels == best);
                cv::dilate(keep, keep, kernel);
                cv::bitwise_and(keep, M0, Vb);
            }
        }
        dbg.stage_removed = M0 & ~Vb;

        // 2) band region from the per-column envelope
        std::vector<float> top(static_cast<size_t>(w), -1.f), bot(static_cast<size_t>(w), -1.f);
        for (int r = 0; r < h; ++r)
        {
            const uchar* m = Vb.ptr<uchar>(r);
            for (int c = 0; c < w; ++c)
                if (m[c]) { if (top[c] < 0) top[c] = static_cast<float>(r); bot[c] = static_cast<float>(r); }
        }
        int firstCol = -1;
        for (int c = 0; c < w && firstCol < 0; ++c) if (top[c] >= 0) firstCol = c;
        cv::Mat B = cv::Mat::zeros(h, w, CV_8U);
        if (firstCol >= 0)
        {
            for (int c = 0; c < w; ++c)
                if (top[c] < 0)
                {
                    int l = c, rr = c;
                    while (l >= 0 && top[l] < 0) --l;
                    while (rr < w && top[rr] < 0) ++rr;
                    const int use = (l < 0) ? rr : (rr >= w ? l : ((c - l) <= (rr - c) ? l : rr));
                    top[c] = top[use]; bot[c] = bot[use];
                }
            Median1D(top, o.env_median);
            Median1D(bot, o.env_median);
            for (int c = 0; c < w; ++c)
            {
                const int t = std::max(0, static_cast<int>(top[c])), b = std::min(h - 1, static_cast<int>(bot[c]));
                if (b >= t) B.col(c).rowRange(t, b + 1).setTo(255);
            }
        }
        B |= Vb;
        dbg.band = B;

        // 3a) drop valid pixels next to nulls (mixed pixels)
        cv::Mat valid = Vb.clone();
        if (o.edge > 0)
        {
            cv::Mat k = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * o.edge + 1, 2 * o.edge + 1));
            cv::erode(Vb, valid, k);
        }
        {
            double mn = 0, mx = 0;
            if (cv::countNonZero(valid) > 0) cv::minMaxLoc(src, &mn, &mx, nullptr, nullptr, valid);
            dbg.z_min = mn; dbg.z_max = mx;
        }

        // 3b) local null density inside the band (dropout zones)
        cv::Mat nullInBand = B & ~Vb, rho;
        {
            cv::Mat f;
            nullInBand.convertTo(f, CV_32F, 1.0 / 255.0);
            const int win = std::max(3, o.rho_win | 1);
            cv::blur(f, rho, cv::Size(win, win));
        }
        dbg.low_mm = -o.range_mm; dbg.high_mm = o.range_mm;
        if (o.mode == 1)
            return ProcessRestore(src, B, Vb, rho, p, dbg);
        if (o.mode == 2)
            return ProcessRestore2(src, B, Vb, rho, p, dbg);

        if (o.rho_kill > 0) valid &= ~(rho > o.rho_kill);

        // 3c) interpolate small holes so that isolated dropout pixels do not punch the texture
        cv::Mat zf = src.clone();
        zf.setTo(0, ~valid);
        cv::Mat usable = valid.clone();
        if (o.fill && o.small_hole > 0)
        {
            cv::Mat holes = B & ~valid, labels, stats, cent;
            const int n = cv::connectedComponentsWithStats(holes, labels, stats, cent, 8, CV_32S);
            if (n > 1)
            {
                std::vector<uchar> small(static_cast<size_t>(n), 0);
                for (int i = 1; i < n; ++i)
                    if (stats.at<int>(i, cv::CC_STAT_AREA) <= o.small_hole) small[i] = 255;
                cv::Mat smallMask(h, w, CV_8U);
                for (int r = 0; r < h; ++r)
                {
                    const int* l = labels.ptr<int>(r);
                    uchar* s = smallMask.ptr<uchar>(r);
                    for (int c = 0; c < w; ++c) s[c] = small[l[c]];
                }
                cv::Mat filled;
                FillSmall(zf, valid, smallMask, filled);
                usable |= filled;
            }
        }

        // 4-5) reference + slope-compensated residual, then one outlier pass inside dropout zones
        cv::Mat basis;
        cv::Mat res = ReferenceAndResidual(zf, usable, p, basis);
        if (o.out_mm > 0 && o.rho_thr > 0)
        {
            cv::Mat outlier = (cv::abs(res) > o.out_mm) & (rho > o.rho_thr) & usable;
            if (cv::countNonZero(outlier) > 0)
            {
                usable &= ~outlier;
                zf.setTo(0, ~usable);
                res = ReferenceAndResidual(zf, usable, p, basis);
            }
        }

        // 6) feather: texture fades to neutral next to unusable pixels
        if (o.feather > 0)
        {
            cv::Mat wgt, f;
            usable.convertTo(f, CV_32F, 1.0 / 255.0);
            cv::GaussianBlur(f, wgt, cv::Size(0, 0), o.feather);
            wgt = (wgt - 0.5f) * 2.0f;                   // 0 at the border of a large hole, 1 well inside valid data
            cv::threshold(wgt, wgt, 0, 0, cv::THRESH_TOZERO);
            cv::threshold(wgt, wgt, 1, 1, cv::THRESH_TRUNC);
            res = res.mul(wgt);
        }
        if (o.taper > 0)                                  // optional: fade the band border itself
        {
            cv::Mat dist, t;
            cv::distanceTransform(B, dist, cv::DIST_L2, 3);
            dist.convertTo(t, CV_32F, 1.0 / o.taper);
            cv::threshold(t, t, 1, 1, cv::THRESH_TRUNC);
            res = res.mul(t);
        }

        const double R = std::max(1e-3, o.range_mm);
        cv::Mat clipped = res.clone();
        clipped.setTo(-R, clipped < -R);
        clipped.setTo(R, clipped > R);
        cv::Mat out;
        clipped.convertTo(out, CV_8U, 127.0 / R, 128.0);
        out.setTo(1, out < 1);
        out.setTo(p.exp.null_value >= 0 ? std::min(p.exp.null_value, 255) : 0, ~B);

        dbg.holes = B & ~usable;
        dbg.zf = zf;
        dbg.basis = basis;
        res.convertTo(dbg.rn_um, CV_32F, 1000.0);
        clipped.convertTo(dbg.clipped_um, CV_32F, 1000.0);
        return out;
    }
}
