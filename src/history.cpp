// history.cpp
#include "history.h"
#include <cstdio>
#include <algorithm>
#include <unordered_set>

// 存储格式：每行  epoch \t 时长ms \t 字节数 \t 完整路径
// 用 TSV 而不是 JSON：解析零依赖，人也能直接看和改。

void History::Load(const std::wstring& path) {
    path_ = path;
    items_.clear();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    std::string text((size_t)sz.QuadPart, '\0');
    DWORD read = 0;
    if (!text.empty()) ReadFile(h, &text[0], (DWORD)text.size(), &read, nullptr);
    text.resize(read);
    CloseHandle(h);

    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text.erase(0, 3);

    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        std::string line = text.substr(i, j - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        i = j + 1;
        if (line.empty() || line[0] == '#') continue;

        // 拆成 4 段
        std::string f[4];
        int n = 0;
        size_t p = 0;
        while (n < 3) {
            size_t t = line.find('\t', p);
            if (t == std::string::npos) break;
            f[n++] = line.substr(p, t - p);
            p = t + 1;
        }
        f[3] = line.substr(p);
        if (n < 3 || f[3].empty()) continue;

        RecItem it;
        try {
            it.epoch      = std::stoll(f[0]);
            it.durationMs = std::stoll(f[1]);
            it.bytes      = std::stoll(f[2]);
        } catch (...) { continue; }
        it.path = U82W(f[3]);
        size_t slash = it.path.find_last_of(L"\\/");
        it.name = (slash == std::wstring::npos) ? it.path : it.path.substr(slash + 1);
        items_.push_back(std::move(it));
    }
    PruneMissing();
}

void History::Save() const {
    std::string out = "# LiteCap recording history\r\n"
                      "# epoch\tduration_ms\tbytes\tpath\r\n";
    for (const RecItem& it : items_) {
        char head[128];
        sprintf_s(head, "%lld\t%lld\t%lld\t", it.epoch, it.durationMs, it.bytes);
        out += head;
        out += W2U8(it.path);
        out += "\r\n";
    }
    HANDLE h = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(h);
}

void History::Add(const RecItem& item) {
    items_.insert(items_.begin(), item);
    Save();
}

void History::RemoveAt(size_t index) {
    if (index >= items_.size()) return;
    items_.erase(items_.begin() + index);
    Save();
}

void History::RemovePaths(const std::vector<std::wstring>& paths) {
    std::unordered_set<std::wstring> removed(paths.begin(), paths.end());
    const auto oldSize = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const RecItem& item) {
        return removed.contains(item.path);
    }), items_.end());
    if (items_.size() != oldSize) Save();
}

std::vector<size_t> History::PruneMissing() {
    std::vector<size_t> removed;
    for (size_t i = 0; i < items_.size(); ++i) {
        DWORD attributes = GetFileAttributesW(items_[i].path.c_str());
        DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if ((attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) ||
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) removed.push_back(i);
    }
    for (auto it = removed.rbegin(); it != removed.rend(); ++it) items_.erase(items_.begin() + *it);
    if (!removed.empty()) Save();
    return removed;
}

void History::Clear() {
    items_.clear();
    Save();
}
