#include "CppFrameSource.h"
#include "Logger.h"

#include <chrono>
#include <cmath>
#include <codecvt>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <locale>

namespace {
const char *modeName(CppFrameSource::Mode m)
{
    return m == CppFrameSource::Mode::LocalFile ? "本地文件" : "模拟数据";
}
const char *formatName(CppFrameSource::Format f)
{
    switch (f) {
    case CppFrameSource::Format::RGB888: return "RGB888";
    case CppFrameSource::Format::RGB565: return "RGB565";
    case CppFrameSource::Format::Gray8:   return "Gray8";
    }
    return "?";
}
// 宽字符串 → UTF-8（用于 spdlog 日志输出，窄字符格式串 + UTF-8 内容）
std::string wtoUtf8(const std::wstring &w)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> cv;
    return cv.to_bytes(w);
}
} // namespace

CppFrameSource::~CppFrameSource()
{
    stop();
}

int CppFrameSource::bytesPerPixel(Format fmt)
{
    switch (fmt) {
    case Format::RGB888: return 3;
    case Format::RGB565: return 2;
    case Format::Gray8:  return 1;
    }
    return 0;
}

void CppFrameSource::configure(Mode mode, const std::wstring &path,
                               int frameSize, int totalFrames, int fps,
                               Format fmt, int width, int height)
{
    m_mode        = mode;
    m_path        = path;
    m_frameSize   = frameSize;
    m_totalFrames = totalFrames;
    m_fps         = fps;
    m_format      = fmt;
    m_width       = width;
    m_height      = height;

    m_stop       = false;
    m_paused     = false;
    m_openFailed = false;
    m_readFailed = false;
    m_seekFrame  = -1;
    m_dropped    = 0;

    // 重建帧槽缓冲池（调用时线程已停止、消费者已归还持有槽）
    m_freeRing.clear_unsafe();
    m_filledRing.clear_unsafe();
    m_pool.clear();
    m_pool.reserve(kPoolSize);
    for (int i = 0; i < kPoolSize; ++i) {
        auto f = std::unique_ptr<Frame>(new Frame);
        f->data.resize(static_cast<size_t>(frameSize));   // 预分配帧内存，运行期零分配
        m_pool.push_back(std::move(f));
        m_freeRing.push(i);
    }
}

bool CppFrameSource::start()
{
    if (m_running.load())
        return false;
    if (m_frameSize <= 0 || m_totalFrames <= 0)
        return false;

    m_stop = false;
    m_thread = std::thread(&CppFrameSource::threadMain, this);
    return true;
}

void CppFrameSource::stop()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

void CppFrameSource::setPaused(bool paused)
{
    m_paused = paused;
}

void CppFrameSource::setFps(int fps)
{
    if (fps > 0)
        m_fps = fps;
}

void CppFrameSource::seekTo(int index)
{
    if (index >= 0 && index < m_totalFrames)
        m_seekFrame = index;
}

CppFrameSource::Frame *CppFrameSource::acquireFrame()
{
    // 排空已填充环只留最新一帧，中间帧直接回收进空闲环
    int slot, last = -1;
    while (m_filledRing.pop(slot)) {
        if (last >= 0)
            m_freeRing.push(last);
        last = slot;
    }
    return last >= 0 ? m_pool[static_cast<size_t>(last)].get() : nullptr;
}

void CppFrameSource::releaseFrame(Frame *f)
{
    if (!f)
        return;
    for (int i = 0; i < kPoolSize; ++i)
        if (m_pool[static_cast<size_t>(i)].get() == f) {
            m_freeRing.push(i);
            return;
        }
}

// ---------------- 子线程主循环 ----------------

void CppFrameSource::threadMain()
{
    m_running = true;

    // 文件模式：线程内独立打开句柄
    std::ifstream file;
    if (m_mode == Mode::LocalFile) {
        file.open(m_path, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERROR("[纯C++线程] 打开文件失败: {}", wtoUtf8(m_path));
            m_openFailed = true;
            m_running = false;
            return;
        }
        LOG_INFO("[纯C++线程] 启动: {} | {} | 帧大小 {}B | 总 {} 帧 | {} FPS",
                 wtoUtf8(m_path), modeName(m_mode), m_frameSize, m_totalFrames, m_fps.load());
    } else {
        initParticles();
        LOG_INFO("[纯C++线程] 启动: {} | {} {}x{} | 帧大小 {}B | 总 {} 帧 | {} FPS",
                 modeName(m_mode), formatName(m_format), m_width, m_height,
                 m_frameSize, m_totalFrames, m_fps.load());
    }

    int idx = 0;
    long long frameCount = 0;   // 本次运行累计产出帧数

    while (!m_stop.load()) {
        // 暂停：小睡空转，保持现场，可随时恢复或响应停止
        if (m_paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 处理跳转请求
        const int pending = m_seekFrame.exchange(-1);
        if (pending >= 0) {
            LOG_DEBUG("[纯C++线程] 跳转到第 {} 帧", pending + 1);
            idx = pending;
        }

        const auto tickStart = std::chrono::steady_clock::now();

        // 从空闲环取槽；池满说明消费滞后，丢弃本帧（下一帧很快就来）
        int slot = -1;
        if (!m_freeRing.pop(slot)) {
            if (++m_dropped % 100 == 1)
                LOG_DEBUG("[纯C++线程] 消费滞后，丢弃新帧（累计 {}）", m_dropped);
        } else {
            Frame &f = *m_pool[static_cast<size_t>(slot)];
            f.index = idx;

            if (m_mode == Mode::LocalFile) {
                file.seekg(static_cast<std::streamoff>(idx) * m_frameSize, std::ios::beg);
                if (!file.good()) {
                    // seek 失败（如文件被外部截断）：清标志重试一次，仍失败则退出
                    file.clear();
                    file.seekg(static_cast<std::streamoff>(idx) * m_frameSize, std::ios::beg);
                    if (!file.good()) {
                        m_readFailed = true;     // 通知 GUI 播放异常终止
                        m_freeRing.push(slot);   // 退出前归还槽位
                        break;
                    }
                }
                file.read(reinterpret_cast<char *>(f.data.data()), m_frameSize);
                if (file.gcount() < m_frameSize) {
                    m_readFailed = true;         // 通知 GUI 播放异常终止
                    m_freeRing.push(slot);
                    break;   // 读不足一帧，退出
                }
            } else {
                makeSimFrame(idx, f.data);
            }

            m_filledRing.push(slot);   // 发布给消费者（零拷贝，仅移交槽所有权）
            ++frameCount;
        }

        // 每 100 帧记一条计数日志
        if ((idx + 1) % 100 == 0)
            LOG_INFO("[纯C++线程] 已产出第 {}/{} 帧", idx + 1, m_totalFrames);

        idx = (idx + 1) % m_totalFrames;   // 到末尾回卷循环

        // 睡到下一帧周期：扣除本帧耗时；10ms 小步睡眠及时响应控制量
        int remain = 1000 / m_fps.load()
                - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - tickStart).count());
        while (remain > 0 && !m_stop.load() && !m_paused.load()) {
            const int step = remain < 10 ? remain : 10;
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            remain -= step;
        }
    }

    if (file.is_open())
        file.close();
    LOG_INFO("[纯C++线程] 退出，本次共产出 {} 帧，丢弃 {} 帧", frameCount, m_dropped);
    m_running = false;
}

// ---------------- 模拟帧生成：流线型动态粒子 ----------------

void CppFrameSource::initParticles()
{
    // 粒子数量随画面面积缩放，保证不同分辨率下密度观感一致
    int count = m_width * m_height / 300;
    if (count < 80)   count = 80;
    if (count > 2000) count = 2000;

    m_particles.resize(static_cast<size_t>(count));
    for (auto &pt : m_particles) {
        pt.x   = static_cast<float>(std::rand() % (m_width  > 0 ? m_width  : 1));
        pt.y   = static_cast<float>(std::rand() % (m_height > 0 ? m_height : 1));
        pt.hue = static_cast<float>(std::rand() % 360);   // 每颗粒子固定基色
    }
    m_trail.assign(static_cast<size_t>(m_width) * m_height * 3, 0);
}

void CppFrameSource::makeSimFrame(int index, std::vector<unsigned char> &buf)
{
    const int w = m_width;
    const int h = m_height;
    if (w <= 0 || h <= 0)
        return;
    if (m_trail.size() != static_cast<size_t>(w) * h * 3)
        initParticles();

    const float t = static_cast<float>(index) * 0.05f;

    // 1) 拖尾衰减：整体亮度每帧按 88% 衰减，粒子轨迹自然拉出流线
    for (auto &v : m_trail)
        v = static_cast<unsigned char>((v * 225) >> 8);

    // 2) 推进粒子：伪流场 = 两组正弦叠加，角度随位置与时间平滑变化
    for (auto &pt : m_particles) {
        const float angle =
                std::sin(pt.x * 0.012f + t) * 1.8f +
                std::cos(pt.y * 0.015f - t * 0.7f) * 1.8f +
                std::sin((pt.x + pt.y) * 0.006f + t * 0.4f);
        pt.x += std::cos(angle) * 1.6f;
        pt.y += std::sin(angle) * 1.6f;

        // 出界回卷到对侧，保持粒子数量恒定
        if (pt.x < 0)        pt.x += w;
        if (pt.x >= w)       pt.x -= w;
        if (pt.y < 0)        pt.y += h;
        if (pt.y >= h)       pt.y -= h;

        // 3) 着色：基色 + 时间缓变，HSV 简化转 RGB（饱和度拉满）
        const float hue = pt.hue + t * 20.0f;
        const int   hi  = (static_cast<int>(hue) / 60) % 6;
        const float f   = hue / 60.0f - std::floor(hue / 60.0f);
        const unsigned char q = static_cast<unsigned char>(255 * (1.0f - f));
        const unsigned char s = static_cast<unsigned char>(255 * f);
        unsigned char r, g, b;
        switch (hi) {
        case 0:  r = 255; g = s;   b = 0;   break;
        case 1:  r = q;   g = 255; b = 0;   break;
        case 2:  r = 0;   g = 255; b = s;   break;
        case 3:  r = 0;   g = q;   b = 255; break;
        case 4:  r = s;   g = 0;   b = 255; break;
        default: r = 255; g = 0;   b = q;   break;
        }

        // 4) 叠加到拖尾缓冲（饱和加法，粒子重叠处更亮）
        unsigned char *px = &m_trail[(static_cast<int>(pt.y) * w
                                      + static_cast<int>(pt.x)) * 3];
        px[0] = static_cast<unsigned char>(px[0] + (r >> 1) > 255 ? 255 : px[0] + (r >> 1));
        px[1] = static_cast<unsigned char>(px[1] + (g >> 1) > 255 ? 255 : px[1] + (g >> 1));
        px[2] = static_cast<unsigned char>(px[2] + (b >> 1) > 255 ? 255 : px[2] + (b >> 1));
    }

    // 5) 按当前像素格式从拖尾缓冲转出一帧
    buf.resize(static_cast<size_t>(m_frameSize));
    unsigned char *p = buf.data();
    const unsigned char *src = m_trail.data();
    switch (m_format) {
    case Format::RGB888:
        std::memcpy(p, src, static_cast<size_t>(w) * h * 3);
        break;
    case Format::RGB565:
        for (int i = 0; i < w * h; ++i, src += 3) {
            const unsigned short v = static_cast<unsigned short>(
                        ((src[0] >> 3) << 11) | ((src[1] >> 2) << 5) | (src[2] >> 3));
            *p++ = static_cast<unsigned char>(v & 0xFF);   // 低字节在前
            *p++ = static_cast<unsigned char>(v >> 8);
        }
        break;
    case Format::Gray8:
        for (int i = 0; i < w * h; ++i, src += 3)
            *p++ = static_cast<unsigned char>(
                        (src[0] * 77 + src[1] * 150 + src[2] * 29) >> 8);
        break;
    }
}
