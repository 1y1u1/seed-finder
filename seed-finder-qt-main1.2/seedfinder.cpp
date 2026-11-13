#include "seedfinder.h"
#include "ui_seedfinder.h"
#include "worker.h"
#include "setting.h"
#include "spawn.h"
#include "howto.h"
#include "about.h"
#include "table.h"
#include <ctime>
#include <QMessageBox>
#include <QApplication>
#include <QRandomGenerator>
#include <QDate>
#include <QClipboard>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>

SeedFinder::SeedFinder(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::SeedFinder)
{
    ui->setupUi(this);
    qRegisterMetaType<std::vector<SeedInfo>>("std::vector<SeedInfo>");

    // add validators
    QRegExp rx_2_digit("[1-9]\\d{0,1}");
    ui->uid->setValidator(new QRegExpValidator(rx_2_digit, this));
    ui->mode->setValidator(new QRegExpValidator(rx_2_digit, this));
    QRegExp rx_5_digit("[1-9]\\d{0,4}|0");
    ui->start_flags->setValidator(new QRegExpValidator(rx_5_digit, this));
    QRegExp rx_3_digit("[1-9]\\d{0,2}");
    ui->flags_span->setValidator(new QRegExpValidator(rx_3_digit, this));
    QRegExp rx_3_digit_ver2("[1-9]\\d{0,1}|100|0");
    ui->hardness->setValidator(new QRegExpValidator(rx_3_digit_ver2, this));
    QRegExp rx_hex_digit("[0-9a-fA-F]{8}");
    ui->seeds->setValidator(new QRegExpValidator(rx_hex_digit, this));

    // set up default start flags
    int year = QDate::currentDate().year() & ~1;
    ui->start_flags->setText(QString::number(year));

    // set up combobox
    ui->scenes->addItems({"DE", "NE", "PE/FE", "RE/ME", "MGE/AQE"});
    ui->scenes->setCurrentIndex(2);

    this->adjustSize();

    // create child pages
    setting_page = new Setting(this);
    spawn_page = new Spawn(this);
    howto_page = new HowTo();
    about_page = new About(this);
    table_page = new Table();
    table_page->setWindowTitle(QString::fromUtf8("出怪详情"));
}

SeedFinder::~SeedFinder()
{
    if (m_worker != nullptr) m_worker->stop();
    m_workThread.quit();
    m_workThread.wait();
    delete ui;
}

void SeedFinder::show_child_window(QWidget *w)
{
    if (w->isVisible()) {
        w->activateWindow();
        w->raise();
    } else {
        w->show();
    }
}

void SeedFinder::on_detailedSetting_triggered()
{
    setting_page->show_setting();
    setting_page->exec();
}

void SeedFinder::on_spawnSetting_triggered()
{
    spawn_page->show_setting(get_scene());
    spawn_page->exec();
}

void SeedFinder::on_howToUse_triggered()
{
    show_child_window(howto_page);
}

void SeedFinder::on_about_triggered()
{
    about_page->exec();
}

void SeedFinder::update_end_flags()
{
    ui->end_flags->setText(QString::number(ui->start_flags->text().toInt() + ui->flags_span->text().toInt() * 2));
}

void SeedFinder::on_start_flags_textChanged(const QString &arg1)
{
    Q_UNUSED(arg1)
    update_end_flags();
}

void SeedFinder::on_flags_span_textEdited(const QString &arg1)
{
    Q_UNUSED(arg1)
    update_end_flags();
}

void SeedFinder::on_hardness_textEdited(const QString &arg1)
{
    if (arg1 == "") return;
    ui->hardness_slider->setValue(arg1.toInt());
}

void SeedFinder::on_hardness_slider_valueChanged(int value)
{
    ui->hardness->setText(QString::number(value));
}

void SeedFinder::set_all_widgets_availability(bool flag)
{
    for (auto w : {ui->uid, ui->mode, ui->start_flags, ui->flags_span, ui->hardness}) {
        w->setEnabled(flag);
    }
    for (auto w : {ui->calc, ui->copy, ui->clear, ui->detail}) {
        w->setEnabled(flag);
    }
    for (auto w : {ui->scenes, ui->seeds}) {
        w->setEnabled(flag);
    }
    ui->menubar->setEnabled(flag);
    ui->hardness_slider->setEnabled(flag);
    ui->stop->setEnabled(!flag);   // 计算中才允许点击停止
}

void SeedFinder::show_warning(const char *title, const char *msg)
{
    QMessageBox Msgbox(this);
    Msgbox.setWindowTitle(QString::fromUtf8(title));
    Msgbox.setIcon(QMessageBox::Information);
    Msgbox.setText(QString::fromUtf8(msg));
    Msgbox.exec();
}

int SeedFinder::get_need_mask()
{
    int mask = 0;
    for (int idx = 1; idx <= GIGA; ++idx) {
        if (spawn_page->_need_flags.at(idx)) {
            mask |= 1 << idx;
        }
    }
    return mask;
}

int SeedFinder::get_refuse_mask()
{
    int mask = 0;
    for (int idx = 1; idx <= GIGA; ++idx) {
        if (spawn_page->_refuse_flags.at(idx)) {
            mask |= 1 << idx;
        }
    }
    return mask;
}

int SeedFinder::get_atleast_mask()
{
    int mask = 0;
    for (int idx = 1; idx <= GIGA; ++idx) {
        if (spawn_page->_atleast_flags.at(idx)) {
            mask |= 1 << idx;
        }
    }
    return mask;
}

int SeedFinder::get_atmost_mask()
{
    int mask = 0;
    for (int idx = 1; idx <= GIGA; ++idx) {
        if (spawn_page->_atmost_flags.at(idx)) {
            mask |= 1 << idx;
        }
    }
    return mask;
}

static int bit_count(int v)
{
    int n = 0;
    while (v) {
        v &= v - 1;
        ++n;
    }
    return n;
}

int SeedFinder::get_effective_atleast()
{
    if (spawn_page->_atleast < 0) return bit_count(get_atleast_mask()); // 默认=勾选数
    return spawn_page->_atleast;
}

int SeedFinder::get_effective_atmost()
{
    return spawn_page->_atmost;
}

std::vector<uint32_t> get_seeds_from_result(std::vector<SeedInfo> &result, int hardness, int output_size)
{
    std::sort(result.begin(), result.end());

    uint64_t total = 0;
    for (auto &&r : result) {
        total += r.seed_count;
    }
    total = uint64_t(total / 100.0 * hardness);
    uint64_t count = 0;
    int idx = 0;
    for (auto &&r : result) {
        count += r.seed_count;
        if (count >= total) break;
        ++idx;
    }

    // output
    std::vector<uint32_t> seeds;
    int upper_pos = idx, lower_pos = idx - 1;
    while ((upper_pos < int(result.size()) || lower_pos > 0) && int(seeds.size()) < output_size) {
        if (upper_pos < int(result.size())) {
            for (auto &&e : result.at(upper_pos).seeds) {
                if (int(seeds.size()) >= output_size) break;
                seeds.push_back(e);
            }
            ++upper_pos;
        }
        if (int(seeds.size()) >= output_size) break;
        if (lower_pos > 0) {
            for (auto &&e : result.at(lower_pos).seeds) {
                if (int(seeds.size()) >= output_size) break;
                seeds.push_back(e);
            }
            --lower_pos;
        }
    }
    std::sort(seeds.begin(), seeds.end());
    return seeds;
}

bool SeedFinder::check_inputs()
{
    if (ui->uid->text().isEmpty()) {
        show_warning("输入错误", "用户ID为空");
        ui->uid->setFocus();
        return false;
    }
    if (ui->mode->text().isEmpty()) {
        show_warning("输入错误", "游戏模式为空");
        ui->mode->setFocus();
        return false;
    }
    if (ui->flags_span->text().isEmpty()) {
        show_warning("输入错误", "计算跨度为空");
        ui->flags_span->setFocus();
        return false;
    }
    if (ui->start_flags->text().toInt() % 2 != 0) {
        show_warning("输入错误", "起始F数应为偶数");
        ui->start_flags->setFocus();
        return false;
    }
    if (ui->start_flags->text().isEmpty()) {
        ui->start_flags->setText("0");
    }
    return true;
}

int SeedFinder::get_scene()
{
    std::vector<int> scenes = {0, 1, 2, 4, 6};
    return scenes[ui->scenes->currentIndex()];
}

bool SeedFinder::check_current_seed()
{
    if (ui->seeds->currentText().isEmpty()) {
        show_warning("输入错误", "当前种子为空");
        ui->seeds->setFocus();
        return false;
    }
    return true;
}

void SeedFinder::on_detail_clicked()
{
    // check inputs
    if (!check_inputs() || !check_current_seed())
        return;

    show_detail(true);
}

void SeedFinder::on_calc_clicked()
{
    if (!check_inputs()) {
        ui->calc->setChecked(false);
        return;
    }

    // modify ui
    ui->calc->setText(QString::fromUtf8("计算中..."));
    ui->seeds->clear();
    ui->progressBar->setValue(0);
    set_all_widgets_availability(false);
    qApp->processEvents();

    // get seed range
    uint32_t seed_start = setting_page->_seed_start;
    uint64_t divisor = SEED_TOTAL - setting_page->_seed_span + 2;
    if (setting_page->_start_at_random_seed) seed_start = QRandomGenerator::global()->generate() % divisor;
    uint64_t seed_end = seed_start + setting_page->_seed_span; // 不包含end
    int need_mask = get_need_mask();
    int refuse_mask = get_refuse_mask();
    int atleast_mask = get_atleast_mask();
    int atmost_mask = get_atmost_mask();
    int n_atleast = get_effective_atleast();
    int n_atmost = get_effective_atmost();

    // start calculation
    if (m_worker != nullptr) delete m_worker;
    m_worker = new SeedCalc(ui->uid->text().toInt(), ui->mode->text().toInt(), get_scene(), ui->start_flags->text().toInt() / 2, ui->end_flags->text().toInt() / 2,
                            seed_start, seed_end, need_mask, refuse_mask, atleast_mask, atmost_mask,
                            n_atleast, n_atmost, setting_page->_output_size);
    m_worker->moveToThread(&m_workThread);
    connect(this, SIGNAL(start_worker_calc()), m_worker, SLOT(calc()));
    connect(&m_workThread, SIGNAL(finished()), m_worker, SLOT(deleteLater()));
    connect(m_worker, SIGNAL(progress_updated(int)), this, SLOT(update_progress_bar(int)));
    connect(m_worker, SIGNAL(output_result(std::vector<SeedInfo>)), this, SLOT(show_result(std::vector<SeedInfo>)));
    connect(m_worker, SIGNAL(show_debug_info(QString)), this, SLOT(update_debug_info(QString)));
    m_workThread.start();
    emit start_worker_calc();
}

void SeedFinder::update_debug_info(QString str)
{
    ui->label_1->setText(str);
}

void SeedFinder::show_detail(bool bring_to_front)
{
    // get seed
    uint32_t seed = std::stoul(ui->seeds->currentText().toStdString(), nullptr, 16);

    // calculate
    SeedCalc sc(ui->uid->text().toInt(), ui->mode->text().toInt(), get_scene(), ui->start_flags->text().toInt() / 2, ui->end_flags->text().toInt() / 2,
                seed);
    auto result = sc.view_detail();

    // update
    table_page->update_contents(result, ui->start_flags->text().toInt(), get_scene(),
                                spawn_page->_need_flags, spawn_page->_refuse_flags,
                                spawn_page->_atleast_flags, spawn_page->_atmost_flags,
                                get_effective_atleast(), get_effective_atmost(), bring_to_front);
}

void SeedFinder::update_progress_bar(int val)
{
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    ui->progressBar->setValue(val);
}

void SeedFinder::show_result(std::vector<SeedInfo> result)
{
    auto list = get_seeds_from_result(result, ui->hardness->text().toInt(), setting_page->_output_size);

    // print result
    ui->calc->setText(QString::fromUtf8("计算"));
    set_all_widgets_availability(true);
    ui->progressBar->setValue(100);
    for (auto r : list) {
        std::ostringstream ss;
        ss << std::setfill('0') << std::setw(8) << std::hex << std::uppercase << r;
        ui->seeds->addItem(QString::fromStdString(ss.str()));
    }
    if (table_page->isVisible()) show_detail(false);
}

void SeedFinder::on_copy_clicked()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(ui->seeds->currentText());
}

void SeedFinder::on_clear_clicked()
{
    ui->seeds->clear();
    ui->seeds->setCurrentText("");
    ui->progressBar->setValue(0);
}

void SeedFinder::on_stop_clicked()
{
    if (m_worker != nullptr) {
        m_worker->stop();
        ui->stop->setEnabled(false);
        update_debug_info(QString::fromUtf8("正在停止, 稍后将输出已计算部分的种子..."));
    }
}
