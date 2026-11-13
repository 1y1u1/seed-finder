#include "seedfinder.h"

#include <QApplication>
#include <QTextCodec>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    SeedFinder w;
    w.show();
    return a.exec();
}
