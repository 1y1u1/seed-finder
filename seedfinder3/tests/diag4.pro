QT += core
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle

TARGET = diag4
TEMPLATE = app

QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_RELEASE += -O2

INCLUDEPATH += .. \
               ref12

SOURCES += diag4.cpp \
           ../seedengine.cpp

HEADERS += ../seedengine.h \
           ../zombiedata.h \
           ref12/worker.h
