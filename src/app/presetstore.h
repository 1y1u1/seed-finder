#pragma once

#include <QString>
#include <QVector>

#include "seedengine.h"

// 一套完整的查找参数, 可以存盘复用
struct Preset
{
    QString name;
    int uid = 1;
    int mode = 13;
    int scene = 2;
    int beginFlag = 2;      // 偶数 (= 起始轮 - 1)
    int endFlag = 50;       // 偶数 (= 终止轮)
    int outputSize = 20;
    QVector<sf::RuleBand> bands;    // 出怪条件 (只用于排名)
    QVector<sf::RuleBand> hard;     // 全局条件 (硬性要求)
};

// 预设存盘位置: 优先 exe 同目录 presets.json, 不可写则退 AppData
QString presetFilePath();

QVector<Preset> loadPresets(QString *error);
bool savePresets(const QVector<Preset> &list, QString *error);
