QT += core
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle

TARGET = probe2
TEMPLATE = app

QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_RELEASE += -O2

INCLUDEPATH += ..

SOURCES += probe2.cpp \
           ../seedengine.cpp

HEADERS += ../seedengine.h \
           ../zombiedata.h
