#pragma once
// alg_depth_preproc.ini ([CALxxxx]) and FOVPROC.ini ([FOVPROCxxxx]) readers mirroring the DLL.

#include <array>
#include <string>
#include <utility>
#include <vector>

class IniFile
{
public:
    // Accepts UTF-8 (with/without BOM), UTF-16LE BOM, or ANSI(CP_ACP); CRLF/LF; ';' and '#' comment lines.
    bool Load(const std::wstring& path, std::string& err);
    bool HasSection(const std::string& section) const;
    // Section/key match is case-insensitive; the value is trimmed. Returns false when missing.
    bool Get(const std::string& section, const std::string& key, std::string& value) const;
    std::string GetOr(const std::string& section, const std::string& key, const std::string& def) const;
    std::vector<std::string> SectionNames() const;

private:
    struct Section { std::string name; std::vector<std::pair<std::string, std::string>> kv; };
    std::vector<Section> sections_;
};

struct CalParams
{
    std::string name;
    std::string type;          // INNERCENTER | BEAD | INSHOULDER
    int    stage = 0;          // AUTO
    int    patch_w = 15, patch_h = 15;
    double lower = 0, upper = 0;
    double overlap = 0.5;      // DLL default when the key is missing
    std::array<int, 4> roi{ 9999, 9999, 0, 0 };
    bool   has_roi = false;
};

// Mirrors C3DPreprocess::ReadSection for [CAL%04d]; throws std::runtime_error with the DLL's messages.
CalParams ReadCalSection(const IniFile& ini, int calIdx);

struct FovProcEntry
{
    int idx = 0;
    std::string name, dll, prefix, paramFile;
    int requireImgIdx = -1, resultImgIdx = -1, paramIdx = -1;
};

std::vector<FovProcEntry> ReadFovProc(const IniFile& ini);

// "%d,%d" / "%d,%d,%d,%d" with sscanf semantics (leading blanks ok, no blank before ',').
bool ParseIntPair(const std::string& s, int& a, int& b);
bool ParseIntQuad(const std::string& s, int& a, int& b, int& c, int& d);
// _wtof semantics (strtod prefix, 0 when not numeric).
double AtofLike(const std::string& s);
int  StageFromString(const std::string& s);        // TOP=1, BOTTOM=2, NONE=3, else AUTO=0
const char* StageToString(int stage);
