#include "fcs_localization.hpp"
#include <wchar.h>

namespace fcs::host {
namespace {

struct Translation {
    const wchar_t* english;
    const wchar_t* chinese;
};

constexpr Translation kTranslations[] = {
    {kControlTitle, L"FFXIV Clean Stream — 控制面板"},
    {kIntroText,
     L"1. 启动 FFXIV，并确保 MMOMinion 界面已显示。\r\n"
     L"2. 选择设置，然后点击“开始 / 继续”。\r\n"
     L"3. 在 Discord 中共享“FFXIV Clean Stream”窗口，请勿共享 FFXIV。"},
    {kInitialStatus, L"尚未连接游戏。点击“开始 / 继续”后，本应用才会连接 FFXIV。"},
    {L"Start / Resume", L"开始 / 继续"},
    {L"Pause capture", L"暂停捕获"},
    {L"End stream", L"结束直播"},
    {L"Copy error", L"复制错误"},
    {L"Copied!", L"已复制！"},
    {L"Copy failed", L"复制失败"},
    {L"Frame rate:", L"帧率："},
    {L"Resolution:", L"分辨率："},
    {L"30 FPS (recommended)", L"30 FPS（推荐）"},
    {L"60 FPS", L"60 FPS"},
    {L"15 FPS (lightest)", L"15 FPS（最低负载）"},
    {L"FFXIV Clean Stream could not reserve its controller session.",
     L"FFXIV Clean Stream 无法创建控制器会话。"},
    {L"FFXIV Clean Stream is already running.", L"FFXIV Clean Stream 已在运行。"},
    {L"The clean stream window could not be created.", L"无法创建纯净画面预览窗口。"},
    {L"MMOMinion changed its graphics hook after capture began. "
     L"Restart FFXIV, wait until the MMOMinion GUI is visible, "
     L"then click Start / Resume.",
     L"捕获开始后，MMOMinion 更改了图形钩子。请重启 FFXIV，等待 MMOMinion 界面显示后，再点击“开始 / 继续”。"},
    {L"Resuming clean capture...", L"正在继续捕获纯净画面…"},
    {L"FFXIV (DirectX 11) was not found. Start the game and MMOMinion first.",
     L"未找到 FFXIV（DirectX 11）。请先启动游戏和 MMOMinion。"},
    {L"The private capture session could not be created.", L"无法创建专用捕获会话。"},
    {L"Attaching after MMOMinion... waiting for the clean game frame.",
     L"正在接入 MMOMinion 之后的捕获链路…等待纯净游戏画面。"},
    {L"The stream is ended. Click Start / Resume to open it again.",
     L"直播已结束。点击“开始 / 继续”可重新打开预览窗口。"},
    {L"GPU frame copies are paused. Click Start / Resume to resume.",
     L"已暂停 GPU 帧复制。点击“开始 / 继续”可恢复捕获。"},
    {L"Stream ended. The clean stream window is closed. Click Start / Resume to open it again.",
     L"直播已结束，纯净画面预览窗口已关闭。点击“开始 / 继续”可重新打开。"},
    {L"FFXIV exited. Start the new game session, then click Start / Resume.",
     L"FFXIV 已退出。请重新启动游戏，然后点击“开始 / 继续”。"},
    {L"The preview could not open this GPU format after three attempts. "
     L"Choose 30 FPS and click Start / Resume to try again.",
     L"预览已尝试三次，仍无法打开此 GPU 格式。请选择 30 FPS，然后点击“开始 / 继续”重试。"},
    {L"Ready. Share the window named ‘FFXIV Clean Stream’ in Discord.\r\n\r\n"
     L"Clean frames: %lld    Busy frames dropped: %lld\r\n"
     L"Worst measured copy-submit CPU time: %.1f microseconds\r\n\r\n%s",
     L"已就绪。请在 Discord 中共享“FFXIV Clean Stream”窗口。\r\n\r\n"
     L"纯净画面帧数：%lld    因忙碌丢弃的帧数：%lld\r\n"
     L"帧复制提交的最长 CPU 耗时：%.1f 微秒\r\n\r\n%s"},
    {L"Preparing a compatibility GPU frame ring outside FFXIV...\r\n\r\n%s",
     L"正在 FFXIV 进程外准备兼容 GPU 帧缓冲环…\r\n\r\n%s"},
    {L"Capture stopped safely because MMOMinion changed its graphics hook.\r\n\r\n"
     L"Restart FFXIV, wait until the MMOMinion GUI is visible, then click "
     L"Start / Resume.\r\n\r\n%s",
     L"MMOMinion 更改了图形钩子，捕获已安全停止。\r\n\r\n"
     L"请重启 FFXIV，等待 MMOMinion 界面显示后，再点击“开始 / 继续”。\r\n\r\n%s"},
    {L"Capture error (%ld):\r\n%s", L"捕获错误（%ld）：\r\n%s"},
    {L"Waiting...", L"正在等待…"},
    {L"Waiting for a clean FFXIV frame...", L"正在等待纯净 FFXIV 画面…"},
    {L"FfxivCleanStreamHook64.dll is missing beside the app.",
     L"应用所在文件夹中缺少 FfxivCleanStreamHook64.dll。"},
    {L"Windows denied access to FFXIV. Run this app at the same privilege level as FFXIV.",
     L"Windows 拒绝访问 FFXIV。请以与 FFXIV 相同的权限级别运行本应用。"},
    {L"The capture helper path could not be sent to FFXIV.", L"无法将捕获辅助模块路径发送到 FFXIV。"},
    {L"Windows could not start the standalone capture helper.", L"Windows 无法启动独立捕获辅助模块。"},
    {L"FFXIV did not load the standalone capture helper.", L"FFXIV 未能加载独立捕获辅助模块。"},
    {L"The preview GPU could not create the compatibility frame ring.", L"预览 GPU 无法创建兼容帧缓冲环。"},
    {L"GPU sharing is ready; waiting for FFXIV to open it...", L"GPU 共享已就绪，正在等待 FFXIV 打开共享资源…"},
    {L"Compatibility GPU sharing is ready; waiting for FFXIV to open it...", L"兼容 GPU 共享已就绪，正在等待 FFXIV 打开共享资源…"},
    {L"The preview does not support this game format.", L"预览不支持此游戏画面格式。"},
    {L"The preview could not find the same GPU used by FFXIV.", L"预览无法找到 FFXIV 使用的 GPU。"},
    {L"The clean preview swap chain could not be created for this game format.", L"无法为此游戏画面格式创建纯净预览交换链。"},
    {L"Direct3D 11.1 GPU sharing is unavailable on this system.", L"此系统不支持 Direct3D 11.1 GPU 共享。"},
    {L"The clean preview backbuffer could not be opened.", L"无法打开纯净预览后台缓冲区。"},
    {L"A shared clean-frame surface could not be opened. Click Start / Resume.", L"无法打开共享的纯净画面纹理。请点击“开始 / 继续”。"},
    {L"Waiting for FFXIV to request a new controller-owned frame ring...", L"正在等待 FFXIV 请求由控制器创建的新帧缓冲环…"},
    {L"The preview GPU completion check failed; reconnecting.", L"预览 GPU 完成状态检查失败，正在重新连接。"},
    {L"The compatibility frame ring changed; reconnecting.", L"兼容帧缓冲环已更改，正在重新连接。"},
    {L"The GPU frame ring lost synchronization; reconnecting.", L"GPU 帧缓冲环失去同步，正在重新连接。"},
    {L"The GPU frame ring was abandoned; reconnecting.", L"GPU 帧缓冲环已失效，正在重新连接。"},
    {L"The preview GPU device was reset; reconnecting.", L"预览 GPU 设备已重置，正在重新连接。"},
    {L"The clean preview could not present a frame; reconnecting.", L"纯净预览无法显示画面，正在重新连接。"},
    // The helper's English IPC messages stay stable for existing diagnostic tools.
    {L"Injected. Waiting for FFXIV's main Direct3D swap chain...", L"辅助模块已加载。正在等待 FFXIV 的主 Direct3D 交换链…"},
    {L"Injected with base Present compatibility. Waiting for FFXIV...", L"辅助模块已使用基础 Present 兼容模式加载。正在等待 FFXIV…"},
    {L"The Direct3D hook engine could not initialize; retrying.", L"Direct3D 钩子引擎初始化失败，正在重试。"},
    {L"Could not locate Direct3D 11 Present; retrying.", L"无法找到 Direct3D 11 Present，正在重试。"},
    {L"Present is hooked incompatibly; retrying.", L"Present 钩子不兼容，正在重试。"},
    {L"Could not enable the Direct3D Present hook; retrying.", L"无法启用 Direct3D Present 钩子，正在重试。"},
    {L"MMOMinion changed the graphics hook after capture started. "
     L"Capture stopped safely to avoid showing the overlay or hurting FPS. "
     L"Restart FFXIV, wait for the MMOMinion GUI, then start this app again.",
     L"捕获开始后，MMOMinion 更改了图形钩子。为避免显示叠加界面或影响帧率，捕获已安全停止。"
     L"请重启 FFXIV，等待 MMOMinion 界面显示后，再启动本应用。"},
    {L"Present is wrapped before the MMOMinion overlay.", L"已在 MMOMinion 叠加界面绘制之前接入 Present。"},
    {L"Clean frames are reaching the preview using the nonblocking compatibility ring.", L"纯净画面正通过非阻塞兼容帧缓冲环传送到预览。"},
    {L"The game GPU device cannot create shared textures; preparing them in the preview instead...", L"游戏 GPU 设备无法创建共享纹理，正在预览端创建…"},
    {L"The named GPU-sharing path was rejected; preparing the compatibility ring instead...", L"命名 GPU 共享方式被拒绝，正在准备兼容帧缓冲环…"},
    {L"Clean frames are reaching the preview using controller-owned GPU sharing.", L"纯净画面正通过控制器创建的 GPU 共享资源传送到预览。"},
    {L"Clean frames are reaching the preview using controller-owned compatibility sharing.", L"纯净画面正通过控制器创建的兼容共享资源传送到预览。"},
    {L"Could not get FFXIV's Direct3D 11 device.", L"无法获取 FFXIV 的 Direct3D 11 设备。"},
    {L"Could not get FFXIV's Direct3D context.", L"无法获取 FFXIV 的 Direct3D 上下文。"},
    {L"Could not identify the GPU used by FFXIV.", L"无法识别 FFXIV 使用的 GPU。"},
    {L"Clean frames are reaching the standalone preview.", L"纯净画面正在传送到独立预览窗口。"},
    {L"Clean frames are reaching the standalone preview using compatibility GPU sharing.", L"纯净画面正通过兼容 GPU 共享传送到独立预览窗口。"},
    {L"The compatibility frame ring changed while a GPU copy was pending; reconnecting.", L"等待 GPU 复制完成时，兼容帧缓冲环发生更改，正在重新连接。"},
    {L"The compatibility frame completion check failed; reconnecting.", L"兼容帧完成状态检查失败，正在重新连接。"},
    {L"The GPU sharing ring was abandoned; recreating it.", L"GPU 共享缓冲环已失效，正在重新创建。"},
    {L"The GPU sharing ring lost synchronization; recreating it.", L"GPU 共享缓冲环失去同步，正在重新创建。"},
    {L"FFXIV's graphics device was reset; waiting to reconnect.", L"FFXIV 的图形设备已重置，正在等待重新连接。"},
    {L"format selection", L"格式选择"},
    {L"sharing-mode selection", L"共享模式选择"},
};

bool SettingsPath(wchar_t (&path)[MAX_PATH], bool createDirectory) {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (!length || length >= MAX_PATH - _countof(L"\\FfxivCleanStream\\settings.ini") ||
        wcscat_s(path, L"\\FfxivCleanStream") != 0) return false;
    if (createDirectory && !CreateDirectoryW(path, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) return false;
    return wcscat_s(path, L"\\settings.ini") == 0;
}

} // namespace

UiLanguage LanguageForWindowsUi(LANGID language) {
    return PRIMARYLANGID(language) == LANG_CHINESE &&
            (SUBLANGID(language) == SUBLANG_CHINESE_SIMPLIFIED ||
             SUBLANGID(language) == SUBLANG_CHINESE_SINGAPORE ||
             SUBLANGID(language) == SUBLANG_NEUTRAL)
        ? UiLanguage::SimplifiedChinese : UiLanguage::English;
}

UiLanguage LoadUiLanguage() {
    wchar_t path[MAX_PATH]{};
    wchar_t value[32]{};
    if (SettingsPath(path, false)) {
        GetPrivateProfileStringW(L"UI", L"Language", L"", value, 32, path);
        if (wcscmp(value, L"zh-CN") == 0) return UiLanguage::SimplifiedChinese;
        if (wcscmp(value, L"en") == 0) return UiLanguage::English;
    }
    return LanguageForWindowsUi(GetUserDefaultUILanguage());
}

bool SaveUiLanguage(UiLanguage language) {
    if (language != UiLanguage::English &&
        language != UiLanguage::SimplifiedChinese) return false;
    wchar_t path[MAX_PATH]{};
    return SettingsPath(path, true) && WritePrivateProfileStringW(
        L"UI", L"Language", language == UiLanguage::SimplifiedChinese
            ? L"zh-CN" : L"en", path);
}

const wchar_t* UiText(UiLanguage language, const wchar_t* english) {
    if (!english) return L"";
    if (language == UiLanguage::SimplifiedChinese) {
        for (const auto& entry : kTranslations) {
            if (wcscmp(english, entry.english) == 0) return entry.chinese;
        }
    }
    return english;
}

void LocalizeDiagnostic(UiLanguage language, const wchar_t* english,
                        wchar_t* output, size_t outputChars) {
    if (!output || !outputChars) return;
    if (!english) english = L"";
    const wchar_t* translated = UiText(language, english);
    if (language == UiLanguage::SimplifiedChinese && translated == english) {
        // These are the only formatted helper diagnostics. Match the entire
        // message before translating; retain unknown/malformed text verbatim.
        unsigned long nt = 0, legacy = 0, name = 0, handle = 0, host = 0, plain = 0;
        int end = 0;
        if (swscanf(english,
            L"All GPU-sharing paths failed. Game NT 0x%lX; game legacy 0x%lX; "
            L"host NT name 0x%lX; host NT handle 0x%lX; host legacy 0x%lX; "
            L"plain legacy 0x%lX.%n", &nt, &legacy, &name, &handle, &host,
            &plain, &end) == 6 && end > 0 && !english[end]) {
            _snwprintf_s(output, outputChars, _TRUNCATE,
                L"所有 GPU 共享方式均失败。游戏 NT 0x%08lX；游戏传统共享 0x%08lX；"
                L"控制器 NT 名称 0x%08lX；控制器 NT 句柄 0x%08lX；"
                L"控制器传统共享 0x%08lX；普通传统共享 0x%08lX。",
                nt, legacy, name, handle, host, plain);
            return;
        }
        wchar_t stage[64]{};
        unsigned long error = 0;
        unsigned int source = 0, transport = 0, width = 0, height = 0, samples = 0, flags = 0;
        end = 0;
        if (swscanf(english,
            L"Shared capture setup failed at %63l[^()] (0x%lX). "
            L"Source format %u -> transport %u, %ux%u, samples %u, bind flags 0x%X.%n",
            stage, &error, &source, &transport, &width, &height, &samples,
            &flags, &end) == 8 && end > 0 && !english[end]) {
            size_t length = wcslen(stage);
            while (length && stage[length - 1] == L' ') stage[--length] = L'\0';
            _snwprintf_s(output, outputChars, _TRUNCATE,
                L"共享捕获在%s阶段设置失败（0x%08lX）。源格式 %u → 传输格式 %u，"
                L"%u×%u，采样数 %u，绑定标志 0x%X。",
                UiText(language, stage), error, source, transport, width, height, samples, flags);
            return;
        }
    }
    wcsncpy_s(output, outputChars, translated, _TRUNCATE);
}

} // namespace fcs::host
