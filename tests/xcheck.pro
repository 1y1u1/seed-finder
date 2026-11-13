QT += core
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle

TARGET = xcheck
TEMPLATE = app

QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_RELEASE += -O2

INCLUDEPATH += ../src/core \
               ref12

SOURCES += xcheck.cpp \
           ../src/core/seedengine.cpp \
           ../src/core/gpuhook.cpp

HEADERS += ../src/core/seedengine.h \
           ../src/core/gpuhook.h \
           ../src/core/zombiedata.h \
           ref12/worker.h
