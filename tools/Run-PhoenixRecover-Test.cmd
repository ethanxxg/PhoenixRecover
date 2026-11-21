@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-PhoenixRecover-Test.ps1" %*
if errorlevel 1 (
    echo.
    echo 测试模式启动失败。
    pause
    exit /b 1
)
endlocal
