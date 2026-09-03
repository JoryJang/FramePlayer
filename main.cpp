#include "FramePlayer.h"
#include "Logger.h"
#include <QtWidgets/QApplication>
#include <spdlog/spdlog.h>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 启动控制台 + spdlog：控制台实时显示操作/计数，同时写 logs/FramePlayer.log
    AppLog::init();
    LOG_INFO("========== FramePlayer 启动 ==========");
    LOG_INFO("Qt {} | 程序: {}", qVersion(),
             QCoreApplication::applicationFilePath().toStdString());

    FramePlayer window;
    window.show();

    const int rc = app.exec();
    LOG_INFO("程序退出，返回码 {}", rc);
    spdlog::shutdown();   // 确保日志全部落盘
    return rc;
}
