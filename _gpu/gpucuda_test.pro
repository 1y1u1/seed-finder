QT += core gui
CONFIG += console c++11
CONFIG -= app_bundle

TARGET = gpucuda_test
TEMPLATE = app

INCLUDEPATH += ../src/core .
QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_RELEASE += /O2

SOURCES += gpucuda_test.cpp \
           ../src/core/seedengine.cpp

HEADERS += ../src/core/seedengine.h \
           ../src/core/zombiedata.h \
           gpucuda.h

CUDA_LIB = C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5/lib/x64
LIBS += -L"$$CUDA_LIB" -lcudart
LIBS += gpucuda.obj
