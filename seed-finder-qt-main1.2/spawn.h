#ifndef SPAWN_H
#define SPAWN_H

#include <QDialog>
#include <vector>

namespace Ui {
class Spawn;
}

// 出怪设置: 四个独立条件, 每个条件单独勾选出怪
//   ① 必出僵尸 (勾选的全部出现)
//   ② 禁出僵尸 (勾选的全部不出现)
//   ③ 至少出 n 个 (勾选的僵尸中至少出现 n 种)
//   ④ 至多出 n 个 (勾选的僵尸中至多出现 n 种)
class Spawn : public QDialog
{
    Q_OBJECT

public:
    explicit Spawn(QWidget *parent = nullptr);
    ~Spawn();
    void show_setting(int scene);

    std::vector<bool> _need_flags;      // 必出 (1..GIGA)
    std::vector<bool> _refuse_flags;    // 禁出 (1..GIGA)
    std::vector<bool> _atleast_flags;   // 至少出 (1..GIGA)
    std::vector<bool> _atmost_flags;    // 至多出 (1..GIGA)
    int _atleast = -1;                  // 至少出几个; -1 = 勾选数(全部出现)
    int _atmost = 0;                    // 至多出几个; 0 = 全部不出现

private slots:
    void on_submit_clicked();
    void show_warning(const char *title, const char *msg);

private:
    Ui::Spawn *ui;
    int _scene = -1;                    // 当前场景 (用于必出限制校验)
};

#endif // SPAWN_H
