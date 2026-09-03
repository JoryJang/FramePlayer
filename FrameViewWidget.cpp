#include "FrameViewWidget.h"
#include "Logger.h"

#include <QPainter>
#include <QPaintEvent>
#include <QThread>
#include <climits>
#include <utility>

// 帧率统计窗口：用最近 30 帧的到达间隔估算 FPS
static const int kFpsWindowSize = 30;

FrameViewWidget::FrameViewWidget(QWidget *parent)
    : QWidget(parent)
{
    // 黑底，避免图像未铺满时露出默认底色
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);

    setMinimumSize(320, 240);
    m_fpsTimer.start();

    // 1 秒内没有新帧则显示 FPS: 0（而不是停留在最后一次的旧读数上）
    m_idleTimer.setInterval(1000);
    m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer, &QTimer::timeout, this, &FrameViewWidget::onIdleTimeout);
}

int FrameViewWidget::bytesPerPixel(PixelFormat fmt)
{
    switch (fmt) {
    case PixelFormat::RGB888: return 3;
    case PixelFormat::RGB565: return 2;
    case PixelFormat::Gray8:  return 1;
    }
    Q_UNREACHABLE();
    return 0;
}

QImage::Format FrameViewWidget::toQImageFormat(PixelFormat fmt)
{
    switch (fmt) {
    case PixelFormat::RGB888: return QImage::Format_RGB888;
    case PixelFormat::RGB565: return QImage::Format_RGB16;   // RGB565，随运行平台字节序
    case PixelFormat::Gray8:  return QImage::Format_Grayscale8;
    }
    Q_UNREACHABLE();
    return QImage::Format_Invalid;
}

QString FrameViewWidget::formatName(PixelFormat fmt)
{
    switch (fmt) {
    case PixelFormat::RGB888: return QStringLiteral("RGB888");
    case PixelFormat::RGB565: return QStringLiteral("RGB565");
    case PixelFormat::Gray8:  return QStringLiteral("灰度8位");
    }
    Q_UNREACHABLE();
    return QString();
}

void FrameViewWidget::setFormat(PixelFormat fmt, int width, int height)
{
    m_format = fmt;
    m_width  = width;
    m_height = height;
    clear();
}

int FrameViewWidget::frameBytes() const
{
    if (m_width <= 0 || m_height <= 0)
        return 0;
    return m_width * m_height * bytesPerPixel(m_format);
}

bool FrameViewWidget::showFrame(const uchar *data, int len,
                                int width, int height, PixelFormat fmt)
{
    if (data == nullptr || width <= 0 || height <= 0)
        return false;

    // qint64 防溢出校验：width/height 可能来自不可信的外部/协议数据，
    // int 相乘可能溢出为负数或极小正数，使 len 校验失效造成越界读
    const qint64 need = qint64(width) * qint64(height) * bytesPerPixel(fmt);
    if (need <= 0 || need > INT_MAX || qint64(len) < need)
        return false;

    if (QThread::currentThread() == thread()) {
        // GUI 线程：直接深拷贝并同步显示
        QByteArray frame(reinterpret_cast<const char *>(data), int(need));
        return presentFrame(std::move(frame), width, height, fmt, nullptr);
    }

    // 非 GUI 线程：先在调用线程深拷贝（函数返回后调用方缓冲区即可复用/释放），
    // 再把帧投递到控件所在线程的事件队列，由其执行真正的显示逻辑
    QByteArray frame(reinterpret_cast<const char *>(data), int(need));
    QMetaObject::invokeMethod(this,
        [this, frame = std::move(frame), width, height, fmt]() mutable {
            presentFrame(std::move(frame), width, height, fmt, nullptr);
        },
        Qt::QueuedConnection);
    return true;
}

bool FrameViewWidget::showFrame(QByteArray frame, int width, int height, PixelFormat fmt,
                                std::function<void(QByteArray)> releaser)
{
    if (width <= 0 || height <= 0) {
        if (releaser)
            releaser(std::move(frame));
        return false;
    }

    const qint64 need = qint64(width) * qint64(height) * bytesPerPixel(fmt);
    if (need <= 0 || need > INT_MAX || qint64(frame.size()) < need) {
        // 数据不足一帧：原样经调用方回调归还，避免帧池槽位泄漏
        if (releaser)
            releaser(std::move(frame));
        return false;
    }

    if (QThread::currentThread() == thread())
        // GUI 线程：直接接管所有权并同步显示
        return presentFrame(std::move(frame), width, height, fmt, std::move(releaser));

    // 非 GUI 线程：把帧连同归还回调一并投递到控件所在线程，
    // 渲染端零拷贝；缓冲区归还时机统一由 GUI 线程内的 presentFrame 决定
    QMetaObject::invokeMethod(this,
        [this, frame = std::move(frame), width, height, fmt,
         releaser = std::move(releaser)]() mutable {
            presentFrame(std::move(frame), width, height, fmt, std::move(releaser));
        },
        Qt::QueuedConnection);
    return true;
}

bool FrameViewWidget::presentFrame(QByteArray frame, int width, int height, PixelFormat fmt,
                                   std::function<void(QByteArray)> releaser)
{
    // 归还上一帧缓冲区：必须用“上一帧自己的回调”（m_bufferReleaser）归还，
    // 保证缓冲区回到它在帧池中的槽位。若误用本帧回调归还上一帧缓冲，
    // 槽位与缓冲会错配，该槽位下次被复用时可能拿到尺寸不符的缓冲区，
    // 大帧 memcpy 越界写入进而崩溃。
    if (m_bufferReleaser)
        m_bufferReleaser(std::move(m_buffer));
    m_bufferReleaser = std::move(releaser);

    // 接管所有权（move），省去大帧（如 2048x2048x3 = 12MB）的每帧深拷贝
    m_buffer = std::move(frame);
    m_format = fmt;
    m_width  = width;
    m_height = height;

    // QImage 浅包装 m_buffer 的数据，二者同生命周期（m_buffer 由本类持有，
    // 不依赖调用方内存存活，避免悬空指针）
    m_image = QImage(reinterpret_cast<const uchar *>(m_buffer.constData()),
                     m_width, m_height, m_width * bytesPerPixel(m_format),
                     toQImageFormat(m_format));
    refreshScaled();   // 新帧到达：重建缩放缓存（paintEvent 不再实时缩放）

    // ---- 实测帧率统计：最近 N 帧时间窗口 ----
    const qint64 now = m_fpsTimer.elapsed();
    m_frameStamps.enqueue(now);
    while (m_frameStamps.size() > kFpsWindowSize)
        m_frameStamps.dequeue();
    if (m_frameStamps.size() >= 2) {
        const qint64 span = m_frameStamps.last() - m_frameStamps.first();
        if (span > 0)
            m_fps = (m_frameStamps.size() - 1) * 1000.0 / span;
    }

    // 每 1 秒记一条实测帧率，控制台实时观察
    if (now - m_lastFpsLogMs >= 1000) {
        m_lastFpsLogMs = now;
        LOG_DEBUG("[显示] 实测 {:.1f} FPS（目标帧已显示 {} 帧）",
                  m_fps, m_frameStamps.size());
    }
    emit fpsChanged(m_fps);

    m_idleTimer.start();   // 有新帧：重置无帧超时计时
    update();              // 触发重绘
    return true;
}

void FrameViewWidget::onIdleTimeout()
{
    // 超过 1 秒没有新帧：FPS 归零并清空统计窗口，而不是停留在最后一次旧读数上
    if (m_fps != 0.0) {
        m_fps = 0.0;
        m_frameStamps.clear();
        emit fpsChanged(m_fps);
        update();
    }
}

void FrameViewWidget::clear()
{
    // 若当前持有帧池缓冲区，先经其回调归还，避免槽位泄漏
    if (m_bufferReleaser) {
        m_bufferReleaser(std::move(m_buffer));
        m_bufferReleaser = nullptr;
    }
    m_buffer.clear();
    m_image = QImage();
    m_scaled = QImage();
    m_frameStamps.clear();
    m_fps = 0.0;
    m_idleTimer.stop();
    update();
}

void FrameViewWidget::refreshScaled()
{
    if (m_image.isNull() || width() <= 0 || height() <= 0) {
        m_scaled = QImage();
        return;
    }
    // 目标尺寸：图像等比缩放进控件；控件未大于图像时直接用原图（零开销）
    const QSize target = m_image.size().scaled(size(), Qt::KeepAspectRatio);
    if (target == m_image.size()) {
        m_scaled = m_image;   // 浅拷贝，共享数据，无额外内存
        return;
    }

    // 大帧（如 2048x2048）关闭平滑插值：双线性缩放每帧代价过高导致卡顿，
    // 近邻缩放快一个数量级，动态播放中视觉差异不明显；
    // 大帧只在此处重采样一次，paintEvent 直接画缓存
    const qint64 pixels = qint64(m_image.width()) * m_image.height();
    const Qt::TransformationMode mode =
        (pixels <= qint64(1024) * 768) ? Qt::SmoothTransformation
                                        : Qt::FastTransformation;
    m_scaled = m_image.scaled(target, Qt::KeepAspectRatio, mode);
}

void FrameViewWidget::resizeEvent(QResizeEvent *event)
{
    // 控件尺寸变化：按新尺寸重建缩放缓存
    refreshScaled();
    QWidget::resizeEvent(event);
}

QSize FrameViewWidget::sizeHint() const
{
    if (m_width > 0 && m_height > 0)
        return QSize(m_width, m_height);
    return QSize(640, 480);
}

void FrameViewWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);

    // ---- 绘制图像：直接画缩放缓存并居中（缩放已在 presentFrame/resize 时完成） ----
    if (!m_scaled.isNull()) {
        const QRect target((width() - m_scaled.width()) / 2,
                           (height() - m_scaled.height()) / 2,
                           m_scaled.width(), m_scaled.height());
        p.drawImage(target, m_scaled);
    } else {
        p.setPen(QColor(120, 120, 120));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("无图像数据\n可拖入文件，或在右侧选择「模拟数据」"));
    }

    // ---- 左上角信息叠加层：宽高 + 格式 + 实测帧率 ----
    if (m_width > 0 && m_height > 0) {
        const QString info = QStringLiteral("%1 x %2  %3  %4 FPS")
                .arg(m_width)
                .arg(m_height)
                .arg(formatName(m_format))
                .arg(m_fps, 0, 'f', 1);

        QFont f = p.font();
        f.setPointSize(10);
        f.setBold(true);
        p.setFont(f);

        const QFontMetrics fm(f);
        const QRect textRect = fm.boundingRect(info).adjusted(-8, -5, 8, 5);
        const QRect box(6, 6, textRect.width(), textRect.height());

        // 半透明深色底，保证任意画面下文字可读
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 160));
        p.drawRoundedRect(box, 4, 4);

        p.setPen(QColor(80, 255, 120));   // 亮绿色文字
        p.drawText(box, Qt::AlignCenter, info);
    }
}
