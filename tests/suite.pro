QT += core
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle
CONFIG += thread

TEMPLATE = app
TARGET = suite

QMAKE_CXXFLAGS_RELEASE += -O2
QMAKE_CXXFLAGS += /utf-8

INCLUDEPATH += \
    ../src/core \
    ../src/app

SOURCES += \
    suite.cpp \
    ../src/core/seedengine.cpp \
    ../src/core/gpuhook.cpp \
    ../src/app/presetstore.cpp

HEADERS += \
    ../src/core/seedengine.h \
    ../src/core/gpuhook.h \
    ../src/core/zombiedata.h \
    ../src/app/presetstore.h
