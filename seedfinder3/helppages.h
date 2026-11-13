#pragma once

class QWidget;
class QDialog;

// 构建使用说明 / 关于 两个信息窗口 (调用者负责 exec 或截图)
QDialog *buildHowto(QWidget *parent);
QDialog *buildAbout(QWidget *parent);

// 直接弹出
void showHowto(QWidget *parent);
void showAbout(QWidget *parent);
