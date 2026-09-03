#pragma once

#include <QThread>
#include <QByteArray>
#include <QString>
#include <atomic>

// 子线程顺序读帧器（数据源「本地文件（子线程）」）
// 工作方式：
//   run() 内 while + msleep 循环：seek 读一帧 → emit frameReady → 睡到下一帧周期，
//   到文件末尾自动回卷循环播放
// 线程安全说明：
//   - QFile 不可跨线程共享：线程内自行打开独立只读句柄（与主线程句柄共存无冲突）
//   - 控制量全部走 std::atomic，GUI 线程随时可改，无需加锁
//   - 帧数据经信号槽队列连接发回 GUI 线程（QByteArray 隐式共享，拷贝开销小）
class FrameReadThread : public QThread
{
    Q_OBJECT

public:
    explicit FrameReadThread(QObject *parent = nullptr);

    // 设置读取参数（仅在线程未运行时调用）
    void setParams(const QString &path, int frameSize, int totalFrames, int fps);

    // 请求停止：仅置标志位，run() 循环退出；调用方需再 wait() 等线程真正结束
    void stopThread();

    // 暂停 / 继续（暂停期间循环小睡空转，保持文件位置与帧号）
    void setPaused(bool paused);

    // 运行中调整帧率（下一帧周期生效）
    void setFps(int fps);

    // 请求跳转到指定帧（下一次循环生效）
    void seekTo(int index);

signals:
    // 读到一帧（跨线程队列连接到 GUI）
    void frameReady(const QByteArray &data, int index);
    // 文件打开失败（路径随信号带出，便于提示）
    void openFailed(const QString &path);

protected:
    void run() override;

private:
    QString m_path;
    int     m_frameSize   = 0;
    int     m_totalFrames = 0;

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<int>  m_fps{25};
    std::atomic<int>  m_seekFrame{-1};   // -1 表示无跳转请求
};
