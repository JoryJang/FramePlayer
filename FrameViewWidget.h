#pragma once

#include <QWidget>
#include <QImage>
#include <QElapsedTimer>
#include <QQueue>

// 像素格式：对应二进制文件中每帧的存储格式
enum class PixelFormat
{
    RGB888,   // 每像素 3 字节，R-G-B 顺序
    RGB565,   // 每像素 2 字节，RGB565（即 RGB16，低字节在前）
    Gray8     // 每像素 1 字节，8 位灰度
};

// 二进制图像帧显示控件
// 功能：
//   1. 按指定格式/宽高解析原始二进制帧并显示（零拷贝：QImage 直接包装外部缓冲）
//   2. 图像等比缩放居中显示（黑底）；缩放结果按控件尺寸缓存，
//      仅在新帧到达或控件 resize 时重采样，paintEvent 只画缓存位图
//   3. 左上角叠加显示：图像宽高、像素格式、实测帧率（FPS）
// 零拷贝契约：
//   showFrame() 不复制帧数据，调用方须保证 data 指向的内存
//   存活到下一次 showFrame()/clear() 调用之前不被改写或释放
class FrameViewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FrameViewWidget(QWidget *parent = nullptr);

    // 设置像素格式与图像宽高（单位：像素）
    void setFormat(PixelFormat fmt, int width, int height);

    // 当前格式下每帧的字节数；参数无效时返回 0
    int frameBytes() const;

    // 送入一帧原始数据并刷新显示（零拷贝，见上方契约）；
    // 每帧自带宽高与格式，与上次不同则自动切换；
    // 数据长度不足一帧时返回 false
    bool showFrame(const uchar *data, int len, int width, int height,
                   PixelFormat fmt);


    // 清空画面与统计信息
    void clear();

    QSize sizeHint() const override;

    // 每像素字节数
    static int bytesPerPixel(PixelFormat fmt);
    // 格式显示名（用于叠加层与界面）
    static QString formatName(PixelFormat fmt);

signals:
    // 用户把文件拖放到显示区域时发出
    void fileDropped(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;   // 尺寸变化时重建缩放缓存
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void refreshScaled();                     // 按当前控件尺寸重建缩放缓存

    QImage      m_image;                      // 当前帧（浅包装外部数据，零拷贝，见类注释契约）
    QImage      m_scaled;                     // 缩放缓存（控件大小），paintEvent 直接绘制
    PixelFormat m_format = PixelFormat::RGB888;
    int         m_width  = 0;
    int         m_height = 0;

    QElapsedTimer m_fpsTimer;                 // 帧率统计计时器
    QQueue<qint64> m_frameStamps;             // 最近若干帧到达时间戳（ms）
    double      m_fps = 0.0;                  // 实测帧率
    qint64      m_lastFpsLogMs = 0;           // 上次帧率日志时间（每 1 秒记一条）
};
