@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=C:\Users\012\Desktop\seed-finder-qt-main\_env\Qt\5.12.9\msvc2017_64\bin;%PATH%
cd /d "%~dp0"
qmake gpupipe.pro -spec win32-msvc "CONFIG+=release"
nmake /NOLOGO release