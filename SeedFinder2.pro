QT += core gui
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = SeedFinder2
TEMPLATE = app

CONFIG += c++11
DEFINES += QT_DEPRECATED_WARNINGS

QMAKE_CXXFLAGS_RELEASE += /O2
QMAKE_CXXFLAGS += /utf-8

# 各层都进搜索路径, 源码里统一写 #include "seedengine.h" 这种裸文件名
INCLUDEPATH += \
    src \
    src/core \
    src/app \
    src/ui

SOURCES += \
    src/main.cpp \
    src/core/seedengine.cpp \
    src/core/gpuhook.cpp \
    src/app/searchworker.cpp \
    src/app/presetstore.cpp \
    src/ui/style.cpp \
    src/ui/helppages.cpp \
    src/ui/conddialog.cpp \
    src/ui/banddialog.cpp \
    src/ui/detailview.cpp \
    src/ui/detaildialog.cpp \
    src/ui/mainwindow.cpp

HEADERS += \
    src/core/zombiedata.h \
    src/core/seedengine.h \
    src/core/gpuhook.h \
    src/app/searchworker.h \
    src/app/presetstore.h \
    src/ui/style.h \
    src/ui/helppages.h \
    src/ui/conddialog.h \
    src/ui/banddialog.h \
    src/ui/detailview.h \
    src/ui/detaildialog.h \
    src/ui/mainwindow.h

RC_ICONS = resources/icon.ico

RESOURCES += resources/res.qrc

# ---- 可选的 CUDA 后端 ----
# _gpu/ 的 CUDA 源码是入库的, 但 gpucuda.obj 这类编译产物不入库,
# 所以这里仍然用 exists() 探测:
#   有 gpucuda.obj  -> 定义 SF_WITH_CUDA, 链接 CUDA 后端, 程序启动时自动启用 GPU
#   没有            -> gpuhook.cpp 提供返回 nullptr 的兜底实现, 程序纯 CPU 运行
# gpucuda.obj 由 _gpu\build_cuda_test.bat (或 build.ps1) 用 nvcc 生成, 不能直接写 .cu。
SF_GPU_DIR = $$PWD/_gpu
exists($$SF_GPU_DIR/gpucuda.obj) {
    message("SeedFinder: CUDA backend enabled")
    DEFINES += SF_WITH_CUDA
    INCLUDEPATH += $$SF_GPU_DIR
    SOURCES += $$SF_GPU_DIR/gpucuda_backend.cpp
    HEADERS += $$SF_GPU_DIR/gpucuda.h
    LIBS += -L"C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5/lib/x64"
    # 静态版不能依赖 cudart 的 dll, 否则"静态"承诺就断了
    # CUDA 13 已经不带 culibos.lib 了, 静态链接只需要 cudart_static。
    # 注意 .obj 的 CRT 必须和本工程一致: 静态版是 /MT, 动态版是 /MD,
    # 混用会报一堆 __imp_xxx 未解析 (cudart_static 里的 CRT 符号对不上)。
    static {
        LIBS += -lcudart_static
        LIBS += $$SF_GPU_DIR/gpucuda_static.obj
    } else {
        LIBS += -lcudart
        LIBS += $$SF_GPU_DIR/gpucuda.obj
    }
} else {
    message("SeedFinder: CUDA backend not found, CPU only")
}

# 静态链接 Qt 时, 需要把用到的插件直接链接进可执行文件
static {
    QTPLUGIN += qwindows
    QTPLUGIN += qwindowsvistastyle
}
