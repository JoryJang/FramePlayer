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
//   - 帧数据经信号槽队列连接发回 GUI 线程（QByteArray 隐式共享，零拷贝）
//
// 零拷贝与背压（帧槽轮转池）：
//   若复用同一个 QByteArray 反复 emit，隐式共享会让引用计数 +1，
//   下一轮 data() 会触发写时复制——每帧一次整体深拷贝（大帧代价极高，零拷贝形同虚设）。
//   改为 kSlotCount 份独立缓冲循环写入，并限制在途未消费帧数，
//   保证 GUI 释放后才绕回写同一槽：运行期零拷贝、零内存分配。
class FrameReadThread : public QThread
{
    Q_OBJECT

public:
    explicit FrameReadThread(QObject *parent = nullptr);

    // 设置读取参数（仅在线程未运行时调用）
    // 会重置帧槽轮转池与在途计数；帧大小变化时重新分配槽缓冲
    void setParams(const QString &path, int frameSize, int totalFrames, int fps);

    // GUI 侧每派发（消费）完一帧调用一次：在途帧计数 -1，用于背压
    // 仅做原子自减，任意时刻调用均安全；即使本帧被丢弃也应调用，
    // 否则在途计数会卡住导致后续帧全被跳过
    void frameConsumed();

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
    // 帧槽轮转池容量（份独立帧缓冲，循环写入）
    static constexpr int kSlotCount = 6;
    // 在途未消费帧上限：达到即跳过本帧（背压，避免事件队列无上限堆积）
    //
    // 取值约束：kSlotCount >= kMaxPending + 2（已用模拟验证，见下）
    // 推导：frameConsumed() 在 GUI 槽函数开头调用，使在途计数先减 1，
    //   因此分配槽时 GUI 侧「其他持有」 = 队列(≤ kMaxPending-1)
    //                                  + 派发中 1 帧 + m_threadFrameBuf 显示中 1 帧
    //                                ≤ kMaxPending + 1
    //   绕回写槽要不撞上这些持有槽，需 kSlotCount > kMaxPending + 1
    // 撞上不会损坏数据（QByteArray 写时复制），只是退化为一次深拷贝、零拷贝失效
    static constexpr int kMaxPending = kSlotCount - 2;

    QString m_path;
    int     m_frameSize   = 0;
    int     m_totalFrames = 0;

    QByteArray m_slots[kSlotCount];   // 帧槽轮转池（setParams / run 入口预分配）
    int        m_slotIdx  = 0;        // 下一个待写入的槽（仅子线程访问）

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<int>  m_fps{25};
    std::atomic<int>  m_seekFrame{-1};   // -1 表示无跳转请求
    std::atomic<int>  m_pending{0};      // 已 emit 但 GUI 尚未派发消费的帧数
};
