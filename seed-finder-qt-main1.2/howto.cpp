#include "howto.h"
#include "ui_howto.h"

HowTo::HowTo(QWidget *parent) :
    QDialog(parent, Qt::WindowCloseButtonHint | Qt::WindowTitleHint),
    ui(new Ui::HowTo)
{
    ui->setupUi(this);
}

HowTo::~HowTo()
{
    delete ui;
}
