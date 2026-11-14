#include "style.h"

QString appStyleSheet()
{
    // 说明: 界面要求"近似方角", 所以除单选圈外所有圆角都压到 0~2px;
    // 所有按钮使用同一套样式(白底 + 浅灰边框), 不再区分强调/危险/幽灵。
    return QString::fromUtf8(R"QSS(
* {
    font-family: "Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", sans-serif;
    /* 11px: 条件表行高压到 18 之后, 12px 的字会顶到格子的上下边框,
       看起来像被切掉一截。全界面统一用 11px, 行高还能再省一点。 */
    font-size: 11px;
    color: #111111;
}

QMainWindow, QDialog {
    background: #f2f2f2;
}

/* ---------- 菜单栏 ---------- */
QMenuBar {
    background: #ffffff;
    border-bottom: 1px solid #dcdcdc;
    padding: 1px 4px;
}
QMenuBar::item {
    background: transparent;
    padding: 4px 10px;
    border-radius: 0;
    color: #222222;
}
QMenuBar::item:selected {
    background: #e8e8e8;
    color: #111111;
}
QMenuBar::item:pressed {
    background: #dcdcdc;
}

/* ---------- 卡片 ---------- */
QFrame#Card {
    background: #ffffff;
    border: 1px solid #dcdcdc;
    border-radius: 2px;
}
QLabel#CardTitle {
    font-size: 12px;
    font-weight: bold;
    color: #111111;
    /* 四张卡的标题等高: 只靠字号的话, 不同 DPI 下会差 1~2px, 卡片顶边看着就不齐 */
    min-height: 18px;
    padding: 0;
}
QLabel#Hint {
    color: #7a7a7a;
    font-size: 11px;
}
QLabel#Stat {
    color: #4a4a4a;
    font-size: 11px;
}

/* ---------- 输入控件 ---------- */
QLineEdit, QSpinBox, QComboBox {
    background: #ffffff;
    border: 1px solid #cccccc;
    border-radius: 0;
    /* 参数卡有 6 个输入控件, 上下各压 1px 就把这一卡省下了一大截 */
    padding: 2px 6px;
    min-height: 16px;
    selection-background-color: #111111;
    selection-color: #ffffff;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
    border: 1px solid #111111;
    background: #ffffff;
}
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {
    background: #f4f4f4;
    color: #a8a8a8;
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QComboBox QAbstractItemView {
    background: #ffffff;
    border: 1px solid #cccccc;
    selection-background-color: #e8e8e8;
    selection-color: #111111;
    outline: none;
}

/* ---------- 按钮: 全部统一为同一种样式 ---------- */
QPushButton {
    background: #ffffff;
    border: 1px solid #cccccc;
    border-radius: 0;
    /* 上下内边距只给 2px, 再配一个固定的 min-height:
       主界面三张卡各有一行按钮, 高度定死之后卡片高度才可预测,
       窗口压定之后才不会因为字号变化而忽高忽低。 */
    padding: 2px 12px;
    min-height: 20px;
    color: #222222;
}
QPushButton:hover {
    background: #f0f0f0;
    border-color: #999999;
}
QPushButton:pressed {
    background: #e2e2e2;
}
QPushButton:disabled {
    color: #b0b0b0;
    border-color: #e4e4e4;
    background: #f8f8f8;
}
QPushButton:focus {
    border-color: #888888;
    outline: none;
}

/* 微调按钮交给系统绘制: 一旦用 QSS 覆盖 up-button/down-button,
   箭头图形就会丢失, 所以这里不写这条规则 */

/* ---------- 菜单 ---------- */
QMenu {
    background: #ffffff;
    border: 1px solid #d0d0d0;
    border-radius: 0;
    padding: 4px;
}
QMenu::item {
    padding: 6px 22px 6px 14px;
    border-radius: 0;
    color: #222222;
}
QMenu::item:selected {
    background: #e8e8e8;
    color: #111111;
}
QMenu::separator {
    height: 1px;
    background: #dedede;
    margin: 4px 8px;
}

/* ---------- 表格 ---------- */
QTableWidget {
    background: #ffffff;
    border: 1px solid #dcdcdc;
    border-radius: 0;
    gridline-color: #eeeeee;
    alternate-background-color: #fafafa;
    outline: none;
    selection-background-color: #e4e4e4;
    selection-color: #111111;
}
QTableWidget::item {
    /* 上下只留 1px: 行高压到 18 时, 3px+3px 的内边距会把 11px 的字挤掉一截,
       看起来像文字被裁了。横向 4px 保留, 挨着列边框才不显得挤。 */
    padding: 1px 4px;
}
QTableWidget::item:selected {
    background: #e4e4e4;
    color: #111111;
}
QHeaderView::section {
    background: #f0f0f0;
    color: #111111;
    font-weight: bold;
    border: none;
    border-right: 1px solid #e2e2e2;
    border-bottom: 1px solid #d8d8d8;
    /* 表头高度写死 20, 上下各留 2px 才够放下 11px 的字 (原来 5px 只剩 10px 可用) */
    padding: 2px 4px;
}
QTableCornerButton::section {
    background: #f0f0f0;
    border: none;
}

/* ---------- 进度条 ---------- */
/* 高度交给代码控制 (mainwindow 里的 setFixedHeight): QSS 的 max-height 会盖掉
   setFixedHeight, 两处各写一个值只会让人以为改了没用。 */
QProgressBar {
    border: 1px solid #dcdcdc;
    border-radius: 0;
    background: #f0f0f0;
    text-align: center;
    color: #111111;
    font-size: 11px;
}
QProgressBar::chunk {
    border-radius: 0;
    background: #b0b0b0;
}

/* ---------- 滚动条 ---------- */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 2px;
}
QScrollBar::handle:vertical {
    background: #c4c4c4;
    border-radius: 0;
    min-height: 28px;
}
QScrollBar::handle:vertical:hover {
    background: #9e9e9e;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
    height: 0;
}
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 2px;
}
QScrollBar::handle:horizontal {
    background: #c4c4c4;
    border-radius: 0;
    min-width: 28px;
}
QScrollBar::handle:horizontal:hover {
    background: #9e9e9e;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    background: transparent;
    width: 0;
}

/* ---------- 复选 ---------- */
QCheckBox {
    spacing: 6px;
    color: #222222;
}
QCheckBox::indicator {
    width: 14px;
    height: 14px;
}
QCheckBox::indicator:unchecked {
    border: 1px solid #b0b0b0;
    border-radius: 0;
    background: #ffffff;
}
QCheckBox::indicator:checked {
    border: 1px solid #2f6fed;
    border-radius: 0;
    background: #2f6fed;
    image: url(:/check_white.png);
}

/* 表格里的勾选格(QTableWidgetItem 的 CheckState)走的是另一条规则,
   上面的 QCheckBox::indicator 管不到, 不写这段就会是系统默认的蓝色方块 */
QTableWidget::indicator {
    width: 14px;
    height: 14px;
}
QTableWidget::indicator:unchecked {
    border: 1px solid #b0b0b0;
    border-radius: 0;
    background: #ffffff;
}
QTableWidget::indicator:checked {
    border: 1px solid #2f6fed;
    border-radius: 0;
    background: #2f6fed;
    image: url(:/check_white.png);
}
)QSS");
}
