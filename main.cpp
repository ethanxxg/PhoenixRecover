#include "crash/CrashHandler.h"
#include "PhoenixRecover.h"
#include "log/Logger.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const std::wstring logPath = Logger::Instance().OpenAutoLogFile();
    Logger::Instance().SetConsoleLogLevel(LogLevel::Info);
    Logger::Instance().SetFileLogLevel(LogLevel::Debug);

    CrashHandler::Install(logPath);

    LOG_INFO(L"PhoenixRecover started");
    LOG_INFO(L"Log file: " + logPath);
    PhoenixRecover window;
    window.show();
    return app.exec();
}
