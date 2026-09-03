#pragma once

// 全局日志封装（spdlog，header-only，Qt / 非 Qt 环境自适应）：
//   1. Windows 下程序启动时 AllocConsole 分配一个控制台窗口，实时显示操作 / 计数 / 帧率
//   2. 同时写滚动文件 logs/yyyyMMdd_FramePlayer.log（5MB x 3 份，info 级以上立即落盘）
//   3. Qt 环境下自动把 qDebug/qWarning/qCritical 转发到 spdlog（非 Qt 环境跳过）
// 使用方式：main() 中调用 AppLog::init() 之后，任意处：
//   LOG_INFO("加载文件: {}（{} 字节）", path, size);
// 依赖：
//   - spdlog header-only（include 路径加 spdlog\include）
//   - Windows 工程建议定义 SPDLOG_WCHAR_FILENAMES（中文日志路径安全），非 Windows 自动降级为窄字符
//   - 无需 C++17，C++14 即可（MSVC v143 默认设置直接可用）

#include <string>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <sys/stat.h>     // POSIX mkdir
  #include <sys/types.h>
#endif

// Qt 环境（Qt MSBuild 工程会全局定义 QT_CORE_LIB）自动启用 Qt 日志转发
#if defined(QT_CORE_LIB)
  #include <QtGlobal>       // qInstallMessageHandler / QtMsgType / QMessageLogContext
  #include <QString>
  #define APPLOG_HAVE_QT 1
#endif

// 便捷宏：自动判空，未初始化时静默跳过
// （需定义在使用 LOG_* 的函数体之前，故放在 namespace 之前）
#define LOG_TRACE(...) do { if (auto _lg = AppLog::detail::logger()) _lg->trace(__VA_ARGS__); } while (0)
#define LOG_DEBUG(...) do { if (auto _lg = AppLog::detail::logger()) _lg->debug(__VA_ARGS__); } while (0)
#define LOG_INFO(...)  do { if (auto _lg = AppLog::detail::logger()) _lg->info(__VA_ARGS__); }  while (0)
#define LOG_WARN(...)  do { if (auto _lg = AppLog::detail::logger()) _lg->warn(__VA_ARGS__); }  while (0)
#define LOG_ERROR(...) do { if (auto _lg = AppLog::detail::logger()) _lg->error(__VA_ARGS__); } while (0)

namespace AppLog {

namespace detail {

// 全局 logger 持有处（未初始化前为空指针，各宏已做保护）
inline std::shared_ptr<spdlog::logger> &logger()
{
    static std::shared_ptr<spdlog::logger> lg;
    return lg;
}

// ---------------- 平台相关辅助 ----------------

#ifdef _WIN32
// exe 所在目录（宽字符，支持中文路径）
inline std::wstring exeDirW()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L".";
    std::wstring p(buf, n);
    const size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

// GUI 程序没有控制台，AllocConsole 分配一个；
// 关闭快速编辑模式，避免鼠标选中文本时 WriteConsole 阻塞拖住输出线程
inline void setupConsole()
{
    if (!AllocConsole())
        return;

    // GUI 程序原本没有标准流，重定向到新控制台（freopen_s 为 MSVC 安全版本）
    FILE *out = nullptr, *err = nullptr;
    freopen_s(&out, "CONOUT$", "w", stdout);
    freopen_s(&err, "CONOUT$", "w", stderr);

    SetConsoleOutputCP(CP_UTF8);            // 中文按 UTF-8 输出，配合 /utf-8 编译选项
    SetConsoleTitleW(L"FramePlayer 日志");

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hIn, &mode)) {
        mode &= ~ENABLE_QUICK_EDIT_MODE;
        mode |= ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hIn, mode);
    }
}
#endif // _WIN32

// 启动日期 yyyyMMdd
inline std::string todayStamp()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[16] = {};
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
    return std::string(buf);
}

// 日志完整路径：exe 同级 logs/yyyyMMdd_FramePlayer.log
// （单文件超 5MB 滚动时 spdlog 自动追加 _1/_2 后缀）
// Windows 用宽字符路径（中文安全）；其他平台相对 CWD 的 logs/，不存在则创建
inline spdlog::filename_t logFilePath()
{
    const std::string name = todayStamp() + "_FramePlayer.log";
#if defined(_WIN32) && defined(SPDLOG_WCHAR_FILENAMES)
    const std::wstring dir = exeDirW() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);   // 已存在时静默失败
    // 文件名只含 ASCII 数字字母，窄转宽可直接逐字符
    return dir + L"\\" + std::wstring(name.begin(), name.end());
#else
    mkdir("logs", 0755);                      // 已存在时静默失败
    return std::string("logs/") + name;
#endif
}

// ---------------- Qt 日志转发（仅 Qt 环境编译） ----------------

#ifdef APPLOG_HAVE_QT
// Qt 日志（qDebug/qWarning 等）转发到 spdlog
inline void qtMessageHandler(QtMsgType type, const QMessageLogContext &context,
                             const QString &msg)
{
    const char *file = context.file ? context.file : "";
    const int line = context.line;
    switch (type) {
    case QtDebugMsg:    LOG_DEBUG("Qt: {} ({}:{})", msg.toStdString(), file, line); break;
    case QtInfoMsg:     LOG_INFO("Qt: {}", msg.toStdString());                      break;
    case QtWarningMsg:  LOG_WARN("Qt: {} ({}:{})", msg.toStdString(), file, line);  break;
    case QtCriticalMsg: LOG_ERROR("Qt: {} ({}:{})", msg.toStdString(), file, line); break;
    case QtFatalMsg:
        LOG_ERROR("Qt(fatal): {} ({}:{})", msg.toStdString(), file, line);
        // Qt 约定：fatal 消息后处理器必须终止进程，否则继续运行属未定义行为
        std::abort();
        break;
    }
}
#endif // APPLOG_HAVE_QT

} // namespace detail

// 初始化控制台与 logger；重复调用无副作用，仅首次生效
inline void init()
{
    static bool inited = false;
    if (inited)
        return;
    inited = true;

#ifdef _WIN32
    detail::setupConsole();
#endif

    // 控制台 sink：彩色、短格式，便于实时观察
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    // 文件 sink：滚动 5MB x 3 份，全时间戳格式
    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                detail::logFilePath(), 5 * 1024 * 1024, 3);
    fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [tid %t] %v");

    auto lg = std::make_shared<spdlog::logger>(
                "frameplayer", spdlog::sinks_init_list{consoleSink, fileSink});
    lg->set_level(spdlog::level::debug);       // debug 及以上同时上控制台与文件
    lg->flush_on(spdlog::level::info);         // info 及以上立即落盘，异常退出不丢日志
    spdlog::register_logger(lg);
    detail::logger() = lg;

#ifdef APPLOG_HAVE_QT
    // Qt 自身的日志也汇进来（仅 Qt 环境）
    qInstallMessageHandler(detail::qtMessageHandler);
#endif
}

} // namespace AppLog
