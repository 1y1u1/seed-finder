QT += core
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle
TEMPLATE = app
TARGET = bench
QMAKE_CXXFLAGS_RELEASE += /O2
QMAKE_CXXFLAGS += /utf-8

SOURCES += bench.cpp \
           ../seedfinder3/seedengine.cpp
HEADERS += ../seedfinder3/seedengine.h \
           ../seedfinder3/zombiedata.h
