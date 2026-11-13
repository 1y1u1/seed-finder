QT += core
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle

TARGET = diag4
TEMPLATE = app

QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_RELEASE += -O2

INCLUDEPATH += .. \
               ../../seed-finder-qt-main1.2

SOURCES += diag4.cpp \
           ../seedengine.cpp

HEADERS += ../seedengine.h \
           ../zombiedata.h \
           ../../seed-finder-qt-main1.2/worker.h
