@echo off
echo Installing I2C Controller Emulator...

REM Use devcon.exe directly (XP x64)
devcon.exe install i2cctrl_emu.inf ROOT\I2CCTRL_EMU

IF %ERRORLEVEL% NEQ 0 (
    echo Installation failed.
    exit /b 1
)

echo Installation completed successfully.
exit /b 0
