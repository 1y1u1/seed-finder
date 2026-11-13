QT += core gui widgets

CONFIG += c++11 console
CONFIG -= app_bundle

TARGET = gpuprobe
TEMPLATE = app

QMAKE_CXXFLAGS_RELEASE += /O2
QMAKE_CXXFLAGS += /utf-8

INCLUDEPATH += ../seedfinder3

SOURCES += gpuprobe.cpp

HEADERS += ../seedfinder3/seedengine.h \
    ../seedfinder3/zombiedata.h
