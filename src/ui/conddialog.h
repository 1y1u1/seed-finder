#pragma once

#include <QDialog>

#include "seedengine.h"

class QTableWidget;
class QSpinBox;
class QLabel;

// 出怪条件编辑对话框 —— 四组勾选: 必出 / 禁出 / 至少 / 至多
class CondDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CondDialog(int scene, QWidget *parent = nullptr);

    void setCondition(const sf::WaveCondition &c);
    sf::WaveCondition condition() const;

private slots:
    void onAccept();
    void onClear();

private:
    void buildUi();
    void rebuildTable();
    void syncCountLimits();
    void normalizeMasks();

    int m_scene;
    quint32 m_needMask = 0;
    quint32 m_refuseMask = 0;
    quint32 m_atLeastMask = 0;
    quint32 m_atMostMask = 0;

    QVector<int> m_rows;        // 行号 → 僵尸 id

    QTableWidget *m_table = nullptr;
    QSpinBox *m_atLeastSpin = nullptr;
    QSpinBox *m_atMostSpin = nullptr;
    QLabel *m_atLeastLabel = nullptr;
    QLabel *m_atMostLabel = nullptr;
};
