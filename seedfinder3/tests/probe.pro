QT += core
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle

TARGET = probe
TEMPLATE = app

QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_RELEASE += -O2

INCLUDEPATH += ../src/core

SOURCES += probe.cpp \
           ../src/core/seedengine.cpp

HEADERS += ../src/core/seedengine.h \
           ../src/core/zombiedata.h
