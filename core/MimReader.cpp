#include "MimReader.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>
#include <sstream>

namespace
{
    struct FileCloser { void operator()(FILE* f) const { if (f) fclose(f); } };
    using FilePtr = std::unique_ptr<FILE, FileCloser>;

    FilePtr OpenRead(const std::wstring& path) { return FilePtr(_wfopen(path.c_str(), L"rb")); }

    bool ReadAt(FILE* f, uint64_t off, void* dst, size_t n)
    {
        if (_fseeki64(f, static_cast<long long>(off), SEEK_SET) != 0) return false;
        return fread(dst, 1, n, f) == n;
    }

    uint16_t U16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
    uint32_t U32(const unsigned char* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); }

    size_t TypeSize(uint16_t type)
    {
        switch (type)
        {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
        }
    }

    struct Entry { uint16_t tag, type; uint32_t count; unsigned char raw[4]; };

    // Reads the integer array of an entry (SHORT or LONG), inline or at offset.
    bool ReadIntArray(FILE* f, const Entry& e, std::vector<uint32_t>& out)
    {
        const size_t ts = TypeSize(e.type);
        if (ts == 0 || (e.type != 3 && e.type != 4 && e.type != 8 && e.type != 9 && e.type != 1)) return false;
        const size_t total = ts * e.count;
        std::vector<unsigned char> buf(total);
        if (total <= 4) memcpy(buf.data(), e.raw, total);
        else if (!ReadAt(f, U32(e.raw), buf.data(), total)) return false;
        out.resize(e.count);
        for (uint32_t i = 0; i < e.count; ++i)
        {
            const unsigned char* p = buf.data() + i * ts;
            out[i] = (ts == 1) ? p[0] : (ts == 2) ? U16(p) : U32(p);
        }
        return true;
    }

    bool ReadAscii(FILE* f, const Entry& e, std::string& out)
    {
        std::vector<char> buf(e.count + 1, 0);
        if (e.count <= 4) memcpy(buf.data(), e.raw, e.count);
        else if (!ReadAt(f, U32(e.raw), buf.data(), e.count)) return false;
        out = std::string(buf.data());
        return true;
    }

    void Put16(std::vector<unsigned char>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
    void Put32(std::vector<unsigned char>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); }

    void PutEntry(std::vector<unsigned char>& v, uint16_t tag, uint16_t type, uint32_t count, uint32_t value)
    {
        Put16(v, tag); Put16(v, type); Put32(v, count);
        if (type == 3 && count == 1) { Put16(v, static_cast<uint16_t>(value)); Put16(v, 0); }
        else Put32(v, value);
    }
}

namespace Mim
{
    bool IsMimExt(const std::wstring& path)
    {
        if (path.size() < 4) return false;
        std::wstring ext = path.substr(path.size() - 4);
        for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
        return ext == L".mim";
    }

    bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes, std::string& err)
    {
        FilePtr f = OpenRead(path);
        if (!f) { err = "cannot open file"; return false; }
        _fseeki64(f.get(), 0, SEEK_END);
        long long sz = _ftelli64(f.get());
        if (sz < 0) { err = "cannot determine file size"; return false; }
        bytes.resize(static_cast<size_t>(sz));
        if (sz > 0 && !ReadAt(f.get(), 0, bytes.data(), bytes.size())) { err = "read failed"; return false; }
        return true;
    }

    bool WriteFileBytes(const std::wstring& path, const void* data, size_t size, std::string& err)
    {
        FilePtr f(_wfopen(path.c_str(), L"wb"));
        if (!f) { err = "cannot open file for writing"; return false; }
        if (size && fwrite(data, 1, size, f.get()) != size) { err = "write failed"; return false; }
        return true;
    }

    bool Read(const std::wstring& path, cv::Mat& out, MimInfo& info, std::string& err)
    {
        info = MimInfo();
        info.source = "mim";
        FilePtr f = OpenRead(path);
        if (!f) { err = "cannot open file"; return false; }

        unsigned char hdr[8];
        if (!ReadAt(f.get(), 0, hdr, 8)) { err = "file too small for a TIFF header"; return false; }
        if (hdr[0] == 'M' && hdr[1] == 'M') { err = "big-endian TIFF is not supported"; return false; }
        if (hdr[0] != 'I' || hdr[1] != 'I') { err = "not a TIFF/MIM file (bad byte-order mark)"; return false; }
        const uint16_t magic = U16(hdr + 2);
        if (magic == 43) { err = "BigTIFF is not supported"; return false; }
        if (magic != 42) { err = "not a TIFF/MIM file (bad magic)"; return false; }
        const uint32_t ifdOff = U32(hdr + 4);

        unsigned char cntBuf[2];
        if (!ReadAt(f.get(), ifdOff, cntBuf, 2)) { err = "cannot read IFD"; return false; }
        const uint16_t n = U16(cntBuf);
        std::vector<unsigned char> ifd(static_cast<size_t>(n) * 12);
        if (n == 0 || !ReadAt(f.get(), ifdOff + 2, ifd.data(), ifd.size())) { err = "cannot read IFD entries"; return false; }

        std::vector<uint32_t> stripOffsets, stripCounts, tmp;
        bool haveW = false, haveH = false, haveOffsets = false;
        for (uint16_t i = 0; i < n; ++i)
        {
            const unsigned char* p = ifd.data() + i * 12;
            Entry e; e.tag = U16(p); e.type = U16(p + 2); e.count = U32(p + 4); memcpy(e.raw, p + 8, 4);
            switch (e.tag)
            {
            case 256: if (ReadIntArray(f.get(), e, tmp) && !tmp.empty()) { info.width = static_cast<int>(tmp[0]); haveW = true; } break;
            case 257: if (ReadIntArray(f.get(), e, tmp) && !tmp.empty()) { info.height = static_cast<int>(tmp[0]); haveH = true; } break;
            case 258: if (ReadIntArray(f.get(), e, tmp) && !tmp.empty()) info.bits = static_cast<int>(tmp[0]); break;
            case 259: if (ReadIntArray(f.get(), e, tmp) && !tmp.empty()) info.compression = static_cast<int>(tmp[0]); break;
            case 262: if (ReadIntArray(f.get(), e, tmp) && !tmp.empty()) info.photometric = static_cast<int>(tmp[0]); break;
            case 273: haveOffsets = ReadIntArray(f.get(), e, stripOffsets); break;
            case 277: if (ReadIntArray(f.get(), e, tmp) && !tmp.empty()) info.samples = static_cast<int>(tmp[0]); break;
            case 278: if (ReadIntArray(f.get(), e, tmp) && !tmp.empty()) info.rowsPerStrip = static_cast<int>(tmp[0]); break;
            case 279: ReadIntArray(f.get(), e, stripCounts); break;
            case 305: ReadAscii(f.get(), e, info.software); break;
            case 339: if (ReadIntArray(f.get(), e, tmp) && !tmp.empty()) info.sampleFormat = static_cast<int>(tmp[0]); break;
            default: break;
            }
        }

        if (!haveW || !haveH || info.width <= 0 || info.height <= 0) { err = "missing ImageWidth/ImageLength"; return false; }
        if (!haveOffsets || stripOffsets.empty()) { err = "missing StripOffsets"; return false; }
        if (info.compression != 1) { err = "compressed TIFF (Compression=" + std::to_string(info.compression) + ") is not supported; only 1 (none)"; return false; }
        if (info.samples != 1) { err = "SamplesPerPixel=" + std::to_string(info.samples) + " is not supported; only single-channel images"; return false; }

        int cvType = -1;
        if (info.sampleFormat == 3 && info.bits == 32) { cvType = CV_32F; info.dtype = "float32"; }
        else if (info.sampleFormat == 1 && info.bits == 8) { cvType = CV_8U; info.dtype = "uint8"; }
        else if (info.sampleFormat == 1 && info.bits == 16) { cvType = CV_16U; info.dtype = "uint16"; }
        else if (info.sampleFormat == 2 && info.bits == 8) { cvType = CV_8S; info.dtype = "int8"; }
        else if (info.sampleFormat == 2 && info.bits == 16) { cvType = CV_16S; info.dtype = "int16"; }
        else if (info.sampleFormat == 2 && info.bits == 32) { cvType = CV_32S; info.dtype = "int32"; }
        else
        {
            err = "unsupported pixel format: SampleFormat=" + std::to_string(info.sampleFormat) + " BitsPerSample=" + std::to_string(info.bits);
            return false;
        }

        info.strips = static_cast<int>(stripOffsets.size());
        if (info.rowsPerStrip <= 0 || info.rowsPerStrip > info.height) info.rowsPerStrip = info.height;
        const size_t rowBytes = static_cast<size_t>(info.width) * (info.bits / 8);
        const int expectedStrips = (info.height + info.rowsPerStrip - 1) / info.rowsPerStrip;
        if (info.strips < expectedStrips) { err = "StripOffsets count does not cover the image"; return false; }

        out.create(info.height, info.width, cvType);
        for (int s = 0; s < expectedStrips; ++s)
        {
            const int row0 = s * info.rowsPerStrip;
            const int rows = std::min(info.rowsPerStrip, info.height - row0);
            const size_t need = rowBytes * rows;
            size_t avail = need;
            if (s < static_cast<int>(stripCounts.size())) avail = std::min<size_t>(need, stripCounts[s]);
            if (avail < need) { err = "strip " + std::to_string(s) + " is shorter than expected"; return false; }
            if (!ReadAt(f.get(), stripOffsets[s], out.ptr(row0), need)) { err = "read failed at strip " + std::to_string(s); return false; }
        }
        return true;
    }

    bool WriteF32(const std::wstring& path, const cv::Mat& img32f, std::string& err)
    {
        if (img32f.empty() || img32f.type() != CV_32FC1) { err = "WriteF32 expects CV_32FC1"; return false; }
        const uint32_t w = img32f.cols, h = img32f.rows;
        const uint32_t dataBytes = w * h * 4;
        std::vector<unsigned char> buf;
        buf.reserve(8 + dataBytes + 2 + 10 * 12 + 4 + 2);
        buf.push_back('I'); buf.push_back('I'); Put16(buf, 42);
        uint32_t ifdOff = 8 + dataBytes;
        if (ifdOff & 1) ++ifdOff;
        Put32(buf, ifdOff);
        for (int r = 0; r < img32f.rows; ++r)
        {
            const unsigned char* p = img32f.ptr<unsigned char>(r);
            buf.insert(buf.end(), p, p + w * 4);
        }
        while (buf.size() < ifdOff) buf.push_back(0);
        Put16(buf, 10);
        PutEntry(buf, 256, 4, 1, w);
        PutEntry(buf, 257, 4, 1, h);
        PutEntry(buf, 258, 3, 1, 32);
        PutEntry(buf, 259, 3, 1, 1);
        PutEntry(buf, 262, 3, 1, 1);
        PutEntry(buf, 273, 4, 1, 8);
        PutEntry(buf, 277, 3, 1, 1);
        PutEntry(buf, 278, 4, 1, h);
        PutEntry(buf, 279, 4, 1, dataBytes);
        PutEntry(buf, 339, 3, 1, 3);
        Put32(buf, 0);
        return WriteFileBytes(path, buf.data(), buf.size(), err);
    }

    bool ReadAsFloat(const std::wstring& path, cv::Mat& out32f, MimInfo& info, std::string& err)
    {
        cv::Mat native;
        if (IsMimExt(path))
        {
            if (!Read(path, native, info, err)) return false;
        }
        else
        {
            std::vector<unsigned char> bytes;
            if (!ReadFileBytes(path, bytes, err)) return false;
            native = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
            if (native.empty()) { err = "cv::imdecode failed (unsupported or corrupt image)"; return false; }
            if (native.channels() == 3) cv::cvtColor(native, native, cv::COLOR_BGR2GRAY);
            else if (native.channels() == 4) cv::cvtColor(native, native, cv::COLOR_BGRA2GRAY);
            else if (native.channels() != 1) { err = "unsupported channel count"; return false; }
            info = MimInfo();
            info.source = "imdecode";
            info.width = native.cols; info.height = native.rows; info.strips = 1; info.rowsPerStrip = native.rows;
            switch (native.depth())
            {
            case CV_8U:  info.bits = 8;  info.sampleFormat = 1; info.dtype = "uint8"; break;
            case CV_16U: info.bits = 16; info.sampleFormat = 1; info.dtype = "uint16"; break;
            case CV_8S:  info.bits = 8;  info.sampleFormat = 2; info.dtype = "int8"; break;
            case CV_16S: info.bits = 16; info.sampleFormat = 2; info.dtype = "int16"; break;
            case CV_32S: info.bits = 32; info.sampleFormat = 2; info.dtype = "int32"; break;
            case CV_32F: info.bits = 32; info.sampleFormat = 3; info.dtype = "float32"; break;
            case CV_64F: info.bits = 64; info.sampleFormat = 3; info.dtype = "float64"; break;
            default: err = "unsupported depth"; return false;
            }
        }
        if (native.type() == CV_32FC1) out32f = native;
        else native.convertTo(out32f, CV_32F);
        return true;
    }
}
