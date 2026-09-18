#include "IniReader.h"
#include "MimReader.h"   // ReadFileBytes

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace
{
    std::string Trim(const std::string& s)
    {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
        return s.substr(b, e - b);
    }

    bool IEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }

    bool IsValidUtf8(const unsigned char* p, size_t n)
    {
        size_t i = 0;
        while (i < n)
        {
            unsigned char c = p[i];
            size_t len = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (len == 0 || i + len > n) return false;
            for (size_t k = 1; k < len; ++k) if ((p[i + k] & 0xC0) != 0x80) return false;
            i += len;
        }
        return true;
    }

    std::string WideToUtf8(const wchar_t* w, int n)
    {
        if (n <= 0) return std::string();
        int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, n, out.data(), len, nullptr, nullptr);
        return out;
    }

    std::string BytesToUtf8Text(const std::vector<unsigned char>& b)
    {
        if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
            return std::string(reinterpret_cast<const char*>(b.data()) + 3, b.size() - 3);
        if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE)
            return WideToUtf8(reinterpret_cast<const wchar_t*>(b.data() + 2), static_cast<int>((b.size() - 2) / 2));
        if (IsValidUtf8(b.data(), b.size()))
            return std::string(reinterpret_cast<const char*>(b.data()), b.size());
        // ANSI (CP_ACP, e.g. CP949) -> wide -> UTF-8
        int wlen = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), w.data(), wlen);
        return WideToUtf8(w.data(), wlen);
    }
}

bool IniFile::Load(const std::wstring& path, std::string& err)
{
    sections_.clear();
    std::vector<unsigned char> bytes;
    if (!Mim::ReadFileBytes(path, bytes, err)) return false;
    const std::string text = BytesToUtf8Text(bytes);

    Section* cur = nullptr;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find('\n', pos);
        std::string line = Trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[')
        {
            size_t close = line.find(']');
            if (close == std::string::npos) continue;
            sections_.push_back(Section{ Trim(line.substr(1, close - 1)), {} });
            cur = &sections_.back();
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || !cur) continue;
        cur->kv.emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
    }
    return true;
}

bool IniFile::HasSection(const std::string& section) const
{
    for (const auto& s : sections_) if (IEquals(s.name, section)) return true;
    return false;
}

bool IniFile::Get(const std::string& section, const std::string& key, std::string& value) const
{
    for (const auto& s : sections_)
    {
        if (!IEquals(s.name, section)) continue;
        for (const auto& kv : s.kv)
            if (IEquals(kv.first, key)) { value = kv.second; return true; }
    }
    return false;
}

std::string IniFile::GetOr(const std::string& section, const std::string& key, const std::string& def) const
{
    std::string v;
    return Get(section, key, v) ? v : def;
}

std::vector<std::string> IniFile::SectionNames() const
{
    std::vector<std::string> out;
    for (const auto& s : sections_) out.push_back(s.name);
    return out;
}

bool ParseIntPair(const std::string& s, int& a, int& b)
{
    return sscanf_s(s.c_str(), "%d,%d", &a, &b) == 2;
}

bool ParseIntQuad(const std::string& s, int& a, int& b, int& c, int& d)
{
    return sscanf_s(s.c_str(), "%d,%d,%d,%d", &a, &b, &c, &d) == 4;
}

double AtofLike(const std::string& s)
{
    return strtod(s.c_str(), nullptr);
}

int StageFromString(const std::string& s)
{
    if (s == "TOP") return 1;
    if (s == "BOTTOM") return 2;
    if (s == "NONE") return 3;
    return 0;
}

const char* StageToString(int stage)
{
    switch (stage) { case 1: return "TOP"; case 2: return "BOTTOM"; case 3: return "NONE"; default: return "AUTO"; }
}

CalParams ReadCalSection(const IniFile& ini, int calIdx)
{
    char sec[32];
    snprintf(sec, sizeof(sec), "CAL%04d", calIdx);
    if (!ini.HasSection(sec))
        throw std::runtime_error(std::string("section [") + sec + "] not found");

    CalParams p;
    p.name = ini.GetOr(sec, "Name", "");
    if (p.name.empty())
        throw std::runtime_error("'Name' key is missing in configuration");

    p.type = ini.GetOr(sec, "DepthPreprocType", "");
    if (p.type.empty())
        throw std::runtime_error("'DepthPreprocType' key is missing in configuration");
    if (p.type != "INNERCENTER" && p.type != "INSHOULDER" && p.type != "BEAD")
        throw std::runtime_error("Invalid 'DepthPreprocType' value");

    p.stage = StageFromString(ini.GetOr(sec, "StagePosition", "AUTO"));

    std::string strPatch = ini.GetOr(sec, "PatchSize", "");
    if (!strPatch.empty())
    {
        int w = 0, h = 0;
        if (ParseIntPair(strPatch, w, h)) { p.patch_w = w; p.patch_h = h; }
        else throw std::runtime_error("[Error] C3DPreprocess::ReadSection: Unable to parse PatchSize from '" + strPatch + "'; expected format: width,height");
    }
    else
        throw std::runtime_error("'PatchSize' key is missing in configuration; expected format: width,height");

    std::string strLow = ini.GetOr(sec, "Lower Percentage", "");
    if (!strLow.empty()) p.lower = AtofLike(strLow);
    else throw std::runtime_error("'Lower Percentage' key is missing in configuration");

    std::string strUpp = ini.GetOr(sec, "Upper Percentage", "");
    if (!strUpp.empty()) p.upper = AtofLike(strUpp);
    else throw std::runtime_error("'Upper Percentage' key is missing in configuration");

    std::string strOverlap = ini.GetOr(sec, "Overlap", "");
    if (!strOverlap.empty())
    {
        float val = static_cast<float>(AtofLike(strOverlap));
        if (val <= 0.0f || val >= 1.0f)
            throw std::runtime_error("[Error] Invalid Overlap value: " + strOverlap + ". Expected range: (0, 1).");
        p.overlap = val;
    }
    else
        p.overlap = 0.5f;

    std::string strRoi = ini.GetOr(sec, "ROI", "");
    if (!strRoi.empty())
    {
        int x1, y1, x2, y2;
        if (ParseIntQuad(strRoi, x1, y1, x2, y2)) { p.roi = { x1, y1, x2, y2 }; p.has_roi = true; }
        else throw std::runtime_error("[Error] C3DPreprocess::ReadSection: Unable to parse ROI from '" + strRoi + "'; expected format: x1,y1,x2,y2");
    }
    return p;
}

std::vector<FovProcEntry> ReadFovProc(const IniFile& ini)
{
    std::vector<FovProcEntry> out;
    for (const auto& name : ini.SectionNames())
    {
        if (name.size() < 8 || !IEquals(name.substr(0, 7), "FOVPROC")) continue;
        std::string num = name.substr(7);
        if (num.empty() || !std::all_of(num.begin(), num.end(), [](char c) { return isdigit(static_cast<unsigned char>(c)) != 0; })) continue;
        FovProcEntry e;
        e.idx = atoi(num.c_str());
        e.name = ini.GetOr(name, "Name", "");
        e.dll = ini.GetOr(name, "DLL", "");
        e.prefix = ini.GetOr(name, "Prefix", "");
        e.paramFile = ini.GetOr(name, "ParamFile", "");
        std::string v;
        if (ini.Get(name, "RequireImgIdx", v)) e.requireImgIdx = atoi(v.c_str());
        if (ini.Get(name, "ResultImgIdx", v)) e.resultImgIdx = atoi(v.c_str());
        if (ini.Get(name, "ParamIdx", v)) e.paramIdx = atoi(v.c_str());
        out.push_back(e);
    }
    return out;
}
