@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Users\012\Desktop\seed-finder-qt-main\_env\Qt\5.12.9\msvc2017_64\bin;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.5\bin;%PATH%

cd /d "C:\Users\012\Desktop\seed-finder-qt-main\_gpu"

rem CUDA side is compiled to an obj separately: qmake does not call nvcc.
rem -Xcompiler /MD keeps the CRT in sync with the Qt build (nvcc defaults to /MT).
rem -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH is required, otherwise the
rem STL1002 assert in MSVC 14.52 yvals_core.h crashes cudafe++ (0xC0000005)
nvcc -Xcompiler "/MD" -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH -arch=sm_120 -O3 -c gpucuda.cu -o gpucuda.obj
if errorlevel 1 goto fail

qmake gpucuda_test.pro -spec win32-msvc "CONFIG+=release"
if errorlevel 1 goto fail

nmake /NOLOGO release
if errorlevel 1 goto fail

echo BUILD OK
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
