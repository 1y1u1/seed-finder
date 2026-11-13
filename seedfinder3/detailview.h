#pragma once

#include <QWidget>

#include "seedengine.h"

class QTableWidget;
class QLabel;

// 单种子的逐轮详情表 (放在独立的"出怪详情"对话框里)
class DetailView : public QWidget
{
    Q_OBJECT

public:
    explicit DetailView(QWidget *parent = nullptr);

    void clear();
    void setData(quint32 seed, int scene, const std::vector<sf::DetailRow> &rows,
                 const sf::RuleTable &rank, const sf::RuleTable &hard);

    // 表格需要的宽度 (各列定宽之和 + 边框)。窗口用它把自己收到刚好放得下,
    // 不留右侧那片空白。
    int preferredWidth() const;

private:
    void rebuild();

    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;

    quint32 m_seed = 0;
    std::vector<sf::DetailRow> m_rows;
    sf::RuleTable m_rankTable;
    sf::RuleTable m_hardTable;
    QVector<int> m_cols;
};
