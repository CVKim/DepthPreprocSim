#pragma once
// MIL .mim reader/writer. A .mim is a baseline little-endian TIFF written by libtiff.

#include <opencv2/core.hpp>
#include <string>
#include <vector>

struct MimInfo
{
    int width = 0, height = 0;
    int bits = 0;              // BitsPerSample
    int sampleFormat = 1;      // 1 uint, 2 int, 3 float
    int compression = 1;
    int samples = 1;
    int photometric = 1;
    int strips = 0;
    int rowsPerStrip = 0;
    std::string dtype;         // "float32" | "uint8" | "uint16" | "int16" | ...
    std::string software;      // tag 305 if present
    std::string source;        // "mim" | "imdecode"
};

namespace Mim
{
    bool IsMimExt(const std::wstring& path);
    bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes, std::string& err);
    bool WriteFileBytes(const std::wstring& path, const void* data, size_t size, std::string& err);

    // Parses .mim tags 256,257,258,259,262,273,277,278,279,339 (+305). Output keeps the native depth.
    bool Read(const std::wstring& path, cv::Mat& out, MimInfo& info, std::string& err);

    // Single-strip float32 TIFF (SampleFormat=3). MIL compatibility is not guaranteed.
    bool WriteF32(const std::wstring& path, const cv::Mat& img32f, std::string& err);

    // .mim -> Read; other extensions -> cv::imdecode(IMREAD_UNCHANGED). Result is CV_32FC1,
    // converted like the DLL does (convertTo for 8/16-bit, color reduced to gray first).
    bool ReadAsFloat(const std::wstring& path, cv::Mat& out32f, MimInfo& info, std::string& err);
}
