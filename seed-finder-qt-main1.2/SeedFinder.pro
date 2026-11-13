QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    about.cpp \
    howto.cpp \
    main.cpp \
    seedfinder.cpp \
    setting.cpp \
    spawn.cpp \
    table.cpp \
    worker.cpp

HEADERS += \
    about.h \
    howto.h \
    seedfinder.h \
    setting.h \
    spawn.h \
    table.h \
    worker.h

FORMS += \
    about.ui \
    howto.ui \
    seedfinder.ui \
    setting.ui \
    spawn.ui \
    table.ui

RC_ICONS = icon.ico

# 静态链接 Qt 时, 需要把用到的插件直接链接进可执行文件
static {
    QTPLUGIN += qwindows
    QTPLUGIN += qwindowsvistastyle
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
