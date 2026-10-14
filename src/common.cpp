// common.cpp
#include "common.h"
#include <shlwapi.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

// ---------------- 字符串 / 路径 ----------------

std::string W2U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring U82W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

long long FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

// ---------------- 格式化 ----------------

std::wstring FormatDuration(long long ms) {
    if (ms < 0) ms = 0;
    long long sec = ms / 1000;
    wchar_t buf[32];
    if (sec >= 3600)
        swprintf(buf, 32, L"%lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
    else
        swprintf(buf, 32, L"%02lld:%02lld", sec / 60, sec % 60);
    return buf;
}

std::wstring FormatBytes(long long bytes) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; ++i; }
    wchar_t buf[64];
    if (i == 0) swprintf(buf, 64, L"%lld %s", bytes, units[i]);
    else        swprintf(buf, 64, L"%.1f %s", v, units[i]);
    return buf;
}

std::wstring FormatDateTime(long long epochSec) {
    time_t t = (time_t)epochSec;
    tm tmv = {};
    localtime_s(&tmv, &t);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return buf;
}

std::wstring DefaultOutputDir() {
    if (GetDriveTypeW(L"D:\\") != DRIVE_NO_ROOT_DIR) return L"D:\\Recordings";
    wchar_t* profile = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&profile, &len, L"USERPROFILE") == 0 && profile) {
        std::wstring p = JoinPath(profile, L"Videos");
        free(profile);
        return p;
    }
    return L".\\Recordings";
}

// ---------------- 配置文件读写 ----------------

static bool ReadAllText(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    if (!out.empty()) ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    out.resize(read);
    CloseHandle(h);
    // 去掉 UTF-8 BOM
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

static bool WriteAllText(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(h);
    return true;
}

static std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) j = s.size();
        std::string line = s.substr(i, j - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        if (j == s.size()) break;
        i = j + 1;
    }
    return out;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void Config::Load(const std::wstring& p) {
    path = p;
    kv.clear();
    std::string text;
    if (!ReadAllText(p, text)) return;
    for (const std::string& raw : SplitLines(text)) {
        std::string line = Trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[Trim(line.substr(0, eq))] = U82W(Trim(line.substr(eq + 1)));
    }
}

void Config::Save() const {
    std::string out = "# LiteCap settings\r\n";
    for (const auto& it : kv) out += it.first + "=" + W2U8(it.second) + "\r\n";
    WriteAllText(path, out);
}

std::wstring Config::Get(const std::string& key, const std::wstring& def) const {
    auto it = kv.find(key);
    return (it == kv.end() || it->second.empty()) ? def : it->second;
}

int Config::GetInt(const std::string& key, int def) const {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

void Config::Set(const std::string& key, const std::wstring& value) {
    kv[key] = value;
}

// ---------------- 本地化 ----------------

void I18n::LoadDir(const std::wstring& dir) {
    tables_.clear();
    langs_.clear();
    std::wstring pattern = JoinPath(dir, L"*.ini");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring full = JoinPath(dir, fd.cFileName);
        std::string text;
        if (!ReadAllText(full, text)) continue;

        std::wstring name = fd.cFileName;
        size_t dot = name.find_last_of(L'.');
        std::string code = W2U8((dot == std::wstring::npos) ? name : name.substr(0, dot));

        std::map<std::string, std::wstring> table;
        for (const std::string& raw : SplitLines(text)) {
            std::string line = Trim(raw);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            table[Trim(line.substr(0, eq))] = U82W(Trim(line.substr(eq + 1)));
        }
        std::wstring disp = code == "zh-CN" ? L"简体中文"
                          : code == "en-US" ? L"English"
                          : table.count("_name") ? table["_name"] : U82W(code);
        langs_.push_back({ code, disp });
        tables_[code] = std::move(table);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(langs_.begin(), langs_.end(),
              [](const Lang& a, const Lang& b) { return a.code < b.code; });
    if (!tables_.count(cur_)) cur_ = tables_.empty() ? "en-US" : tables_.begin()->first;
}

void I18n::SetLang(const std::string& code) {
    if (tables_.count(code)) cur_ = code;
    else if (tables_.count("en-US")) cur_ = "en-US";
}

std::wstring I18n::T(const std::string& key) const {
    auto t = tables_.find(cur_);
    if (t != tables_.end()) {
        auto it = t->second.find(key);
        if (it != t->second.end()) return it->second;
    }
    auto f = tables_.find("en-US");
    if (f != tables_.end()) {
        auto it = f->second.find(key);
        if (it != f->second.end()) return it->second;
    }
    return U82W(key);
}
