QT += core
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle

TARGET = engine_test
TEMPLATE = app

QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_RELEASE += -O2

INCLUDEPATH += ../src/core

SOURCES += engine_test.cpp \
           ../src/core/seedengine.cpp \
           ../src/core/gpuhook.cpp

HEADERS += ../src/core/seedengine.h \
           ../src/core/gpuhook.h \
           ../src/core/zombiedata.h
