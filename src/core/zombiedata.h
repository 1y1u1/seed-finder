#pragma once

// 僵尸编号 / 名称 / 场景允许表 —— 与原版 worker.h 完全一致
#include <QString>
#include <QStringList>
#include <QVector>

enum ZombieId {
    Z_REGULAR = 0,
    Z_CONE,
    Z_POLE,
    Z_BUCKET,
    Z_NEWS,
    Z_DOOR,
    Z_FOOTBALL,
    Z_DANCE,
    Z_SNORKEL,
    Z_ZOMBONI,
    Z_DOLPHIN,
    Z_JACK,
    Z_BALLOON,
    Z_DIGGER,
    Z_POGO,
    Z_BUNGEE,
    Z_LADDER,
    Z_CATAPULT,
    Z_GARG,
    Z_GIGA,
    Z_COUNT
};

// 场景索引: 0=DE 1=NE 2=PE/FE 3=FE 4=RE/ME 5=ME 6=MGE/AQE 7=AQE
// 注意: bit24/bit25 恒为 1 是有意义的 —— 抽卡表里 type 24/25 表示"抽到但不上场"的
// 空槽 (computeRow 里 `nid != 24 && nid != 25` 的分支), 所以不能裁掉。
inline int sceneAllowMask(int scene)
{
    static const int allow_base[8] = {
        0x0f0ffaff, 0x0f0ff8ff, 0x0f0fffff, 0x0f0fffff,
        0x0f0fda7f, 0x0f0fda7f, 0x0f0ffaff, 0x0f0ffaff
    };
    return allow_base[scene & 7];
}

// 只含真实僵尸 (0..Z_COUNT-1) 的允许位, 用于界面上的"种类数"统计
inline quint32 sceneZombieMask(int scene)
{
    return quint32(sceneAllowMask(scene)) & ((1u << Z_COUNT) - 1u);
}

inline QString zombieName(int id)
{
    static const char *names[Z_COUNT] = {
        "普僵", "路障", "撑杆", "铁桶", "读报", "铁门", "橄榄", "舞王", "潜水", "冰车",
        "海豚", "小丑", "气球", "矿工", "跳跳", "蹦极", "扶梯", "投篮", "白眼", "红眼"
    };
    if (id < 0 || id >= Z_COUNT) return QString();
    return QString::fromUtf8(names[id]);
}

// 单字简称: 出怪条件表格里一行要塞下"必出 路障、撑杆、铁桶…"这种长串,
// 用两字全称会被截断, 所以列表里统一用单字 (20 个单字互不重复)
// 注意 15/17 号: 蹦极取"偷"、投篮取"篮", 与游戏里的常见叫法一致
inline QString zombieAbbr(int id)
{
    static const char *abbr[Z_COUNT] = {
        "普", "障", "杆", "桶", "报", "门", "橄", "舞", "潜", "车",
        "豚", "丑", "气", "矿", "跳", "偷", "梯", "篮", "白", "红"
    };
    if (id < 0 || id >= Z_COUNT) return QString();
    return QString::fromUtf8(abbr[id]);
}

// 场景下拉: 显示名 + 对应 scene 编号
inline QStringList sceneDisplayNames()
{
    return QStringList() << QString::fromUtf8("DE")
                         << QString::fromUtf8("NE")
                         << QString::fromUtf8("PE/FE")
                         << QString::fromUtf8("RE/ME")
                         << QString::fromUtf8("MGE/AQE");
}

inline QVector<int> sceneIds()
{
    QVector<int> v;
    v << 0 << 1 << 2 << 4 << 6;
    return v;
}

// 某场景下可能出现的僵尸列表 (按界面展示顺序)
inline QVector<int> sceneZombieList(int scene)
{
    QVector<int> lst;
    // 展示顺序即枚举顺序; 普僵几乎总会作为基础僵尸出现, 界面层自己过滤
    const int allow = sceneAllowMask(scene);
    for (int z = 0; z < Z_COUNT; ++z) {
        if (allow & (1 << z)) lst << z;
    }
    return lst;
}
