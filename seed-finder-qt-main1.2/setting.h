#ifndef SETTING_H
#define SETTING_H

#include <QDialog>

namespace Ui {
class Setting;
}

// 详细设置: 只负责种子遍历范围/起点/输出数量
class Setting : public QDialog
{
    Q_OBJECT

public:
    explicit Setting(QWidget *parent = nullptr);
    ~Setting();
    void show_setting();

    // config
    int _output_size = 10;
    bool _start_at_random_seed = true;
    uint32_t _seed_start = 0;
    uint64_t _seed_span = 1000000;

private slots:
    void on_submit_clicked();
    void show_warning(const char *title, const char *msg);

private:
    Ui::Setting *ui;
};

#endif // SETTING_H
