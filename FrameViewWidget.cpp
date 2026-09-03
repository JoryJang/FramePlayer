#include "FrameViewWidget.h"
#include "Logger.h"

#include <QPainter>
#include <QPaintEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

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

    // 接受文件拖放
    setAcceptDrops(true);
}

void FrameViewWidget::dragEnterEvent(QDragEnterEvent *event)
{
    // 文件格式不限：只要拖入的是本地文件就接受
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void FrameViewWidget::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty()) {
        const QString path = urls.first().toLocalFile();
        if (!path.isEmpty())
            emit fileDropped(path);   // 交给主窗口加载
    }
    event->acceptProposedAction();
}

int FrameViewWidget::bytesPerPixel(PixelFormat fmt)
{
    switch (fmt) {
    case PixelFormat::RGB888: return 3;
    case PixelFormat::RGB565: return 2;
    case PixelFormat::Gray8:  return 1;
    }
    return 0;
}

QString FrameViewWidget::formatName(PixelFormat fmt)
{
    switch (fmt) {
    case PixelFormat::RGB888: return QStringLiteral("RGB888");
    case PixelFormat::RGB565: return QStringLiteral("RGB565");
    case PixelFormat::Gray8:  return QStringLiteral("灰度8位");
    }
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

    const int need = width * height * bytesPerPixel(fmt);
    if (need <= 0 || len < need)
        return false;

    // 每帧自带参数：与当前设置不同则切换（不清空画面，避免闪烁）
    if (fmt != m_format || width != m_width || height != m_height) {
        m_format = fmt;
        m_width  = width;
        m_height = height;
    }

    // 零拷贝：QImage 直接浅包装外部缓冲（调用方保证其存活到下一帧，见类注释），
    // 省去大帧（如 2048x2048x3 = 12MB）的每帧深拷贝
    QImage::Format qfmt = QImage::Format_Invalid;
    switch (m_format) {
    case PixelFormat::RGB888: qfmt = QImage::Format_RGB888;      break;
    case PixelFormat::RGB565: qfmt = QImage::Format_RGB16;       break;  // RGB565
    case PixelFormat::Gray8:  qfmt = QImage::Format_Grayscale8;  break;
    }

    m_image = QImage(data, m_width, m_height, qfmt);
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
    const qint64 logNow = m_fpsTimer.elapsed();
    if (logNow - m_lastFpsLogMs >= 1000) {
        m_lastFpsLogMs = logNow;
        LOG_DEBUG("[显示] 实测 {:.1f} FPS（目标帧已显示 {} 帧）",
                  m_fps, m_frameStamps.size());
    }

    update();   // 触发重绘
    return true;
}

void FrameViewWidget::clear()
{
    m_image = QImage();
    m_scaled = QImage();
    m_frameStamps.clear();
    m_fps = 0.0;
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
    if (target == m_image.size())
        m_scaled = m_image;   // 浅拷贝，共享数据，无额外内存
    else
        // FastTransformation：60fps 下质量/速度平衡；
        // 大帧只在此处重采样一次，paint 直接画缓存
        m_scaled = m_image.scaled(target, Qt::KeepAspectRatio,
                                  Qt::FastTransformation);
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
    p.fillRect(rect(), Qt::black);

    // ---- 绘制图像：直接画缩放缓存并居中（缩放已在 showFrame/resize 时完成） ----
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
