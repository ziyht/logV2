TEMPLATE = app
CONFIG += console c++11
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += main.c \
    log.c \
    logtest.c

HEADERS += \
    log.h \
    logtest.h

LIBS += \
    -lpthread
