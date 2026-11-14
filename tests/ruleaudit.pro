QT += core
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle

TEMPLATE = app
TARGET = ruleaudit

QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_RELEASE += -O2

INCLUDEPATH += ../src/core
INCLUDEPATH += ref12

SOURCES += ruleaudit.cpp
SOURCES += ../src/core/seedengine.cpp
SOURCES += ../src/core/gpuhook.cpp

HEADERS += ../src/core/seedengine.h
HEADERS += ../src/core/gpuhook.h
HEADERS += ../src/core/zombiedata.h
HEADERS += ref12/worker.h
