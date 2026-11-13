#include "spawn.h"
#include "ui_spawn.h"
#include "worker.h"
#include "table.h"
#include <QCheckBox>
#include <QRegExpValidator>
#include <QMessageBox>
#include <sstream>

static std::vector<bool> read_group(QWidget *w, const char *prefix)
{
    std::vector<bool> v;
    for (int i = 1; i <= 19; ++i) {
        v.push_back(w->findChild<QCheckBox *>(QString(prefix) + QString::number(i))->isChecked());
    }
    return v;
}

static void write_group(QWidget *w, const char *prefix, const std::vector<bool> &flags)
{
    for (int i = 1; i <= 19; ++i) {
        w->findChild<QCheckBox *>(QString(prefix) + QString::number(i))->setChecked(flags.at(i));
    }
}

Spawn::Spawn(QWidget *parent) :
    QDialog(parent, Qt::WindowCloseButtonHint | Qt::WindowTitleHint),
    ui(new Ui::Spawn)
{
    ui->setupUi(this);

    // 默认配置: 必出红眼 (与原版"难度关=红眼关"一致)
    for (int i = 0; i < GIGA; ++i) {
        _need_flags.push_back(false);
        _refuse_flags.push_back(false);
        _atleast_flags.push_back(false);
        _atmost_flags.push_back(false);
    }
    _need_flags.push_back(true);
    _refuse_flags.push_back(false);
    _atleast_flags.push_back(false);
    _atmost_flags.push_back(false);

    QRegExp rx_n("[0-1]?[0-9]");
    ui->atleast_val->setValidator(new QRegExpValidator(rx_n, this));
    ui->atmost_val->setValidator(new QRegExpValidator(rx_n, this));
}

Spawn::~Spawn()
{
    delete ui;
}

void Spawn::show_setting(int scene)
{
    _scene = scene;
    write_group(this, "needCheckBox_", _need_flags);
    write_group(this, "refuseCheckBox_", _refuse_flags);
    write_group(this, "atleastCheckBox_", _atleast_flags);
    write_group(this, "atmostCheckBox_", _atmost_flags);
    ui->atleast_val->setText(_atleast < 0 ? "" : QString::number(_atleast));
    ui->atmost_val->setText(_atmost > 0 ? QString::number(_atmost) : "");
}

void Spawn::show_warning(const char *title, const char *msg)
{
    QMessageBox Msgbox(this);
    Msgbox.setWindowTitle(QString::fromUtf8(title));
    Msgbox.setIcon(QMessageBox::Information);
    Msgbox.setText(QString::fromUtf8(msg));
    Msgbox.exec();
}

void Spawn::on_submit_clicked()
{
    std::vector<bool> need = read_group(this, "needCheckBox_");
    std::vector<bool> refuse = read_group(this, "refuseCheckBox_");
    std::vector<bool> atleast = read_group(this, "atleastCheckBox_");
    std::vector<bool> atmost = read_group(this, "atmostCheckBox_");

    bool any = false;
    int atleast_count = 0, atmost_count = 0;
    for (int i = 1; i <= 19; ++i) {
        if (atleast.at(i - 1)) ++atleast_count;
        if (atmost.at(i - 1)) ++atmost_count;
        if (need.at(i - 1) || refuse.at(i - 1) || atleast.at(i - 1) || atmost.at(i - 1)) any = true;
    }
    if (!any) {
        show_warning("输入错误", "未选择任何僵尸 (四个条件均为空)");
        return;
    }

    // 限制1: 四种条件中不能勾选相同的出怪
    for (int i = 1; i <= 19; ++i) {
        int groups = 0;
        if (need.at(i - 1)) ++groups;
        if (refuse.at(i - 1)) ++groups;
        if (atleast.at(i - 1)) ++groups;
        if (atmost.at(i - 1)) ++groups;
        if (groups > 1) {
            std::ostringstream oss;
            oss << "第" << i << "种僵尸(" << zombie_names.at(i) << ")在多个条件中重复勾选, 请只保留一个";
            show_warning("输入错误", oss.str().c_str());
            return;
        }
    }

    // 限制2: 按场景限制必出僵尸 (与游戏出怪规则一致)
    if (_scene >= 0) {
        int allow = scene_allow_mask(_scene);
        std::ostringstream bad;
        for (int i = 1; i <= 19; ++i) {
            if (need.at(i - 1) && !(allow & (1 << i))) {
                bad << zombie_names.at(i) << " ";
            }
        }
        if (!bad.str().empty()) {
            std::ostringstream oss;
            oss << "当前场景不能勾选以下必出僵尸: " << bad.str();
            show_warning("输入错误", oss.str().c_str());
            return;
        }
    }

    // 至少/至多: 该组未勾选任何僵尸时不要求填写, 视为 0 (条件自动满足)
    int atleast_val = 0;
    if (atleast_count > 0) {
        if (ui->atleast_val->text().isEmpty()) {
            show_warning("输入错误", "至少出 n 种僵尸未填写");
            ui->atleast_val->setFocus();
            return;
        }
        atleast_val = ui->atleast_val->text().toInt();
        if (atleast_val > atleast_count) {
            std::ostringstream oss;
            oss << "至少出的数量(" << atleast_val << ")不能超过勾选的僵尸数(" << atleast_count << ")";
            show_warning("输入错误", oss.str().c_str());
            return;
        }
    }

    int atmost_val = 0;
    if (atmost_count > 0) {
        if (ui->atmost_val->text().isEmpty()) {
            show_warning("输入错误", "至多出 n 种僵尸未填写");
            ui->atmost_val->setFocus();
            return;
        }
        atmost_val = ui->atmost_val->text().toInt();
        if (atmost_val > 19) atmost_val = 19;
        if (atmost_val < 0) atmost_val = 0;
    }

    for (int i = 1; i <= 19; ++i) {
        _need_flags[i] = need.at(i - 1);
        _refuse_flags[i] = refuse.at(i - 1);
        _atleast_flags[i] = atleast.at(i - 1);
        _atmost_flags[i] = atmost.at(i - 1);
    }
    _atleast = atleast_val;
    _atmost = atmost_val;
    this->close();
}
