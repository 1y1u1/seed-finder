QT += core gui
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = SeedFinder3
TEMPLATE = app

CONFIG += c++11
DEFINES += QT_DEPRECATED_WARNINGS

QMAKE_CXXFLAGS_RELEASE += /O2
QMAKE_CXXFLAGS += /utf-8

SOURCES += \
    main.cpp \
    seedengine.cpp \
    style.cpp \
    helppages.cpp \
    presetstore.cpp \
    conddialog.cpp \
    banddialog.cpp \
    detailview.cpp \
    detaildialog.cpp \
    searchworker.cpp \
    mainwindow.cpp

HEADERS += \
    zombiedata.h \
    seedengine.h \
    style.h \
    helppages.h \
    presetstore.h \
    conddialog.h \
    banddialog.h \
    detailview.h \
    detaildialog.h \
    searchworker.h \
    mainwindow.h

RC_ICONS = ../seed-finder-qt-main1.2/icon.ico

RESOURCES += res.qrc

# 静态链接 Qt 时, 需要把用到的插件直接链接进可执行文件
static {
    QTPLUGIN += qwindows
    QTPLUGIN += qwindowsvistastyle
}
