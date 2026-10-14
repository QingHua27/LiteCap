// player.h - 软件内播放（Media Foundation MFPlay）
#pragma once
#include "common.h"

class Player {
public:
    // 在软件内打开一个视频文件；同一时刻只允许一个播放窗口
    static bool Open(HINSTANCE inst, const std::wstring& path, I18n* i18n);
    static void Close();
    static bool IsOpen();
};
