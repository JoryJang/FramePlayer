#pragma once

// ============================================================
// 纯 C++ 子线程帧数据源（完全不依赖 Qt，可独立移植）
// 依赖：仅 C++ 标准库（std::thread / std::ifstream / std::atomic）
//        + spdlog 日志（经 Logger.h 的 LOG_* 宏，纯 C++ 头文件库）
//
// 两种工作模式：
//   1. LocalFile —— 子线程内 while + sleep 顺序 seek 读取本地二进制文件帧，
//      到末尾回卷循环；线程内独立打开句柄（std::ifstream）
//   2. Simulate  —— 子线程内实时生成流线型动态粒子帧（RGB888/RGB565/Gray8）：
//      粒子群沿伪流场（正弦叠加噪声）流动，画面每帧整体衰减形成拖尾
//
// 取帧方式（拉模式，零拷贝）：
//   帧槽缓冲池（4 个预分配槽位，无每帧内存分配）+ 双 SPSC 无锁环形缓冲：
//     freeRing   空闲槽（消费者归还 → 生产者取用）
//     filledRing 已填充槽（生产者发布 → 消费者取用）
//   消费者 acquireFrame() 取走最新一帧（排空只留最新），
//   用完后 releaseFrame() 归还槽位；持有期间槽内数据稳定可读
//
// 线程安全：
//   控制量全部走 std::atomic，任意线程可随时调用 pause/setFps/seekTo/stop；
//   帧槽经 SPSC 环做所有权移交，同一时刻只有一个线程访问某个槽
// ============================================================

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "SpscRingBuffer.h"

class CppFrameSource
{
public:
    // 像素格式（与 UI 选择对应）
    enum class Format { RGB888, RGB565, Gray8 };

    // 工作模式
    enum class Mode { LocalFile, Simulate };

    // 一帧数据（出队交付）
    struct Frame {
        int              index = 0;    // 帧号（0 起）
        std::vector<unsigned char> data;
    };

    CppFrameSource() = default;
    ~CppFrameSource();                  // 自动 stop 并 join

    CppFrameSource(const CppFrameSource &) = delete;
    CppFrameSource &operator=(const CppFrameSource &) = delete;

    // 设置参数（仅在线程未运行时调用）
    //   mode        本地文件 / 模拟数据
    //   path        本地文件路径（Simulate 模式忽略）
    //   frameSize   每帧字节数
    //   totalFrames 总帧数（模拟模式为设定的模拟帧数）
    //   fps         帧率
    //   fmt/w/h     像素格式与宽高（仅 Simulate 模式生成图案用）
    // path 使用 std::wstring：MSVC 下 ifstream(wchar_t*) 原生支持任意 Unicode 路径，
    // 避免 QString::toStdString() 返回 UTF-8 在 GBK 系统代码页下乱码的问题
    void configure(Mode mode, const std::wstring &path,
                   int frameSize, int totalFrames, int fps,
                   Format fmt, int width, int height);

    bool start();                       // 启动子线程；已在运行返回 false
    void stop();                        // 请求停止并 join 等待退出（幂等）
    bool isRunning() const { return m_running.load(); }

    void setPaused(bool paused);        // 暂停/继续（暂停时小睡空转保持现场）
    void setFps(int fps);               // 运行中调整帧率（下一帧周期生效）
    void seekTo(int index);             // 请求跳转到指定帧（下次循环生效）

    // ---- 消费端接口（仅单一消费者线程调用，如 GUI 定时器） ----

    // 取最新一帧：排空已填充环只留最新，中间帧直接回收；
    // 返回帧槽指针（持有期间数据稳定），无新帧返回 nullptr
    Frame *acquireFrame();

    // 归还帧槽到空闲环（acquireFrame 拿到的槽必须归还，否则池会耗尽）
    void releaseFrame(Frame *f);

    // 打开文件是否失败过（start 后轮询检查；失败时线程已自行退出）
    bool openFailed() const { return m_openFailed.load(); }
    std::wstring filePath() const { return m_path; }

private:
    void threadMain();                  // 子线程主循环
    void makeSimFrame(int index, std::vector<unsigned char> &buf);
    void initParticles();               // 初始化/重置粒子群与拖尾缓冲

    static int bytesPerPixel(Format fmt);

    Mode        m_mode = Mode::LocalFile;
    std::wstring m_path;
    int         m_frameSize   = 0;
    int         m_totalFrames = 0;
    Format      m_format = Format::RGB888;
    int         m_width  = 0;
    int         m_height = 0;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_openFailed{false};
    std::atomic<int>  m_fps{25};
    std::atomic<int>  m_seekFrame{-1};  // -1 表示无跳转请求

    // ---- 帧槽缓冲池 + 双 SPSC 环（零拷贝、无每帧分配） ----
    static constexpr int kPoolSize = 4;             // 预分配槽位数
    std::vector<std::unique_ptr<Frame>> m_pool;     // 槽位本体（configure 时预分配）
    SpscRingBuffer<int> m_freeRing{kPoolSize};      // 空闲槽：消费者归还 / 生产者取用
    SpscRingBuffer<int> m_filledRing{kPoolSize};    // 已填充槽：生产者发布 / 消费者取用
    long long m_dropped = 0;                        // 池满丢弃的新帧计数（仅子线程写）

    // 粒子模拟状态（仅子线程访问）：粒子坐标、底色相位、拖尾缓冲（RGB888）
    struct Particle { float x, y; float hue; };
    std::vector<Particle>      m_particles;
    std::vector<unsigned char> m_trail;      // w*h*3，每帧衰减形成流线拖尾
};
