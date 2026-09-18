// depth_sim.exe - offline simulator CLI for alg_depth_preproc.dll (see DESIGN.md section 2).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "DepthPreprocCore.h"
#include "IniReader.h"
#include "MimReader.h"
#include "Stats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const char* kToolVersion = "depth_sim 1.1.0 (alg_depth_preproc site PC3 2026-07-08 rc 1.0.2.0.38cf930_HT, OpenCV " CV_VERSION ")";

struct ExitError { int code; std::string msg; };

// ---------------------------------------------------------------- strings
static std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

static std::wstring Upper(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}

static void PrintErrorJson(const std::string& msg)
{
    std::string line = "{\"error\":\"" + JsonEscape(msg) + "\"}\n";
    fwrite(line.data(), 1, line.size(), stderr);
    fflush(stderr);
}

static void PrintStdout(const std::string& s)
{
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
}

static void Warn(const std::string& msg)
{
    std::string line = "{\"warning\":\"" + JsonEscape(msg) + "\"}\n";
    fwrite(line.data(), 1, line.size(), stderr);
}

// ---------------------------------------------------------------- output root
// A relative --out is resolved under (1) the DEPTH_SIM_OUT_ROOT environment variable, else
// (2) the compile-time DEPTH_SIM_DEFAULT_OUT_ROOT (<source>/Result/runs, set by CMakeLists.txt),
// else (3) <exe dir>/../../Result/runs. Absolute paths are used as given.
static fs::path DefaultOutRoot()
{
    DWORD n = GetEnvironmentVariableW(L"DEPTH_SIM_OUT_ROOT", nullptr, 0);
    if (n > 1)
    {
        std::wstring v(static_cast<size_t>(n), L'\0');
        DWORD got = GetEnvironmentVariableW(L"DEPTH_SIM_OUT_ROOT", v.data(), n);
        v.resize(got);
        if (!v.empty()) return fs::path(v);
    }
#ifdef DEPTH_SIM_DEFAULT_OUT_ROOT
    return fs::path(DEPTH_SIM_DEFAULT_OUT_ROOT);
#else
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path().parent_path().parent_path() / L"Result" / L"runs";
#endif
}

static fs::path ResolveOutDir(const std::wstring& out)
{
    fs::path p(out);
    // Drive-relative ("C:rel") or root-relative ("\\x") paths are not is_absolute(); resolve them explicitly so they
    // never escape by way of operator/ dropping the default root.
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return fs::absolute(p).lexically_normal();
    return (DefaultOutRoot() / p).lexically_normal();
}

// ---------------------------------------------------------------- options
static const std::set<std::wstring> kKnownOptions = {
    L"--in", L"--out", L"--ref", L"--folder", L"--fovproc", L"--ini", L"--cal", L"--type", L"--patch", L"--overlap",
    L"--lower", L"--upper", L"--stage", L"--roi", L"--exp-use-ini-pct", L"--exp-valid-pct", L"--exp-masked-median",
    L"--exp-null-value", L"--exp-fill-holes", L"--dump", L"--preview", L"--px-x", L"--px-y", L"--break-kernel" };

struct Options
{
    std::map<std::wstring, std::wstring> kv;
    bool Has(const wchar_t* k) const { return kv.count(k) > 0; }
    std::wstring Get(const wchar_t* k, const wchar_t* def = L"") const { auto it = kv.find(k); return it == kv.end() ? def : it->second; }
    std::wstring Require(const wchar_t* k) const
    {
        auto it = kv.find(k);
        if (it == kv.end() || it->second.empty()) throw ExitError{ 2, "missing required option " + Utf8(k) };
        return it->second;
    }
};

static Options ParseOptions(int argc, wchar_t** argv, int start)
{
    Options o;
    for (int i = start; i < argc; ++i)
    {
        std::wstring k = argv[i];
        if (k.rfind(L"--", 0) != 0 || !kKnownOptions.count(k)) throw ExitError{ 2, "unknown option " + Utf8(k) };
        if (i + 1 >= argc) throw ExitError{ 2, "option " + Utf8(k) + " needs a value" };
        o.kv[k] = argv[++i];
    }
    return o;
}

static int ParseIntStrict(const std::wstring& s, const char* what)
{
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (s.empty() || !end || *end != 0) throw ExitError{ 2, std::string("invalid integer for ") + what + ": " + Utf8(s) };
    return static_cast<int>(v);
}

static double ParseDoubleStrict(const std::wstring& s, const char* what)
{
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (s.empty() || !end || *end != 0) throw ExitError{ 2, std::string("invalid number for ") + what + ": " + Utf8(s) };
    return v;
}

static bool ParseFlag01(const std::wstring& s, const char* what)
{
    if (s == L"0") return false;
    if (s == L"1") return true;
    throw ExitError{ 2, std::string(what) + " expects 0 or 1" };
}

// ---------------------------------------------------------------- parameter resolution
struct Resolved
{
    SimParams p;
    std::string type = "INSHOULDER";
    int cal = -1;
};

static void ApplyRecipeDefaults(Resolved& r, int cal)
{
    // Hard-coded copy of D:\AIV\MODEL\[1]TireInspect_PC3_DEPLOY\alg_depth_preproc.ini
    r.cal = cal;
    r.p.lower = 5; r.p.upper = 95; r.p.stage = STAGE_AUTO; r.p.roi = { 9999, 9999, 0, 0 };
    switch (cal)
    {
    case 1: r.type = "INNERCENTER"; r.p.patch_w = 50; r.p.patch_h = 50; r.p.overlap = static_cast<float>(0.1); break;
    case 2: r.type = "BEAD";        r.p.patch_w = 15; r.p.patch_h = 15; r.p.overlap = static_cast<float>(0.25); break;
    case 3: r.type = "INSHOULDER";  r.p.patch_w = 15; r.p.patch_h = 15; r.p.overlap = static_cast<float>(0.25); break;
    default: throw ExitError{ 2, "--cal must be 1..3 when no --ini is given (built-in recipe has CAL0001..CAL0003)" };
    }
}

static void ApplyIni(Resolved& r, const IniFile& ini, int cal)
{
    CalParams c;
    try { c = ReadCalSection(ini, cal); }
    catch (const std::exception& e) { throw ExitError{ 2, std::string("ini: ") + e.what() }; }
    r.cal = cal;
    r.type = c.type;
    r.p.patch_w = c.patch_w; r.p.patch_h = c.patch_h;
    r.p.lower = c.lower; r.p.upper = c.upper;
    r.p.overlap = c.overlap;
    r.p.stage = c.stage;
    r.p.roi = c.roi;
}

static void ApplyOverrides(Resolved& r, const Options& o)
{
    if (o.Has(L"--type"))
    {
        std::wstring t = Upper(o.Get(L"--type"));
        if (t != L"INNERCENTER" && t != L"BEAD" && t != L"INSHOULDER") throw ExitError{ 2, "--type must be INNERCENTER|BEAD|INSHOULDER" };
        r.type = Utf8(t);
    }
    if (o.Has(L"--patch"))
    {
        int w = 0, h = 0;
        if (!ParseIntPair(Utf8(o.Get(L"--patch")), w, h) || w < 1 || h < 1)
            throw ExitError{ 2, "invalid --patch '" + Utf8(o.Get(L"--patch")) + "'; expected W,H with positive integers" };
        r.p.patch_w = w; r.p.patch_h = h;
    }
    if (o.Has(L"--overlap"))
    {
        double v = ParseDoubleStrict(o.Get(L"--overlap"), "--overlap");
        float f = static_cast<float>(v);
        if (f <= 0.0f || f >= 1.0f) throw ExitError{ 2, "invalid --overlap; expected range (0, 1)" };
        r.p.overlap = f;   // float widened to double, like the DLL
    }
    if (o.Has(L"--lower"))
    {
        double v = ParseDoubleStrict(o.Get(L"--lower"), "--lower");
        if (v < 0 || v > 100) throw ExitError{ 2, "--lower must be within [0,100]" };
        r.p.lower = v;
    }
    if (o.Has(L"--upper"))
    {
        double v = ParseDoubleStrict(o.Get(L"--upper"), "--upper");
        if (v < 0 || v > 100) throw ExitError{ 2, "--upper must be within [0,100]" };
        r.p.upper = v;
    }
    if (o.Has(L"--stage"))
    {
        std::wstring s = Upper(o.Get(L"--stage"));
        if (s != L"AUTO" && s != L"TOP" && s != L"BOTTOM" && s != L"NONE") throw ExitError{ 2, "--stage must be AUTO|TOP|BOTTOM|NONE" };
        r.p.stage = StageFromString(Utf8(s));
    }
    if (o.Has(L"--roi"))
    {
        int a, b, c, d;
        if (!ParseIntQuad(Utf8(o.Get(L"--roi")), a, b, c, d)) throw ExitError{ 2, "invalid --roi; expected x1,y1,x2,y2" };
        r.p.roi = { a, b, c, d };
    }
    if (o.Has(L"--exp-use-ini-pct")) r.p.exp.use_ini_pct = ParseFlag01(o.Get(L"--exp-use-ini-pct"), "--exp-use-ini-pct");
    if (o.Has(L"--exp-valid-pct")) r.p.exp.valid_pct = ParseFlag01(o.Get(L"--exp-valid-pct"), "--exp-valid-pct");
    if (o.Has(L"--exp-masked-median")) r.p.exp.masked_median = ParseFlag01(o.Get(L"--exp-masked-median"), "--exp-masked-median");
    if (o.Has(L"--exp-null-value"))
    {
        int v = ParseIntStrict(o.Get(L"--exp-null-value"), "--exp-null-value");
        if (v < -1 || v > 255) throw ExitError{ 2, "--exp-null-value must be -1..255" };
        r.p.exp.null_value = v;
    }
    if (o.Has(L"--exp-fill-holes"))
    {
        int v = ParseIntStrict(o.Get(L"--exp-fill-holes"), "--exp-fill-holes");
        if (v < 0) throw ExitError{ 2, "--exp-fill-holes must be >= 0" };
        r.p.exp.fill_holes = v;
    }
    if (o.Has(L"--break-kernel"))
    {
        int v = ParseIntStrict(o.Get(L"--break-kernel"), "--break-kernel");
        if (v < 0 || v > 99) throw ExitError{ 2, "--break-kernel must be 0..99 (site build uses 3; <3 disables the erosion)" };
        r.p.break_kernel = v;
    }
    if (r.p.exp.use_ini_pct && !(r.p.lower < r.p.upper)) throw ExitError{ 2, "--lower must be smaller than --upper" };

    // DepthPreprocType selects the stage-removal step in the site build (INNERCENTER none / BEAD / INSHOULDER).
    r.p.type = (r.type == "INNERCENTER") ? TYPE_INNERCENTER : (r.type == "BEAD") ? TYPE_BEAD : TYPE_INSHOULDER;
}

// ---------------------------------------------------------------- common run settings
struct RunSettings
{
    bool dumpAll = true;
    int preview = 8;
    double pxX = 300, pxY = 100;
    bool pxSpecified = false;
};

static RunSettings ReadRunSettings(const Options& o)
{
    RunSettings s;
    if (o.Has(L"--dump"))
    {
        std::wstring d = o.Get(L"--dump");
        if (d == L"min") s.dumpAll = false;
        else if (d == L"all") s.dumpAll = true;
        else throw ExitError{ 2, "--dump must be min|all" };
    }
    if (o.Has(L"--preview"))
    {
        s.preview = ParseIntStrict(o.Get(L"--preview"), "--preview");
        if (s.preview < 1) throw ExitError{ 2, "--preview must be >= 1" };
    }
    if (o.Has(L"--px-x")) { s.pxX = ParseDoubleStrict(o.Get(L"--px-x"), "--px-x"); s.pxSpecified = true; }
    if (o.Has(L"--px-y")) { s.pxY = ParseDoubleStrict(o.Get(L"--px-y"), "--px-y"); s.pxSpecified = true; }
    if (s.pxX <= 0 || s.pxY <= 0) throw ExitError{ 2, "--px-x/--px-y must be positive" };
    return s;
}

// ---------------------------------------------------------------- image I/O helpers
static void WriteImage(const fs::path& p, const cv::Mat& m)
{
    std::vector<unsigned char> buf;
    if (!cv::imencode(".png", m, buf)) throw ExitError{ 4, "imencode failed for " + Utf8(p.wstring()) };
    std::string err;
    if (!Mim::WriteFileBytes(p.wstring(), buf.data(), buf.size(), err)) throw ExitError{ 4, err + ": " + Utf8(p.wstring()) };
}

static void WriteF32Raw(const fs::path& p, const cv::Mat& m32f)
{
    CV_Assert(m32f.type() == CV_32FC1);
    cv::Mat c = m32f.isContinuous() ? m32f : m32f.clone();
    std::string err;
    if (!Mim::WriteFileBytes(p.wstring(), c.data, c.total() * sizeof(float), err)) throw ExitError{ 4, err + ": " + Utf8(p.wstring()) };
}

static void WriteText(const fs::path& p, const std::string& s)
{
    std::string err;
    if (!Mim::WriteFileBytes(p.wstring(), s.data(), s.size(), err)) throw ExitError{ 4, err + ": " + Utf8(p.wstring()) };
}

static cv::Mat Preview(const cv::Mat& m, int n)
{
    if (n <= 1 || m.empty()) return m;
    cv::Mat out;
    cv::resize(m, out, cv::Size(std::max(1, m.cols / n), std::max(1, m.rows / n)), 0, 0, cv::INTER_AREA);
    return out;
}

static cv::Mat MakeRawVis(const cv::Mat& raw32f, const cv::Mat& nullMask)
{
    std::vector<float> vals;
    vals.reserve(raw32f.total());
    for (int y = 0; y < raw32f.rows; ++y)
    {
        const float* p = raw32f.ptr<float>(y);
        const uchar* m = nullMask.ptr<uchar>(y);
        for (int x = 0; x < raw32f.cols; ++x) if (!m[x]) vals.push_back(p[x]);
    }
    double p1 = 0, p99 = 1;
    if (!vals.empty())
    {
        size_t i1 = static_cast<size_t>(0.01 * (vals.size() - 1));
        size_t i99 = static_cast<size_t>(0.99 * (vals.size() - 1));
        std::nth_element(vals.begin(), vals.begin() + i1, vals.end());
        p1 = vals[i1];
        std::nth_element(vals.begin(), vals.begin() + i99, vals.end());
        p99 = vals[i99];
    }
    const double range = (p99 > p1) ? (p99 - p1) : 1.0;
    cv::Mat vis(raw32f.size(), CV_8UC3);
    for (int y = 0; y < raw32f.rows; ++y)
    {
        const float* p = raw32f.ptr<float>(y);
        const uchar* m = nullMask.ptr<uchar>(y);
        cv::Vec3b* o = vis.ptr<cv::Vec3b>(y);
        for (int x = 0; x < raw32f.cols; ++x)
        {
            if (m[x]) { o[x] = cv::Vec3b(0, 0, 255); continue; }
            uchar g = cv::saturate_cast<uchar>((p[x] - p1) / range * 255.0);
            o[x] = cv::Vec3b(g, g, g);
        }
    }
    return vis;
}

static cv::Mat MakeDiffPng(const cv::Mat& diff32f)
{
    cv::Mat out(diff32f.size(), CV_8UC1);
    for (int y = 0; y < diff32f.rows; ++y)
    {
        const float* d = diff32f.ptr<float>(y);
        uchar* o = out.ptr<uchar>(y);
        for (int x = 0; x < diff32f.cols; ++x)
            o[x] = cv::saturate_cast<uchar>((static_cast<double>(d[x]) + 1000.0) / 2000.0 * 255.0);
    }
    return out;
}

static cv::Mat MakeClippedPng(const cv::Mat& clipped32f, double low, double high)
{
    cv::Mat out(clipped32f.size(), CV_8UC1, cv::Scalar(0));
    if (!(high > low)) return out;
    const double k = 255.0 / (high - low);
    for (int y = 0; y < clipped32f.rows; ++y)
    {
        const float* d = clipped32f.ptr<float>(y);
        uchar* o = out.ptr<uchar>(y);
        for (int x = 0; x < clipped32f.cols; ++x)
            o[x] = cv::saturate_cast<uchar>((static_cast<double>(d[x]) - low) * k);
    }
    return out;
}

static cv::Mat To16U(const cv::Mat& f32)
{
    cv::Mat out;
    f32.convertTo(out, CV_16U);   // saturates 0..65535, rounds
    return out;
}

static cv::Mat LoadInput(const std::wstring& path, MimInfo& info, double& readMs)
{
    auto t0 = std::chrono::steady_clock::now();
    cv::Mat img;
    std::string err;
    if (!fs::exists(fs::path(path))) throw ExitError{ 3, "input not found: " + Utf8(path) };
    if (!Mim::ReadAsFloat(path, img, info, err)) throw ExitError{ 3, "cannot read input: " + err + " (" + Utf8(path) + ")" };
    readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return img;
}

static cv::Mat LoadRef8u(const std::wstring& path)
{
    if (!fs::exists(fs::path(path))) throw ExitError{ 3, "reference not found: " + Utf8(path) };
    cv::Mat f; MimInfo info; std::string err;
    if (!Mim::ReadAsFloat(path, f, info, err)) throw ExitError{ 3, "cannot read reference: " + err + " (" + Utf8(path) + ")" };
    cv::Mat r8;
    f.convertTo(r8, CV_8U);       // DLL output buffer holds 0..255 as float; round to 8U
    return r8;
}

// ---------------------------------------------------------------- one run
struct RunRequest
{
    std::wstring input, ref;
    fs::path outDir;
    Resolved rp;
    RunSettings settings;
    bool strictRef = true;   // run: exit 3 on incompatible ref, batch: warn and skip
};

struct RunOutcome
{
    std::string statsJson;
    RunStats stats;
    int width = 0, height = 0;
};

static RunOutcome RunOne(const RunRequest& req)
{
    MimInfo info;
    double readMs = 0;
    cv::Mat raw = LoadInput(req.input, info, readMs);
    // The verbatim patchBasedMedian reads rows/cols up to the patch size without bounds checks;
    // a patch larger than the (ROI-clipped) view would read out of bounds in the DLL as well. Refuse it.
    cv::Rect eff(0, 0, raw.cols, raw.rows);
    if (req.rp.p.UseRoi())
    {
        const auto& q = req.rp.p.roi;
        eff = cv::Rect(q[0], q[1], q[2] - q[0], q[3] - q[1]) & eff;
        if (eff.width <= 0 || eff.height <= 0) throw ExitError{ 2, "ROI does not intersect the image" };
    }
    if (req.rp.p.patch_w > eff.width || req.rp.p.patch_h > eff.height)
        throw ExitError{ 2, "patch size exceeds the image (or ROI) size: patch " + std::to_string(req.rp.p.patch_w) + "x"
            + std::to_string(req.rp.p.patch_h) + " vs " + std::to_string(eff.width) + "x" + std::to_string(eff.height) };

    cv::Mat ref8u;
    if (!req.ref.empty())
    {
        ref8u = LoadRef8u(req.ref);
        if (ref8u.size() != raw.size())
        {
            std::string m = "reference size differs from input (" + std::to_string(ref8u.cols) + "x" + std::to_string(ref8u.rows) + " vs "
                + std::to_string(raw.cols) + "x" + std::to_string(raw.rows) + ")";
            if (req.strictRef) throw ExitError{ 3, m };
            Warn(m + "; reference ignored");
            ref8u.release();
        }
    }

    SimResult res;
    try { SimPipeline::Run(raw, req.rp.p, res); }
    catch (const cv::Exception& e) { throw ExitError{ 4, std::string("processing failed (OpenCV): ") + e.what() }; }
    catch (const std::exception& e) { throw ExitError{ 4, std::string("processing failed: ") + e.what() }; }
    if (!res.capture_identical)
        Warn("captured pipeline differs from processHighCurvature output; intermediates may not match the result");

    RunStats stats = ComputeRunStats(raw, res);
    if (!ref8u.empty())
    {
        CompareWithRef(res.result8u, ref8u, stats.ref);
        stats.ref.path_utf8 = Utf8(req.ref);
    }

    StatsContext ctx;
    ctx.tool_version = kToolVersion;
    ctx.input_path_utf8 = Utf8(req.input);
    ctx.out_dir_utf8 = Utf8(req.outDir.wstring());
    ctx.width = raw.cols; ctx.height = raw.rows;
    ctx.dtype = info.dtype;
    ctx.type = req.rp.type;
    ctx.params = req.rp.p;
    ctx.result = &res;
    ctx.stats = &stats;
    ctx.read_ms = readMs;
    ctx.px_x_um = req.settings.pxX; ctx.px_y_um = req.settings.pxY; ctx.px_specified = req.settings.pxSpecified;
    std::string json = BuildStatsJson(ctx);

    std::error_code ec;
    fs::create_directories(req.outDir, ec);
    if (!fs::is_directory(req.outDir)) throw ExitError{ 4, "cannot create output directory: " + Utf8(req.outDir.wstring()) };
    const fs::path& d = req.outDir;
    const int n = req.settings.preview;

    WriteImage(d / L"result.png", res.result8u);
    WriteImage(d / L"preview_result.png", Preview(res.result8u, n));
    if (req.settings.dumpAll)
    {
        cv::Mat result32f;
        res.result8u.convertTo(result32f, CV_32F);
        std::string err;
        if (!Mim::WriteF32((d / L"result_f32.tif").wstring(), result32f, err)) throw ExitError{ 4, err };

        cv::Mat rawVis = MakeRawVis(raw, res.null_mask_before);
        cv::Mat scaled16 = To16U(res.scaled), basis16 = To16U(res.basis);
        cv::Mat diffPng = MakeDiffPng(res.diff_f32);
        cv::Mat clippedPng = MakeClippedPng(res.clipped, res.clip_low, res.clip_high);

        WriteImage(d / L"raw_vis.png", rawVis);                       WriteImage(d / L"preview_raw_vis.png", Preview(rawVis, n));
        WriteImage(d / L"null_mask.png", res.null_mask_before);       WriteImage(d / L"preview_null_mask.png", Preview(res.null_mask_before, n));
        WriteImage(d / L"stage_removed_mask.png", res.stage_removed_mask); WriteImage(d / L"preview_stage_removed_mask.png", Preview(res.stage_removed_mask, n));
        WriteImage(d / L"scaled.png", scaled16);                      WriteImage(d / L"preview_scaled.png", Preview(scaled16, n));
        WriteImage(d / L"basis.png", basis16);                        WriteImage(d / L"preview_basis.png", Preview(basis16, n));
        WriteImage(d / L"diff.png", diffPng);                         WriteImage(d / L"preview_diff.png", Preview(diffPng, n));
        WriteImage(d / L"clipped.png", clippedPng);                   WriteImage(d / L"preview_clipped.png", Preview(clippedPng, n));
        if (!ref8u.empty())
        {
            cv::Mat ad, refDiff;
            cv::absdiff(res.result8u, ref8u, ad);
            ad.convertTo(refDiff, CV_8U, 8.0);
            WriteImage(d / L"ref.png", ref8u);                        WriteImage(d / L"preview_ref.png", Preview(ref8u, n));
            WriteImage(d / L"ref_diff.png", refDiff);                 WriteImage(d / L"preview_ref_diff.png", Preview(refDiff, n));
        }
        WriteF32Raw(d / L"raw.f32", raw);
        WriteF32Raw(d / L"basis.f32", res.basis);
        WriteF32Raw(d / L"diff.f32", res.diff_f32);
    }
    WriteText(d / L"stats.json", json);

    RunOutcome out;
    out.statsJson = json;
    out.stats = stats;
    out.width = raw.cols; out.height = raw.rows;
    return out;
}

// ---------------------------------------------------------------- commands
static Resolved ResolveParams(const Options& o)
{
    Resolved r;
    r.p.overlap = static_cast<float>(0.25);   // recipe default (DLL default without an ini key would be 0.5)
    const bool hasIni = o.Has(L"--ini"), hasCal = o.Has(L"--cal");
    if (hasIni)
    {
        IniFile ini; std::string err;
        if (!ini.Load(o.Get(L"--ini"), err)) throw ExitError{ 3, "cannot read ini: " + err + " (" + Utf8(o.Get(L"--ini")) + ")" };
        if (!hasCal)
            fputs("{\"warning\":\"--ini given without --cal; loading [CAL0001] (INNERCENTER). Pass --cal N to pick the section.\"}\n", stderr);
        ApplyIni(r, ini, hasCal ? ParseIntStrict(o.Get(L"--cal"), "--cal") : 1);
    }
    else if (hasCal)
        ApplyRecipeDefaults(r, ParseIntStrict(o.Get(L"--cal"), "--cal"));
    ApplyOverrides(r, o);
    return r;
}

static int CmdRun(const Options& o)
{
    RunRequest req;
    req.input = o.Require(L"--in");
    req.outDir = ResolveOutDir(o.Require(L"--out"));
    req.ref = o.Get(L"--ref");
    req.rp = ResolveParams(o);
    req.settings = ReadRunSettings(o);
    req.strictRef = true;
    RunOutcome out = RunOne(req);
    PrintStdout(out.statsJson);
    return 0;
}

static int CmdInfo(const Options& o)
{
    std::wstring in = o.Require(L"--in");
    MimInfo info; double readMs = 0;
    cv::Mat img = LoadInput(in, info, readMs);
    cv::Mat nullMask, validMask;
    cv::compare(img, -999, nullMask, cv::CMP_EQ);
    cv::compare(nullMask, 0, validMask, cv::CMP_EQ);
    const long long nullCount = cv::countNonZero(nullMask);
    double vmin = 0, vmax = 0, mn = 0, mx = 0;
    cv::minMaxLoc(img, &mn, &mx);
    if (cv::countNonZero(validMask) > 0) cv::minMaxLoc(img, &vmin, &vmax, nullptr, nullptr, validMask);
    std::string j = "{\"path\": \"" + JsonEscape(Utf8(in)) + "\", \"width\": " + JsonNum(img.cols) + ", \"height\": " + JsonNum(img.rows)
        + ", \"dtype\": \"" + info.dtype + "\", \"source\": \"" + info.source + "\", \"bits\": " + JsonNum(info.bits)
        + ", \"sample_format\": " + JsonNum(info.sampleFormat) + ", \"compression\": " + JsonNum(info.compression)
        + ", \"strips\": " + JsonNum(info.strips) + ", \"rows_per_strip\": " + JsonNum(info.rowsPerStrip)
        + ", \"software\": \"" + JsonEscape(info.software) + "\", \"null_count\": " + JsonNum(nullCount)
        + ", \"null_pct\": " + JsonNum(img.total() ? 100.0 * nullCount / static_cast<double>(img.total()) : 0.0)
        + ", \"valid_min\": " + JsonNum(vmin) + ", \"valid_max\": " + JsonNum(vmax) + ", \"min\": " + JsonNum(mn) + ", \"max\": " + JsonNum(mx)
        + ", \"read_ms\": " + JsonNum(readMs) + "}\n";
    PrintStdout(j);
    return 0;
}

struct BatchItem
{
    int imgIdx = 0;
    std::wstring name;        // stem without the leading index
    fs::path input, ref;
    int cal = -1;
};

static bool ParseLeadingIndex(const std::wstring& stem, int& idx, std::wstring& rest)
{
    size_t i = 0;
    while (i < stem.size() && iswdigit(stem[i])) ++i;
    if (i == 0 || i >= stem.size() || stem[i] != L'_') return false;
    idx = _wtoi(stem.substr(0, i).c_str());
    rest = stem.substr(i + 1);
    return true;
}

static int CmdBatch(const Options& o)
{
    fs::path folder(o.Require(L"--folder"));
    fs::path outDir = ResolveOutDir(o.Require(L"--out"));
    if (!fs::is_directory(folder)) throw ExitError{ 3, "folder not found: " + Utf8(folder.wstring()) };

    std::map<int, int> refMap = { {1, 9}, {3, 10}, {5, 11}, {7, 12}, {13, 21}, {15, 22}, {17, 23}, {19, 24} };
    std::map<int, int> calMap = { {1, 1}, {13, 1}, {3, 2}, {15, 2}, {5, 3}, {7, 3}, {17, 3}, {19, 3} };
    if (o.Has(L"--fovproc"))
    {
        IniFile ini; std::string err;
        if (!ini.Load(o.Get(L"--fovproc"), err)) throw ExitError{ 3, "cannot read FOVPROC.ini: " + err };
        refMap.clear(); calMap.clear();
        for (const auto& e : ReadFovProc(ini))
        {
            if (e.requireImgIdx < 0) continue;
            if (e.resultImgIdx >= 0) refMap[e.requireImgIdx] = e.resultImgIdx;
            if (e.paramIdx >= 0) calMap[e.requireImgIdx] = e.paramIdx;
        }
    }

    // Collect depth inputs (*_D.mim without _Proc) and index the reference files.
    std::vector<BatchItem> items;
    std::map<int, fs::path> procByIdx;
    for (const auto& de : fs::directory_iterator(folder))
    {
        if (!de.is_regular_file()) continue;
        const fs::path p = de.path();
        if (!Mim::IsMimExt(p.wstring())) continue;
        const std::wstring stem = p.stem().wstring();
        int idx; std::wstring rest;
        if (!ParseLeadingIndex(stem, idx, rest)) continue;
        const bool isProc = Upper(stem).find(L"_PROC") != std::wstring::npos;
        if (isProc) { procByIdx[idx] = p; continue; }
        if (Upper(stem).size() < 2 || Upper(stem).substr(Upper(stem).size() - 2) != L"_D") continue;
        BatchItem it; it.imgIdx = idx; it.name = rest; it.input = p;
        items.push_back(it);
    }
    std::sort(items.begin(), items.end(), [](const BatchItem& a, const BatchItem& b) { return a.imgIdx < b.imgIdx; });
    if (items.empty()) throw ExitError{ 3, "no *_D.mim depth images found in " + Utf8(folder.wstring()) };

    RunSettings settings = ReadRunSettings(o);
    IniFile ini; bool hasIni = o.Has(L"--ini");
    if (hasIni)
    {
        std::string err;
        if (!ini.Load(o.Get(L"--ini"), err)) throw ExitError{ 3, "cannot read ini: " + err };
    }
    const bool forceCal = o.Has(L"--cal");
    const int forcedCal = forceCal ? ParseIntStrict(o.Get(L"--cal"), "--cal") : -1;

    std::error_code ec;
    fs::create_directories(outDir, ec);
    std::string json = "{\"tool_version\": \"" + JsonEscape(kToolVersion) + "\", \"folder\": \"" + JsonEscape(Utf8(folder.wstring()))
        + "\", \"out_dir\": \"" + JsonEscape(Utf8(outDir.wstring())) + "\", \"items\": [\n";
    int failures = 0;
    for (size_t i = 0; i < items.size(); ++i)
    {
        BatchItem& it = items[i];
        auto rm = refMap.find(it.imgIdx);
        if (rm != refMap.end() && procByIdx.count(rm->second)) it.ref = procByIdx[rm->second];
        auto cm = calMap.find(it.imgIdx);
        it.cal = forceCal ? forcedCal : (cm != calMap.end() ? cm->second : -1);

        RunRequest req;
        req.input = it.input.wstring();
        req.ref = it.ref.wstring();
        req.outDir = outDir / (std::to_wstring(it.imgIdx) + L"_" + it.name);
        req.settings = settings;
        req.strictRef = false;
        std::string entry = "{\"imgIdx\": " + JsonNum(it.imgIdx) + ", \"name\": \"" + JsonEscape(Utf8(it.name)) + "\", \"input\": \"" + JsonEscape(Utf8(req.input))
            + "\", \"ref\": " + (it.ref.empty() ? std::string("null") : "\"" + JsonEscape(Utf8(req.ref)) + "\"")
            + ", \"cal\": " + JsonNum(it.cal) + ", \"out_dir\": \"" + JsonEscape(Utf8(req.outDir.wstring())) + "\", ";
        try
        {
            req.rp.p.overlap = static_cast<float>(0.25);
            if (hasIni && it.cal > 0) ApplyIni(req.rp, ini, it.cal);
            else if (it.cal > 0) ApplyRecipeDefaults(req.rp, it.cal);
            ApplyOverrides(req.rp, o);
            RunOutcome out = RunOne(req);
            entry += "\"stats\": " + out.statsJson + "}";
        }
        catch (const ExitError& e)
        {
            ++failures;
            Warn("imgIdx " + std::to_string(it.imgIdx) + ": " + e.msg);
            entry += "\"error\": \"" + JsonEscape(e.msg) + "\", \"stats\": null}";
        }
        json += entry + (i + 1 < items.size() ? ",\n" : "\n");
    }
    json += "]}\n";
    WriteText(outDir / L"batch.json", json);
    PrintStdout(json);
    return failures ? 4 : 0;
}

static void Usage()
{
    const char* u =
        "depth_sim.exe run   --in <file> --out <dir> [--ref <file>] [options]\n"
        "depth_sim.exe batch --folder <tire folder> --out <dir> [--fovproc <FOVPROC.ini>] [options]\n"
        "depth_sim.exe info  --in <file>\n"
        "depth_sim.exe version\n"
        "options: --ini <ini> --cal <N> --type INNERCENTER|BEAD|INSHOULDER (selects the stage-removal step like the site DLL)\n"
        "         --patch W,H --overlap F --lower P --upper P --stage AUTO|TOP|BOTTOM|NONE --break-kernel N (site: 3)\n"
        "         --roi x1,y1,x2,y2 --exp-use-ini-pct 0|1 --exp-valid-pct 0|1 --exp-masked-median 0|1 --exp-null-value N\n"
        "         --exp-fill-holes <maxpx> --dump min|all --preview N --px-x um --px-y um\n";
    fputs(u, stderr);
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc < 2) { Usage(); throw ExitError{ 2, "missing command" }; }
        std::wstring cmd = argv[1];
        if (cmd == L"version") { PrintStdout(std::string(kToolVersion) + "\n"); return 0; }
        Options o = ParseOptions(argc, argv, 2);
        if (cmd == L"run") return CmdRun(o);
        if (cmd == L"info") return CmdInfo(o);
        if (cmd == L"batch") return CmdBatch(o);
        Usage();
        throw ExitError{ 2, "unknown command " + Utf8(cmd) };
    }
    catch (const ExitError& e) { PrintErrorJson(e.msg); return e.code; }
    catch (const cv::Exception& e) { PrintErrorJson(std::string("OpenCV exception: ") + e.what()); return 4; }
    catch (const std::exception& e) { PrintErrorJson(std::string("exception: ") + e.what()); return 4; }
}
