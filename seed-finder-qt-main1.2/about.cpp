#include "about.h"
#include "ui_about.h"

About::About(QWidget *parent) :
    QDialog(parent, Qt::WindowCloseButtonHint | Qt::WindowTitleHint),
    ui(new Ui::About)
{
    ui->setupUi(this);
    ui->col2->setTextInteractionFlags(Qt::TextBrowserInteraction);
    ui->col2->setOpenExternalLinks(true);
}

About::~About()
{
    delete ui;
}
