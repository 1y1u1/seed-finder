QT += core gui
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = SeedFinder3
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

# 静态链接 Qt 时, 需要把用到的插件直接链接进可执行文件
static {
    QTPLUGIN += qwindows
    QTPLUGIN += qwindowsvistastyle
}
