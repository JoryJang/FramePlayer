#include "FrameReadThread.h"
#include "Logger.h"

#include <QFile>
#include <QElapsedTimer>

FrameReadThread::FrameReadThread(QObject *parent)
    : QThread(parent)
{}

void FrameReadThread::setParams(const QString &path, int frameSize,
                                int totalFrames, int fps)
{
    m_path        = path;
    m_frameSize   = frameSize;
    m_totalFrames = totalFrames;
    m_fps         = fps;
    m_stop        = false;
    m_paused      = false;
    m_seekFrame   = -1;
}

void FrameReadThread::stopThread()
{
    m_stop = true;
}

void FrameReadThread::setPaused(bool paused)
{
    m_paused = paused;
}

void FrameReadThread::setFps(int fps)
{
    if (fps > 0)
        m_fps = fps;
}

void FrameReadThread::seekTo(int index)
{
    if (index >= 0 && index < m_totalFrames)
        m_seekFrame = index;
}

void FrameReadThread::run()
{
    if (m_frameSize <= 0 || m_totalFrames <= 0)
        return;

    // 线程内独立打开文件句柄（QFile 不可跨线程共享）
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_ERROR("[读帧线程] 打开文件失败: {}", m_path.toStdString());
        emit openFailed(m_path);
        return;
    }
    LOG_INFO("[读帧线程] 启动: {} | 帧大小 {}B | 总 {} 帧 | {} FPS",
             m_path.toStdString(), m_frameSize, m_totalFrames, m_fps.load());

    QByteArray buf;
    buf.resize(m_frameSize);
    int idx = 0;
    long long readCount = 0;   // 本次运行累计读出的帧数

    QElapsedTimer elapsed;
    while (!m_stop) {
        // 暂停：小睡空转，保持现场，可随时恢复或响应停止
        if (m_paused) {
            msleep(10);
            continue;
        }

        // 处理 GUI 侧的跳转请求
        const int pending = m_seekFrame.exchange(-1);
        if (pending >= 0) {
            LOG_DEBUG("[读帧线程] 跳转到第 {} 帧", pending + 1);
            idx = pending;
        }

        elapsed.start();
        if (!file.seek(static_cast<qint64>(idx) * m_frameSize))
            break;
        const qint64 n = file.read(buf.data(), m_frameSize);
        if (n < m_frameSize)
            break;   // 读不足一帧（如文件被外部截断），直接退出
        emit frameReady(buf, idx);
        ++readCount;

        // 每 100 帧记一条计数日志，控制台实时观察子线程进度
        if ((idx + 1) % 100 == 0)
            LOG_INFO("[读帧线程] 已读第 {}/{} 帧", idx + 1, m_totalFrames);

        idx = (idx + 1) % m_totalFrames;   // 到末尾回卷循环播放

        // 睡到下一帧周期：扣除读帧耗时；
        // 按 10ms 小步睡眠，以便及时响应停止 / 暂停 / 帧率调整
        int remain = 1000 / m_fps.load() - static_cast<int>(elapsed.elapsed());
        while (remain > 0 && !m_stop && !m_paused) {
            const int step = qMin(remain, 10);
            msleep(static_cast<unsigned long>(step));
            remain -= step;
        }
    }

    LOG_INFO("[读帧线程] 退出，本次共读取 {} 帧", readCount);
}
