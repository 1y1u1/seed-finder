#ifndef HOWTO_H
#define HOWTO_H

#include <QDialog>

namespace Ui {
class HowTo;
}

class HowTo : public QDialog
{
    Q_OBJECT

public:
    explicit HowTo(QWidget *parent = nullptr);
    ~HowTo();

private:
    Ui::HowTo *ui = nullptr;
};

#endif // HOWTO_H
