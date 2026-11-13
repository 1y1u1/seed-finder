QT += core
CONFIG += console c++11
CONFIG -= app_bundle

TARGET = gpucmp
TEMPLATE = app

INCLUDEPATH += ../src/core .

SOURCES += \
    gpucmp.cpp \
    gpucuda_backend.cpp \
    ../src/core/seedengine.cpp \
    ../src/core/gpuhook.cpp

HEADERS += \
    ../src/core/seedengine.h \
    ../src/core/zombiedata.h \
    ../src/core/gpuhook.h \
    gpucuda.h

DEFINES += SF_WITH_CUDA
QMAKE_CXXFLAGS += /utf-8

LIBS += -L"C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.5/lib/x64" -lcudart
LIBS += gpucuda.obj
