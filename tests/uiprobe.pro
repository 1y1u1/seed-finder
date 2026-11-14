QT += core gui widgets
QT -= app_bundle
CONFIG += console c++11
CONFIG -= app_bundle
TEMPLATE = app
TARGET = uiprobe

QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_RELEASE += -O2

INCLUDEPATH += ../src ../src/core ../src/app ../src/ui

SOURCES += uiprobe.cpp \
    ../src/core/seedengine.cpp \
    ../src/core/gpuhook.cpp \
    ../src/app/presetstore.cpp \
    ../src/app/searchworker.cpp \
    ../src/ui/mainwindow.cpp \
    ../src/ui/banddialog.cpp \
    ../src/ui/conddialog.cpp \
    ../src/ui/detaildialog.cpp \
    ../src/ui/detailview.cpp \
    ../src/ui/helppages.cpp \
    ../src/ui/style.cpp

HEADERS += \
    ../src/core/seedengine.h \
    ../src/core/gpuhook.h \
    ../src/core/zombiedata.h \
    ../src/app/presetstore.h \
    ../src/app/searchworker.h \
    ../src/ui/mainwindow.h \
    ../src/ui/banddialog.h \
    ../src/ui/conddialog.h \
    ../src/ui/detaildialog.h \
    ../src/ui/detailview.h \
    ../src/ui/helppages.h \
    ../src/ui/style.h

RESOURCES += ../resources/res.qrc
