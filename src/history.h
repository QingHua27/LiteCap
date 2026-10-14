// history.h - 录制记录
#pragma once
#include "common.h"

struct RecItem {
    long long    epoch = 0;         // 录制结束时间 (Unix 秒)
    long long    durationMs = 0;
    long long    bytes = 0;
    std::wstring path;              // 完整路径
    std::wstring name;              // 文件名
};

class History {
public:
    void Load(const std::wstring& path);
    void Save() const;

    void Add(const RecItem& item);          // 插到最前
    void RemoveAt(size_t index);
    void Clear();

    const std::vector<RecItem>& Items() const { return items_; }
    size_t Count() const { return items_.size(); }

private:
    std::vector<RecItem> items_;
    std::wstring path_;
};
