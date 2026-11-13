#pragma once

#include <QDialog>

#include "seedengine.h"

class DetailView;
class QLabel;

// 单种子逐轮详情的独立弹窗 —— 内部直接复用 DetailView, 避免两套渲染逻辑
class DetailDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DetailDialog(QWidget *parent = nullptr);

    // uid / mode 不参与显示 (种子号是 32 位原始值, 与 uid/mode 无关),
    // 原来收下来只是为了 Q_UNUSED 丢掉, 直接去掉这两个参数。
    void setData(quint32 seed, int scene,
                 const std::vector<sf::DetailRow> &rows,
                 const sf::RuleTable &rank, const sf::RuleTable &hard);

    // 没有选中任何种子时清空内容 (窗口本身留着, 下次查找还能复用)
    void clear();

private:
    QLabel *m_title = nullptr;
    DetailView *m_view = nullptr;
};
