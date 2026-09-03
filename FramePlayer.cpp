#include "FramePlayer.h"
#include "Logger.h"

#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

FramePlayer::FramePlayer(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    // 设置窗口图标
    setWindowIcon(QIcon(QStringLiteral(":/FramePlayer/app.ico")));

    // 整个窗口接受文件拖放（图像显示控件不再自行处理拖放，事件上浮到这里统一处理）
    setAcceptDrops(true);

    // ---- 显式 connect()，不使用按名字自动连接 ----
    connect(ui.btnOpen, &QPushButton::clicked,
            this, &FramePlayer::onOpenFile);
    connect(ui.btnStart, &QPushButton::clicked,
            this, &FramePlayer::onStart);
    connect(ui.btnPauseResume, &QPushButton::clicked,
            this, &FramePlayer::onPauseResume);
    connect(ui.btnStop, &QPushButton::clicked,
            this, &FramePlayer::onStop);
    connect(ui.sliderFrame, &QSlider::sliderMoved,
            this, &FramePlayer::onSliderMoved);
    connect(&m_playTimer, &QTimer::timeout,
            this, &FramePlayer::onTimerTick);
    // 纯C++线程模式：定时从帧队列取帧显示（10ms 轮询，远高于帧率不积压）
    connect(&m_cppPollTimer, &QTimer::timeout,
            this, &FramePlayer::onCppPollTick);

    // 数据源切换：5 个 RadioButton 统一交给互斥组，由 buttonToggled 驱动。
    // 只在 checked == true（新选中项）时刷新，
    // 避免「旧项取消选中」再重复触发一次序列重建
    m_sourceGroup = new QButtonGroup(this);
    m_sourceGroup->addButton(ui.radioFile);
    m_sourceGroup->addButton(ui.radioFileThread);
    m_sourceGroup->addButton(ui.radioFileCpp);
    m_sourceGroup->addButton(ui.radioSim);
    m_sourceGroup->addButton(ui.radioSimCpp);
    // 注意：buttonToggled 有两个重载（QAbstractButton* / int），
    // 必须用 QOverload 指定版本，否则模板参数无法推导、直接编译报错
    connect(m_sourceGroup,
            QOverload<QAbstractButton *, bool>::of(&QButtonGroup::buttonToggled),
            this, [this](QAbstractButton *, bool checked) {
        if (checked)
            onSourceModeChanged();
    });

    // 图像参数 / 模拟帧数变化统一走 onParamChanged
    connect(ui.comboFormat, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FramePlayer::onParamChanged);
    connect(ui.spinWidth, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &FramePlayer::onParamChanged);
    connect(ui.spinHeight, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &FramePlayer::onParamChanged);
    connect(ui.spinSimFrames, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &FramePlayer::onParamChanged);

    // 子线程读帧器：常驻对象，随窗口销毁；帧数据跨线程队列连接回 GUI
    m_readThread = new FrameReadThread(this);
    connect(m_readThread, &FrameReadThread::frameReady,
            this, &FramePlayer::onThreadFrame);
    connect(m_readThread, &FrameReadThread::openFailed,
            this, &FramePlayer::onThreadOpenFailed);

    // 帧率变化只调整定时节奏，不打断播放（两种驱动方式都同步）
    connect(ui.spinFps, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int fps) {
        m_playTimer.setInterval(1000 / fps);
        m_readThread->setFps(fps);
        m_cppSource.setFps(fps);
    });

    // 初始化：显示控件格式 + 数据源控件状态
    ui.frameView->setFormat(currentFormat(),
                            ui.spinWidth->value(), ui.spinHeight->value());
    onSourceModeChanged();
    LOG_INFO("主窗口初始化完成（默认格式 {} {}x{}）",
             FrameViewWidget::formatName(currentFormat()).toStdString(),
             ui.spinWidth->value(), ui.spinHeight->value());
}

FramePlayer::~FramePlayer()
{
    stopReadThread();   // 先停子线程再析构，避免信号发往已销毁对象
    stopCppSource();    // 纯C++线程同理（其析构也会自动 join，这里显式停更清晰）
}

// ---------------- 辅助 ----------------

int FramePlayer::bytesPerPixel() const
{
    switch (ui.comboFormat->currentIndex()) {
    case 0: return 3;   // RGB888
    case 1: return 2;   // RGB565
    case 2: return 1;   // 灰度8位
    }
    return 0;
}

PixelFormat FramePlayer::currentFormat() const
{
    switch (ui.comboFormat->currentIndex()) {
    case 0: return PixelFormat::RGB888;
    case 1: return PixelFormat::RGB565;
    case 2: return PixelFormat::Gray8;
    }
    return PixelFormat::RGB888;
}

int FramePlayer::frameSize() const
{
    return ui.spinWidth->value() * ui.spinHeight->value() * bytesPerPixel();
}

bool FramePlayer::isThreadMode() const
{
    return ui.radioFileThread->isChecked();
}

bool FramePlayer::isCppFileMode() const
{
    return ui.radioFileCpp->isChecked();
}

bool FramePlayer::isCppMode() const
{
    return ui.radioFileCpp->isChecked() || ui.radioSimCpp->isChecked();
}

bool FramePlayer::isFileLikeMode() const
{
    return ui.radioFile->isChecked() || isThreadMode() || isCppFileMode();
}

bool FramePlayer::isSimLikeMode() const
{
    return ui.radioSim->isChecked() || ui.radioSimCpp->isChecked();
}

void FramePlayer::stopReadThread()
{
    // 仅置标志位后等待退出；线程未运行时 wait() 立即返回
    if (m_readThread && m_readThread->isRunning()) {
        m_readThread->stopThread();
        m_readThread->wait();
    }
}

void FramePlayer::stopCppSource()
{
    // stop() 幂等：置标志位并 join；线程未运行时立即返回
    m_cppPollTimer.stop();
    m_cppSource.stop();
    // 线程停下后归还持有的帧槽，避免缓冲池耗尽
    if (m_cppHeldFrame) {
        m_cppSource.releaseFrame(m_cppHeldFrame);
        m_cppHeldFrame = nullptr;
    }
}

// ---------------- 文件加载 / 拖放 ----------------

void FramePlayer::onOpenFile()
{
    // 文件格式不限
    const QString path = QFileDialog::getOpenFileName(
                this, QStringLiteral("打开二进制图像文件"),
                QString(),
                QStringLiteral("所有文件 (*)"));
    if (path.isEmpty())
        return;
    loadFile(path);
}

void FramePlayer::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void FramePlayer::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty()) {
        const QString path = urls.first().toLocalFile();
        if (!path.isEmpty())
            loadFile(path);
    }
}

void FramePlayer::loadFile(const QString &path)
{
    // 关闭之前打开的文件
    if (m_file.isOpen())
        m_file.close();

    // 只打开句柄，不读内容；之后按帧 seek 读取，支持超大文件
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法打开文件：%1").arg(path));
        LOG_WARN("无法打开文件: {}", path.toStdString());
        return;
    }

    ui.labelFilePath->setText(QFileInfo(path).fileName()
                              + QStringLiteral("（%1 字节）").arg(m_file.size()));
    statusBar()->showMessage(QStringLiteral("已加载 %1").arg(path));
    LOG_INFO("加载文件: {}（{} 字节）", path.toStdString(), m_file.size());

    // 切到文件模式并重建帧序列（保留当前文件子模式：主线程 / 子线程）
    // （setChecked 会通过 toggled 信号触发 onSourceModeChanged）
    if (ui.radioFile->isChecked() || ui.radioFileThread->isChecked()
            || isCppFileMode())
        onSourceModeChanged();
    else
        ui.radioFile->setChecked(true);
}

// ---------------- 数据源切换 / 参数变化 ----------------

void FramePlayer::onSourceModeChanged()
{
    // 切换数据源时停止播放（定时器与两种子线程驱动都要停）
    m_playTimer.stop();
    stopReadThread();
    stopCppSource();
    m_state = Stopped;
    m_lastLoggedFrame = 0;

    // 三种文件模式共用「打开文件」入口与总帧数计算
    const bool fileMode = isFileLikeMode();
    ui.btnOpen->setEnabled(fileMode);
    ui.labelFilePath->setEnabled(fileMode);
    ui.labelSimFrames->setEnabled(!fileMode);
    ui.spinSimFrames->setEnabled(!fileMode);

    // 按数据源计算总帧数
    if (fileMode)
        m_totalFrames = (m_file.isOpen() && frameSize() > 0)
                ? static_cast<int>(m_file.size() / frameSize()) : 0;
    else
        m_totalFrames = ui.spinSimFrames->value();

    m_currentFrame = 0;
    ui.sliderFrame->setRange(0, qMax(0, m_totalFrames - 1));
    ui.sliderFrame->setValue(0);

    if (m_totalFrames > 0)
        showFrameAt(0);   // 预览第 0 帧
    else {
        ui.frameView->clear();
        updateFrameInfo();
        // 文件已加载但不足一帧时提示原因，便于排查格式/宽高设置
        if (fileMode && m_file.isOpen())
            statusBar()->showMessage(
                QStringLiteral("文件 %1 字节，不足一帧（%2 字节），请检查格式与宽高设置")
                    .arg(m_file.size()).arg(frameSize()));
    }
    updateControlStates();

    const char *modeName = ui.radioSim->isChecked()    ? "模拟数据"
                         : ui.radioSimCpp->isChecked() ? "模拟数据(纯C++线程)"
                         : isThreadMode()              ? "本地文件(QThread子线程)"
                         : isCppFileMode()             ? "本地文件(纯C++线程)"
                                                       : "本地文件";
    LOG_INFO("数据源: {} | 格式 {} {}x{} | 帧大小 {}B | 总帧数 {}",
             modeName,
             FrameViewWidget::formatName(currentFormat()).toStdString(),
             ui.spinWidth->value(), ui.spinHeight->value(),
             frameSize(), m_totalFrames);
}

void FramePlayer::onParamChanged()
{
    // 格式/宽高/模拟帧数变化会使已切分的帧失效：先停播，再按新参数重建
    m_playTimer.stop();
    stopReadThread();
    stopCppSource();
    m_state = Stopped;

    ui.frameView->setFormat(currentFormat(),
                            ui.spinWidth->value(), ui.spinHeight->value());

    if (isFileLikeMode())
        m_totalFrames = (m_file.isOpen() && frameSize() > 0)
                ? static_cast<int>(m_file.size() / frameSize()) : 0;
    else
        m_totalFrames = ui.spinSimFrames->value();

    m_currentFrame = 0;
    m_lastLoggedFrame = 0;
    ui.sliderFrame->setRange(0, qMax(0, m_totalFrames - 1));
    ui.sliderFrame->setValue(0);

    if (m_totalFrames > 0)
        showFrameAt(0);
    else {
        ui.frameView->clear();
        updateFrameInfo();
    }
    updateControlStates();
    LOG_DEBUG("参数变化: {} {}x{} | 总帧数 {}",
              FrameViewWidget::formatName(currentFormat()).toStdString(),
              ui.spinWidth->value(), ui.spinHeight->value(), m_totalFrames);
}

// ---------------- 播放控制 ----------------

void FramePlayer::onStart()
{
    if (m_totalFrames <= 0 || m_state != Stopped)
        return;

    // 从第 0 帧开始播放
    m_currentFrame = 0;
    ui.sliderFrame->setValue(0);
    showFrameAt(0);

    m_state = Playing;
    if (isThreadMode()) {
        // QThread 子线程驱动：传参后启动，定时由线程内 while+msleep 自持
        m_readThread->setParams(m_file.fileName(), frameSize(),
                                m_totalFrames, ui.spinFps->value());
        m_readThread->start();
    } else if (isCppMode()) {
        // 纯C++线程驱动：std::thread 产帧入队，主线程 QTimer 轮询取帧
        const CppFrameSource::Mode mode = isCppFileMode()
                ? CppFrameSource::Mode::LocalFile
                : CppFrameSource::Mode::Simulate;
        CppFrameSource::Format fmt = CppFrameSource::Format::RGB888;
        switch (currentFormat()) {
        case PixelFormat::RGB888: fmt = CppFrameSource::Format::RGB888; break;
        case PixelFormat::RGB565: fmt = CppFrameSource::Format::RGB565; break;
        case PixelFormat::Gray8:  fmt = CppFrameSource::Format::Gray8;  break;
        }
        m_cppSource.configure(mode,
                              isCppFileMode() ? m_file.fileName().toStdWString()
                                              : std::wstring(),
                              frameSize(), m_totalFrames, ui.spinFps->value(),
                              fmt, ui.spinWidth->value(), ui.spinHeight->value());
        m_cppSource.start();
        m_cppPollTimer.start(10);
    } else {
        m_playTimer.start(1000 / ui.spinFps->value());
    }
    const char *drive = isThreadMode() ? "QThread子线程"
                      : isCppMode()    ? "纯C++线程"
                                       : "主线程";
    LOG_INFO("开始播放: 模式={} | {} FPS | 总帧数 {}",
             drive, ui.spinFps->value(), m_totalFrames);
    updateControlStates();
}

void FramePlayer::onPauseResume()
{
    if (m_state == Playing) {
        if (isThreadMode())
            m_readThread->setPaused(true);   // 线程内小睡空转，保持现场
        else if (isCppMode())
            m_cppSource.setPaused(true);
        else
            m_playTimer.stop();
        m_state = Paused;
        LOG_INFO("暂停（第 {}/{} 帧）", m_currentFrame + 1, m_totalFrames);
    } else if (m_state == Paused) {
        m_state = Playing;
        if (isThreadMode())
            m_readThread->setPaused(false);
        else if (isCppMode())
            m_cppSource.setPaused(false);
        else
            m_playTimer.start(1000 / ui.spinFps->value());
        LOG_INFO("继续播放（第 {}/{} 帧）", m_currentFrame + 1, m_totalFrames);
    }
    updateControlStates();
}

void FramePlayer::onStop()
{
    m_playTimer.stop();
    stopReadThread();
    stopCppSource();
    LOG_INFO("停止播放，回到第 0 帧");
    m_state = Stopped;

    // 回到第 0 帧
    m_currentFrame = 0;
    ui.sliderFrame->setValue(0);
    if (m_totalFrames > 0)
        showFrameAt(0);
    else
        ui.frameView->clear();
    updateControlStates();
}

void FramePlayer::onTimerTick()
{
    if (m_totalFrames <= 0)
        return;

    // 到末尾自动回卷循环播放
    m_currentFrame = (m_currentFrame + 1) % m_totalFrames;
    showFrameAt(m_currentFrame);
    ui.sliderFrame->setValue(m_currentFrame);

    // 每 100 帧记一条计数日志（回卷时也记），控制台可实时观察进度
    if (m_currentFrame < m_lastLoggedFrame
            || m_currentFrame - m_lastLoggedFrame >= 100) {
        LOG_INFO("播放中: 已到第 {}/{} 帧", m_currentFrame + 1, m_totalFrames);
        m_lastLoggedFrame = m_currentFrame;
    }
}

void FramePlayer::onSliderMoved(int value)
{
    if (m_totalFrames <= 0)
        return;

    m_currentFrame = value;
    LOG_DEBUG("拖动进度条跳转: 第 {} 帧", value + 1);
    if (isThreadMode() && m_state == Playing && m_readThread->isRunning())
        // 播放中：通知子线程跳转，下一帧由线程从新位置读出
        m_readThread->seekTo(value);
    else if (isCppMode() && m_state == Playing && m_cppSource.isRunning())
        // 纯C++线程播放中：通知跳转，下一帧由线程从新位置产出
        m_cppSource.seekTo(value);
    else
        // 暂停/停止/其他模式：主线程直接读帧，立即反馈
        showFrameAt(m_currentFrame);
}

// ---------------- 纯C++线程帧轮询 ----------------

void FramePlayer::onCppPollTick()
{
    // 停止后迟到/残留的帧直接丢弃；暂停时也不取（队列有上限自动丢旧留新）
    if (m_state != Playing || !isCppMode())
        return;

    // 子线程打开文件失败：停播并提示（只提示一次）
    if (m_cppSource.openFailed()) {
        onStop();
        const QString path = QString::fromStdWString(m_cppSource.filePath());
        LOG_ERROR("纯C++线程打开文件失败: {}", path.toStdString());
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("子线程无法打开文件：%1").arg(path));
        return;
    }

    // 取最新一帧（acquireFrame 内部排空只留最新）；
    // 零拷贝：归还上一个槽再持有新槽，图像直接包装槽内内存
    CppFrameSource::Frame *f = m_cppSource.acquireFrame();
    if (!f)
        return;
    if (m_cppHeldFrame)
        m_cppSource.releaseFrame(m_cppHeldFrame);
    m_cppHeldFrame = f;

    m_currentFrame = f->index;
    ui.frameView->showFrame(f->data.data(), static_cast<int>(f->data.size()),
                            ui.spinWidth->value(), ui.spinHeight->value(),
                            currentFormat());
    ui.sliderFrame->setValue(f->index);
    updateFrameInfo();
}

// ---------------- 子线程帧回调 ----------------

void FramePlayer::onThreadFrame(const QByteArray &data, int index)
{
    // 事件已出队：立刻归还一个在途名额（无论本帧最终是否使用），
    // 否则停止/切源路径下未消费的帧会把背压计数卡死
    m_readThread->frameConsumed();

    // 停止后迟到的帧（队列中未派发）直接丢弃
    if (m_state == Stopped || !isThreadMode())
        return;

    m_currentFrame = index;
    // 隐式共享赋值（零拷贝）持有帧数据，保证图像包装内存存活到下一帧
    m_threadFrameBuf = data;
    ui.frameView->showFrame(
                reinterpret_cast<const uchar *>(m_threadFrameBuf.constData()),
                m_threadFrameBuf.size(),
                ui.spinWidth->value(), ui.spinHeight->value(), currentFormat());
    ui.sliderFrame->setValue(index);
    updateFrameInfo();
}

void FramePlayer::onThreadOpenFailed(const QString &path)
{
    // 子线程此时已自行退出 run()，onStop 里的 wait() 会立即返回
    onStop();
    LOG_ERROR("子线程打开文件失败: {}", path.toStdString());
    QMessageBox::warning(this, QStringLiteral("错误"),
                         QStringLiteral("子线程无法打开文件：%1").arg(path));
}

// ---------------- 帧获取 ----------------

void FramePlayer::showFrameAt(int index)
{
    const int fs = frameSize();
    if (fs <= 0 || index < 0 || index >= m_totalFrames)
        return;

    if (isSimLikeMode()) {
        // 模拟数据：按需生成该帧
        makeSimFrame(index);
        ui.frameView->showFrame(
                    reinterpret_cast<const uchar *>(m_simBuffer.constData()), fs,
                    ui.spinWidth->value(), ui.spinHeight->value(), currentFormat());
    } else {
        // 本地文件：按需 seek 读取该帧，内存占用恒为单帧大小
        if (!m_file.isOpen())
            return;
        const qint64 offset = static_cast<qint64>(index) * fs;
        if (!m_file.seek(offset))
            return;
        m_frameBuffer.resize(fs);
        const qint64 n = m_file.read(m_frameBuffer.data(), fs);
        if (n < fs)
            return;   // 读到的数据不足一帧（如文件被外部截断）
        ui.frameView->showFrame(
                    reinterpret_cast<const uchar *>(m_frameBuffer.constData()), fs,
                    ui.spinWidth->value(), ui.spinHeight->value(), currentFormat());
    }
    updateFrameInfo();
}

void FramePlayer::makeSimFrame(int index)
{
    const int w = ui.spinWidth->value();
    const int h = ui.spinHeight->value();
    const int fs = frameSize();

    m_simBuffer.resize(fs);
    uchar *p = reinterpret_cast<uchar *>(m_simBuffer.data());

    // 移动渐变图案：随帧号水平滚动，肉眼可确认播放状态
    switch (currentFormat()) {
    case PixelFormat::RGB888:
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                *p++ = static_cast<uchar>((x + index * 4) & 0xFF);
                *p++ = static_cast<uchar>(y & 0xFF);
                *p++ = static_cast<uchar>((x + y + index * 2) & 0xFF);
            }
        break;
    case PixelFormat::RGB565:
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int r = (x + index * 4) & 0xFF;
                const int g = y & 0xFF;
                const int b = (x + y + index * 2) & 0xFF;
                const quint16 v = static_cast<quint16>(
                            ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                *p++ = static_cast<uchar>(v & 0xFF);   // 低字节在前
                *p++ = static_cast<uchar>(v >> 8);
            }
        break;
    case PixelFormat::Gray8:
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                *p++ = static_cast<uchar>((x + y + index * 3) & 0xFF);
        break;
    }
}

// ---------------- 界面状态 ----------------

void FramePlayer::updateFrameInfo()
{
    ui.labelFrameInfo->setText(QStringLiteral("%1 / %2")
                               .arg(m_totalFrames > 0 ? m_currentFrame + 1 : 0)
                               .arg(m_totalFrames));
}

void FramePlayer::updateControlStates()
{
    const bool hasFrames = m_totalFrames > 0;

    ui.btnStart->setEnabled(hasFrames && m_state == Stopped);
    ui.btnPauseResume->setEnabled(m_state != Stopped);
    ui.btnStop->setEnabled(m_state != Stopped);
    ui.sliderFrame->setEnabled(hasFrames);

    // 暂停/继续按钮文字随状态切换
    ui.btnPauseResume->setText(m_state == Playing
                               ? QStringLiteral("暂停")
                               : QStringLiteral("继续"));
}
