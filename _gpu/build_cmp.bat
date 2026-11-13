@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.5\bin;C:\Users\012\Desktop\seed-finder-qt-main\_env\Qt\5.12.9\msvc2017_64\bin;%PATH%
cd /d C:\Users\012\Desktop\seed-finder-qt-main\_gpu
nvcc -Xcompiler "/MD" -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH -arch=sm_120 -O3 -c gpucuda.cu -o gpucuda.obj
if errorlevel 1 goto :eof
qmake gpucmp.pro -spec win32-msvc "CONFIG+=release"
nmake /NOLOGO release