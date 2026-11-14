// 用 1.2 版原版实现 (ref12/worker.cpp) 实测"出怪要求", 用来核对条件校验器。
//
// 只做两件事:
//   1) 打印每一轮的僵尸种类数分布与第一轮的固定位模式 —— 这是"出怪要求"的直接证据
//   2) 拿一条用户报过误报的条件(3~4 F 禁出一长串), 逐场景数出真实解的数量,
//      再和 sf::checkBandLocal 的判定并列打印
//
// 判定一律用原版 single_seed / d_page, 不在这里另写一套枚举逻辑。
#include "ref12/worker.cpp"

#include "seedengine.h"
#include "zombiedata.h"

#include <QCoreApplication>
#include <QRandomGenerator>

#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

namespace {

QString abbrs(quint32 bits)
{
    QString s;
    for (int z = 0; z < Z_COUNT; ++z)
        if (bits & (1u << z)) s += zombieAbbr(z);
    return s;
}

// ---------------------------------------------------------------------------
// 1) 每一轮的种类数上下界 + 第一轮位模式
// ---------------------------------------------------------------------------
void dumpShape(int scene, int samples)
{
    d_page p(scene);
    const int kMaxLv = 10;
    int lo[kMaxLv], hi[kMaxLv];
    std::map<quint32, int> firstBits;
    for (int i = 0; i < kMaxLv; ++i) { lo[i] = 99; hi[i] = -1; }

    for (int k = 0; k < samples; ++k) {
        const quint32 rVal = quint32(QRandomGenerator::global()->generate());
        for (int lv = 1; lv < kMaxLv; ++lv) {
            const int bits = p.get(rVal + quint32(lv), lv);
            const int n = __popcnt(unsigned(bits));
            lo[lv] = std::min(lo[lv], n);
            hi[lv] = std::max(hi[lv], n);
        }
        ++firstBits[quint32(p.get(rVal, 0))];
    }

    std::printf("scene %d: 第一轮位模式 %d 种:", scene, int(firstBits.size()));
    for (const auto &kv : firstBits)
        std::printf(" 0x%03X(%s)", kv.first, abbrs(kv.first).toUtf8().constData());
    std::printf("\n");
    for (int lv = 1; lv < kMaxLv; ++lv) {
        std::printf("  level %d: 种类 %d..%d  (公式 %d..%d)\n", lv, lo[lv], hi[lv],
                    std::min(lv + 1, 9), std::min(lv + 3, 11));
    }
}

// ---------------------------------------------------------------------------
// 2) 一条具体条件: 真实解数 vs checkBandLocal 的判定
// ---------------------------------------------------------------------------
void checkOne(const char *name, int scene, int beginFlag, int endFlag, quint32 refuse)
{
    const int lvBegin = beginFlag / 2;
    const int lvEnd = endFlag / 2;

    sf::RuleBand b;
    b.beginFlag = beginFlag;
    b.endFlag = endFlag;
    b.cond.refuseMask = refuse;
    b.cond.sanitize();

    const QString msg = sf::checkBandLocal(b, scene, QString::fromUtf8("条件"));
    const int total = __popcnt(unsigned(sceneZombieMask(scene) & ~refuse & ((1u << Z_COUNT) - 1u)));

    // 原版口径: 逐种子算, r_val 空间连续扫 (与 1.2 的 work() 一致)
    d_page p(scene);
    const int kSeeds = 20000;
    quint64 hit = 0;
    for (int i = 0; i < kSeeds; ++i) {
        const quint32 rVal = quint32(0x40000000u + i);
        if (single_seed(rVal, p, 0, int(refuse), 0, 0, 0, 0, lvBegin, lvEnd) == lvEnd - lvBegin)
            ++hit;
    }

    std::printf("[%s] scene=%d  F%d-%d (level %d..%d)\n", name, scene, beginFlag + 1, endFlag,
                lvBegin, lvEnd);
    std::printf("    可出且不禁出的种类 = %d,  该段最多槽位 = %d\n", total,
                std::min(lvEnd, 9));
    std::printf("    1.2 实测: %d 个种子里有 %llu 个满足 (%.4f%%)\n", kSeeds,
                static_cast<unsigned long long>(hit), 100.0 * double(hit) / kSeeds);
    std::printf("    校验器判定: %s\n",
                msg.isEmpty() ? "放行" : msg.toUtf8().constData());
    std::printf("\n");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const int samples = (argc > 1) ? std::max(1, QString::fromUtf8(argv[1]).toInt()) : 40000;
    std::printf("== 每轮种类数上下界 (原版 d_page, 每场景 %d 样本) ==\n", samples);
    for (int scene : sceneIds()) dumpShape(scene, samples);
    std::printf("\n");

    // 用户报过的那条: 3~4 F 禁出 杆/桶/报/门/橄/舞/车/丑/气/矿/跳/蹦/梯/篮/白/红
    const quint32 refuse = (1u << Z_POLE) | (1u << Z_BUCKET) | (1u << Z_NEWS) | (1u << Z_DOOR)
                           | (1u << Z_FOOTBALL) | (1u << Z_DANCE) | (1u << Z_ZOMBONI)
                           | (1u << Z_JACK) | (1u << Z_BALLOON) | (1u << Z_DIGGER)
                           | (1u << Z_POGO) | (1u << Z_BUNGEE) | (1u << Z_LADDER)
                           | (1u << Z_CATAPULT) | (1u << Z_GARG) | (1u << Z_GIGA);
    std::printf("== 用户报误报的那条 (refuse=0x%X, 共 %d 种) ==\n", refuse,
                __popcnt(unsigned(refuse)));
    for (int scene : sceneIds()) checkOne("3~4F 禁出一长串", scene, 2, 4, refuse);

    // 同一条条件换个更长的区间: 上一段 2 轮、这一段 8 轮
    std::printf("== 同一条条件, 区间拉到 3~10 F (level 1..5) ==\n");
    for (int scene : sceneIds()) checkOne("3~10F 禁出一长串", scene, 2, 10, refuse);

    return 0;
}
