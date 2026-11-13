#ifndef TABLE_H
#define TABLE_H

#include <QWidget>
#include <array>

namespace Ui {
class Table;
}

const std::vector<const char *> zombie_names = {"普僵", "路障", "撑杆", "铁桶", "读报", "铁门", "橄榄", "舞王", "潜水", "冰车",
                                                "海豚", "小丑", "气球", "矿工", "跳跳", "蹦极", "扶梯", "投篮", "白眼", "红眼"
                                               };

class Table : public QWidget
{
    Q_OBJECT

public:
    explicit Table(QWidget *parent = nullptr);
    ~Table();

    void update_contents(std::vector<std::array<bool, 20>> data, int start_flags, int scene,
                         std::vector<bool> need_flags, std::vector<bool> refuse_flags,
                         std::vector<bool> atleast_flags, std::vector<bool> atmost_flags,
                         int n_atleast, int n_atmost, bool bring_to_front);

private:
    Ui::Table *ui = nullptr;
    class QTableWidget *table = nullptr;
    class QGridLayout *main_layout = nullptr;
    class QLabel *m_label = nullptr;
    void move_to_screen_center(int expected_width, int expected_height);

    // std::vector<int> get_show_list(int scene);
};

#endif // TABLE_H
