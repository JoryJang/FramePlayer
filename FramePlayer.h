#pragma once

#include <QtWidgets/QMainWindow>
#include <QTimer>
#include <QFile>
#include <QButtonGroup>
#include "ui_FramePlayer.h"
#include "FrameReadThread.h"
#include "CppFrameSource.h"

// 主窗口：二进制图像播放器
// 五种数据源（RadioButton 切换）：
//   1. 本地文件——打开或拖入任意格式的二进制文件，保持句柄打开、
//      按帧 seek 读取（不整文件进内存，支持超大文件）
//   2. 本地文件（子线程）——QThread 子线程内 while+msleep 顺序读帧，
//      经信号发回主线程显示，定时由子线程自持
//   3. 本地文件（纯C++线程）——不依赖 Qt 的 std::thread 子线程读帧，
//      帧进互斥锁队列，主线程 QTimer 轮询取帧显示
//   4. 模拟数据——按设定的格式/宽高实时生成移动渐变测试帧
//   5. 模拟数据（纯C++线程）——std::thread 子线程实时生成测试帧
// 播放控制：开始播放 / 暂停|继续 / 停止 三态状态机
class FramePlayer : public QMainWindow
{
    Q_OBJECT

public:
    FramePlayer(QWidget *parent = nullptr);
    ~FramePlayer();

protected:
    // 整个窗口区域统一接受文件拖放（含图像区：显示控件不接收拖放，事件上浮到主窗口）
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onOpenFile();                     // 打开本地文件
    void onStart();                        // 开始播放
    void onPauseResume();                  // 暂停 / 继续
    void onStop();                         // 停止（回到第 0 帧）
    void onTimerTick();                    // 定时器到期，送下一帧
    void onSliderMoved(int value);         // 拖动进度条跳转
    void onParamChanged();                 // 格式 / 宽高 / 模拟帧数变化
    void onSourceModeChanged();            // 数据源切换（本地文件 / 子线程 / 模拟数据）
    void onThreadFrame(const QByteArray &data, int index);  // QThread 子线程回发一帧
    void onThreadOpenFailed(const QString &path);           // QThread 子线程打开文件失败
    void onCppPollTick();                    // 纯C++线程模式：定时从帧队列取帧显示

private:
    // 播放状态机
    enum PlayState { Stopped, Playing, Paused };

    void loadFile(const QString &path);    // 加载本地文件（仅打开句柄）
    void showFrameAt(int index);           // 显示第 index 帧（按数据源取帧）
    void makeSimFrame(int index);          // 生成第 index 帧模拟数据到 m_simBuffer
    void updateFrameInfo();                // 刷新 "当前帧 / 总帧数" 标签
    void updateControlStates();            // 按状态机刷新各按钮可用性与文字
    int  bytesPerPixel() const;            // 当前选择的每像素字节数
    PixelFormat currentFormat() const;     // 当前选择的像素格式
    int  frameSize() const;                // 当前参数下每帧字节数
    bool isThreadMode() const;             // 当前是否为「本地文件（QThread 子线程）」模式
    bool isCppMode() const;                // 当前是否为纯C++线程模式（文件或模拟）
    bool isCppFileMode() const;            // 当前是否为「本地文件（纯C++线程）」模式
    bool isFileLikeMode() const;           // 三种本地文件模式之一
    bool isSimLikeMode() const;            // 两种模拟数据模式之一
    void stopReadThread();                 // 停止 QThread 子线程并等待其退出（未运行则无操作）
    void stopCppSource();                  // 停止纯C++线程数据源（未运行则无操作）

    Ui::FramePlayerClass ui;

    // 5 个数据源 RadioButton 的互斥组，统一发出切换通知。
    // 不能只监听 radioFile 的 toggled：那样「非 radioFile → 非 radioFile」
    // （如 子线程 → 模拟数据）时 radioFile 状态未变，收不到任何信号，
    // 会导致控件启用状态、总帧数与预览画面都不刷新
    QButtonGroup *m_sourceGroup = nullptr;

    QFile      m_file;          // 本地文件句柄（保持打开，按需 seek 读帧）
    QByteArray m_frameBuffer;   // 文件帧读取缓冲（复用，内存占用恒为单帧大小）
    QByteArray m_simBuffer;     // 模拟数据帧缓冲（复用，避免每帧分配）
    int        m_totalFrames = 0;
    int        m_currentFrame = 0;
    int        m_lastLoggedFrame = 0;   // 上次计数日志的帧号（主线程模式每 100 帧记一条）
    PlayState  m_state = Stopped;
    QTimer     m_playTimer;
    FrameReadThread *m_readThread = nullptr;   // QThread 子线程读帧器（按需创建，随窗口销毁）
    CppFrameSource  m_cppSource;               // 纯C++线程帧数据源（std::thread，无 Qt 依赖）
    QTimer          m_cppPollTimer;            // 纯C++模式的帧队列轮询定时器
    CppFrameSource::Frame *m_cppHeldFrame = nullptr;  // 当前持有显示的帧槽（下一帧到达时归还）
    QByteArray      m_threadFrameBuf;          // QThread 模式当前帧（隐式共享持有，保图像不悬空）
};
