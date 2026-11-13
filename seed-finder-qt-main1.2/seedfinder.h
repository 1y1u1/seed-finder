#ifndef SEEDFINDER_H
#define SEEDFINDER_H

#include <QMainWindow>
#include <QThread>
#include "worker.h"

QT_BEGIN_NAMESPACE
namespace Ui { class SeedFinder; }
QT_END_NAMESPACE

#define VERSION_NAME_FULL "2.0"

class SeedFinder : public QMainWindow
{
    Q_OBJECT

public:
    SeedFinder(QWidget *parent = nullptr);
    ~SeedFinder();
signals:
    void start_worker_calc();
public slots:
    void update_progress_bar(int val);
    void show_result(std::vector<SeedInfo> result);
    void update_debug_info(QString str);
private slots:
    void on_detailedSetting_triggered();
    void on_spawnSetting_triggered();
    void on_howToUse_triggered();

    void on_about_triggered();

    void on_start_flags_textChanged(const QString &arg1);

    void on_flags_span_textEdited(const QString &arg1);

    void on_hardness_textEdited(const QString &arg1);

    void on_hardness_slider_valueChanged(int value);

    void on_detail_clicked();

    void on_calc_clicked();

    void on_copy_clicked();

    void on_clear_clicked();

    void on_stop_clicked();

private:
    void show_child_window(QWidget *dialog);
    void update_end_flags();
    void set_all_widgets_availability(bool flag);
    void show_warning(const char *title, const char *msg);
    int get_scene();
    bool check_inputs();
    bool check_current_seed();
    int get_need_mask();
    int get_refuse_mask();
    int get_atleast_mask();
    int get_atmost_mask();
    int get_effective_atleast();
    int get_effective_atmost();
    void show_detail(bool bring_to_front);
    Ui::SeedFinder *ui = nullptr;
    class Setting *setting_page = nullptr;
    class Spawn *spawn_page = nullptr;
    class HowTo *howto_page = nullptr;
    class About *about_page = nullptr;
    class Table *table_page = nullptr;

    QThread m_workThread;
    class SeedCalc *m_worker = nullptr;
};
#endif // SEEDFINDER_H
