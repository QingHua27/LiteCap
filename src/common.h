// common.h - 基础工具、配置、本地化
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <map>

// ---------------- 字符串 / 路径 ----------------
std::string  W2U8(const std::wstring& w);
std::wstring U82W(const std::string& s);
std::wstring ExeDir();
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
bool         FileExists(const std::wstring& p);
long long    FileSize(const std::wstring& p);

// ---------------- 格式化 ----------------
std::wstring FormatDuration(long long ms);          // 00:00 / 01:23:45
std::wstring FormatBytes(long long bytes);          // 1.4 MB
std::wstring FormatDateTime(long long epochSec);    // 2026-10-15 03:56

// ---------------- 配置 (settings.ini) ----------------
class Config {
public:
    void Load(const std::wstring& path);
    void Save() const;

    std::wstring Get(const std::string& key, const std::wstring& def = L"") const;
    int          GetInt(const std::string& key, int def) const;
    void         Set(const std::string& key, const std::wstring& value);

    std::wstring path;
    std::map<std::string, std::wstring> kv;
};

// ---------------- 本地化 (lang/*.ini) ----------------
class I18n {
public:
    void LoadDir(const std::wstring& dir);
    void SetLang(const std::string& code);
    std::wstring T(const std::string& key) const;

    struct Lang { std::string code; std::wstring name; };
    const std::vector<Lang>& Languages() const { return langs_; }
    const std::string& Current() const { return cur_; }

private:
    std::map<std::string, std::map<std::string, std::wstring>> tables_;
    std::vector<Lang> langs_;
    std::string cur_ = "zh-CN";
};

// ---------------- 输出目录默认值 ----------------
std::wstring DefaultOutputDir();

// ---------------- 图标 ----------------
// 资源 ID，与 res/app.rc 里一致
#define IDI_APPICON 1

// 按实际需要的尺寸加载图标。不指定尺寸的话系统会把 32x32 硬缩到 16x16，边缘会糊。
HICON LoadAppIcon(HINSTANCE inst, int cx, int cy);
