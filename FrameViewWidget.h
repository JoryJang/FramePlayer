#pragma once

#include <QWidget>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <QQueue>
#include <QByteArray>
#include <functional>

// 像素格式：对应二进制文件中每帧的存储格式
enum class PixelFormat
{
    RGB888,   // 每像素 3 字节，R-G-B 顺序
    RGB565,   // 每像素 2 字节，RGB565（即 RGB16，低字节在前）
    Gray8     // 每像素 1 字节，8 位灰度
};

// 二进制图像帧显示控件（只负责显示，拖放由主窗口统一处理）
// 功能：
//   1. 按指定格式/宽高解析原始二进制帧并显示；帧数据由本类持有
//      （QImage 浅包装 m_buffer，不依赖调用方内存存活）
//   2. 线程安全：任意线程可调用 showFrame()；非 GUI 线程调用时
//      帧自动投递到控件所在线程显示，无需调用方关心线程模型
//   3. 图像等比缩放居中显示（黑底）；缩放结果按控件尺寸缓存，
//      仅在新帧到达或控件 resize 时重采样，paintEvent 只画缓存位图；
//      小帧用平滑插值、大帧用快速插值（质量/速度自适应）
//   4. 左上角叠加显示：图像宽高、像素格式、实测帧率（FPS）；
//      1 秒无新帧 FPS 自动归零（fpsChanged 信号同步通知）
//
// 两种送帧接口：
//   - 裸指针版 showFrame(data, len, ...)：内部深拷贝，函数返回后
//     调用方缓冲区即可复用/释放
//   - 所有权版 showFrame(QByteArray, ..., releaser)：move 进来零拷贝；
//     可选 releaser 回调用于帧池缓冲区归还（上一帧缓冲区在下一帧
//     到达时、clear() 时经其“自己的”回调归还，槽位不会错配）
class FrameViewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FrameViewWidget(QWidget *parent = nullptr);

    // 设置像素格式与图像宽高（单位：像素）
    void setFormat(PixelFormat fmt, int width, int height);

    // 当前格式下每帧的字节数；参数无效时返回 0
    int frameBytes() const;

    // 送入一帧原始数据并刷新显示（内部深拷贝，任意线程可调用）；
    // 每帧自带宽高与格式，与上次不同则自动切换；
    // 数据长度不足一帧时返回 false
    bool showFrame(const uchar *data, int len, int width, int height,
                   PixelFormat fmt);

    // 送入一帧并移交所有权（零拷贝，任意线程可调用）；
    // releaser 非空时，缓冲区在不再显示时经该回调归还（帧池场景）；
    // 数据长度不足一帧时缓冲区原样经 releaser 归还并返回 false
    bool showFrame(QByteArray frame, int width, int height, PixelFormat fmt,
                   std::function<void(QByteArray)> releaser = nullptr);

    // 清空画面与统计信息（持有帧池缓冲区时先经其回调归还）
    void clear();

    QSize sizeHint() const override;

    // 每像素字节数
    static int bytesPerPixel(PixelFormat fmt);
    // 格式显示名（用于叠加层与界面）
    static QString formatName(PixelFormat fmt);
    // PixelFormat -> QImage::Format
    static QImage::Format toQImageFormat(PixelFormat fmt);

signals:
    // 实测帧率变化（含 1 秒无新帧时的归零通知）
    void fpsChanged(double fps);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;   // 尺寸变化时重建缩放缓存

private slots:
    void onIdleTimeout();                     // 1 秒无新帧：FPS 归零

private:
    // 实际显示逻辑（必须在 GUI 线程调用）：
    // 接管帧所有权、归还上一帧缓冲区、重建缩放缓存、统计帧率
    bool presentFrame(QByteArray frame, int width, int height, PixelFormat fmt,
                      std::function<void(QByteArray)> releaser);
    void refreshScaled();                     // 按当前控件尺寸重建缩放缓存

    QByteArray m_buffer;                      // 当前帧数据（类持有，QImage 浅包装它）
    std::function<void(QByteArray)> m_bufferReleaser;  // 当前帧缓冲区归还回调（帧池）
    QImage      m_image;                      // 当前帧（浅包装 m_buffer）
    QImage      m_scaled;                     // 缩放缓存（控件大小），paintEvent 直接绘制
    PixelFormat m_format = PixelFormat::RGB888;
    int         m_width  = 0;
    int         m_height = 0;

    QElapsedTimer m_fpsTimer;                 // 帧率统计计时器
    QQueue<qint64> m_frameStamps;             // 最近若干帧到达时间戳（ms）
    double      m_fps = 0.0;                  // 实测帧率
    qint64      m_lastFpsLogMs = 0;           // 上次帧率日志时间（每 1 秒记一条）
    QTimer      m_idleTimer;                  // 1 秒无新帧则 FPS 归零
};
