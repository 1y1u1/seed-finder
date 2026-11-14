// 条件校验器审计: 系统性地找"误报无解"(校验器拦下, 但实际上有解)的条件。
//
// 判定一律用 1.2 版原版实现:
//   - 每一轮出怪位用 d_page::get() 取 (与原版 single_seed 内部完全一致)
//   - 条件判定逐条照抄 single_seed: 必出全中 / 禁出全不中 / 至少 n / 至多 n
//   - 发现可疑用例后再用 single_seed() 逐种子复核, 确认不是采样假象
//
// 枚举范围 (覆盖用户实际能勾出来的各种组合):
//   need    1~3 个必出
//   refuse  只剩 1~3 个能出 (即"禁出一长串"那一类)
//   atLeast 2~3 个里至少 1~2 个
//   atMost  2~3 个里至多 0~1 个
// 区间: 每个 level 单独一段 (1..9), 外加几个跨轮区间。
#include "ref12/worker.cpp"

#include "seedengine.h"
#include "zombiedata.h"

#include <QCoreApplication>

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

namespace {

const int kSeeds = 4000;
const int kMaxLv = 12;      // 用到 level 1..11

struct Table
{
    int scene = 0;
    std::vector<unsigned> bits;                 // [seed * kMaxLv + level]
    std::vector<quint32> patterns[kMaxLv];      // 每轮出现过的位模式

    unsigned at(int s, int lv) const { return bits[size_t(s) * kMaxLv + size_t(lv)]; }
};

void buildTable(Table &t)
{
    d_page p(t.scene);
    t.bits.assign(size_t(kSeeds) * kMaxLv, 0u);
    for (int lv = 0; lv < kMaxLv; ++lv) { t.patterns[lv].clear(); }
    std::set<quint32> seen[kMaxLv];

    for (int i = 0; i < kSeeds; ++i) {
        const quint32 rVal = quint32(0x40000000u + i);
        for (int lv = 1; lv < kMaxLv; ++lv) {
            const quint32 b = quint32(p.get(rVal + quint32(lv), lv));
            t.bits[size_t(i) * kMaxLv + size_t(lv)] = b;
            if (seen[lv].insert(b).second) t.patterns[lv].push_back(b);
        }
    }
}

// 与 1.2 的 single_seed 内部判定逐条对应
bool rowOk(quint32 bits, const sf::WaveCondition &c)
{
    if (__popcnt(bits & c.needMask) != unsigned(c.needTotal)) return false;
    if (bits & c.refuseMask) return false;
    if (int(__popcnt(bits & c.atLeastMask)) < c.atLeast) return false;
    if (int(__popcnt(bits & c.atMostMask)) > c.atMost) return false;
    return true;
}

// 该区间里是否存在至少一个种子每一轮都满足 (1.2 口径)
bool hasSolution(const Table &t, int lvBegin, int lvEnd, const sf::WaveCondition &c)
{
    // 单轮: 直接看该轮出现过的位模式, 一次遍历就够, 不用扫种子
    if (lvEnd - lvBegin == 1) {
        for (quint32 p : t.patterns[lvBegin])
            if (rowOk(p, c)) return true;
        return false;
    }
    for (int s = 0; s < kSeeds; ++s) {
        bool ok = true;
        for (int lv = lvBegin; lv < lvEnd; ++lv) {
            if (!rowOk(t.at(s, lv), c)) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

// 用原版 single_seed 复核: 数一个大样本里有多少真解
int countWithRef12(int scene, int lvBegin, int lvEnd, const sf::WaveCondition &c, int seeds)
{
    d_page p(scene);
    int hits = 0;
    for (int i = 0; i < seeds; ++i) {
        const quint32 rVal = quint32(0x40000000u + i);
        if (single_seed(rVal, p, int(c.needMask), int(c.refuseMask), int(c.atLeastMask),
                        int(c.atMostMask), c.atLeast, c.atMost, lvBegin, lvEnd)
            == lvEnd - lvBegin)
            ++hits;
    }
    return hits;
}

QString abbrs(quint32 bits)
{
    QString s;
    for (int z = 0; z < Z_COUNT; ++z)
        if (bits & (1u << z)) s += zombieAbbr(z);
    return s.isEmpty() ? QStringLiteral("无") : s;
}

struct Range
{
    int lvBegin;
    int lvEnd;
    QString text() const
    {
        return QStringLiteral("F%1-%2").arg(lvBegin * 2 + 1).arg(lvEnd * 2);
    }
};

struct Finding
{
    QString kind;
    int scene;
    Range range;
    sf::WaveCondition cond;
    QString verdict;
    int refHits;
};

int g_falseReject = 0;
int g_falseAccept = 0;
long long g_checked = 0;
QVector<Finding> g_findings;

void auditOne(const char *kind, int scene, const Table &t, const Range &r,
              const sf::WaveCondition &in)
{
    sf::WaveCondition c = in;
    c.sanitize();
    if (c.isEmpty()) return;

    // 引擎里 beginLevel() = beginFlag/2, endLevel() = endFlag/2, 覆盖 [beginLevel, endLevel),
    // 所以要让覆盖区间正好是 [lvBegin, lvEnd), 取 beginFlag = 2*lvBegin, endFlag = 2*lvEnd。
    sf::RuleBand b;
    b.beginFlag = r.lvBegin * 2;
    b.endFlag = r.lvEnd * 2;
    b.cond = c;

    const QString err = sf::checkBandLocal(b, scene, QStringLiteral("条件"));
    const bool solverReject = !err.isEmpty();
    const bool realSol = hasSolution(t, r.lvBegin, r.lvEnd, c);
    ++g_checked;

    if (solverReject && realSol) {
        const int hits = countWithRef12(scene, r.lvBegin, r.lvEnd, c, 20000);
        if (hits <= 0) return;      // 1.2 复核也没解: 采样假象, 不算误报
        ++g_falseReject;
        Finding f;
        f.kind = QString::fromLatin1(kind);
        f.scene = scene;
        f.range = r;
        f.cond = c;
        f.verdict = err;
        f.refHits = hits;
        if (g_findings.size() < 120) g_findings << f;
    } else if (!solverReject && !realSol) {
        ++g_falseAccept;
    }
}

void subsets(const QVector<int> &pool, int k, int start, quint32 acc, QVector<quint32> &out)
{
    if (k == 0) { out << acc; return; }
    for (int i = start; i + k <= pool.size(); ++i)
        subsets(pool, k - 1, i + 1, acc | (1u << pool[i]), out);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QVector<Range> ranges;
    for (int lv = 1; lv <= 9; ++lv) ranges << Range{ lv, lv + 1 };
    ranges << Range{ 1, 3 };
    ranges << Range{ 1, 5 };
    ranges << Range{ 2, 6 };

    for (int scene : sceneIds()) {
        Table t;
        t.scene = scene;
        buildTable(t);

        const quint32 allow = quint32(sceneZombieMask(scene));
        QVector<int> pool;
        for (int z = 1; z < Z_COUNT; ++z)
            if (allow & (1u << z)) pool << z;
        quint32 keepAll = 0;
        for (int z : pool) keepAll |= (1u << z);

        std::printf("== scene %d, 可勾僵尸 %d 种 ==\n", scene, pool.size());

        for (const Range &r : ranges) {
            QVector<quint32> sub1, sub2, sub3;
            subsets(pool, 1, 0, 0u, sub1);
            subsets(pool, 2, 0, 0u, sub2);
            subsets(pool, 3, 0, 0u, sub3);
            QVector<quint32> all3 = sub1;
            all3 += sub2;
            all3 += sub3;

            for (quint32 m : all3) {                 // need
                sf::WaveCondition c;
                c.needMask = m;
                auditOne("need", scene, t, r, c);
            }
            for (quint32 keep : all3) {              // refuse: 只剩这 1~3 个能出
                sf::WaveCondition c;
                c.refuseMask = keepAll & ~keep;
                auditOne("refuse", scene, t, r, c);
            }
            for (quint32 m : sub2) {                 // atLeast 2 个里至少 1
                sf::WaveCondition c;
                c.atLeastMask = m;
                c.atLeast = 1;
                auditOne("atLeast", scene, t, r, c);
            }
            for (quint32 m : sub3) {                 // atLeast 3 个里至少 1 / 2
                for (int n = 1; n <= 2; ++n) {
                    sf::WaveCondition c;
                    c.atLeastMask = m;
                    c.atLeast = n;
                    auditOne("atLeast", scene, t, r, c);
                }
            }
            for (quint32 m : sub2) {                 // atMost 2 个里至多 0 / 1
                for (int n = 0; n <= 1; ++n) {
                    sf::WaveCondition c;
                    c.atMostMask = m;
                    c.atMost = n;
                    auditOne("atMost", scene, t, r, c);
                }
            }
            for (quint32 m : sub3) {                 // atMost 3 个里至多 1
                sf::WaveCondition c;
                c.atMostMask = m;
                c.atMost = 1;
                auditOne("atMost", scene, t, r, c);
            }
        }
        std::printf("   累计: checked=%lld falseReject=%d\n", g_checked, g_falseReject);
    }

    std::printf("\n==== 汇总: 检查 %lld 条, 误报无解 %d 条, 采样漏网 %d 条 ====\n", g_checked,
                g_falseReject, g_falseAccept);
    for (const Finding &f : g_findings) {
        std::printf("[误报] %s scene=%d %s  need(%s) refuse(%s) atLeast %d/(%s) atMost %d/(%s)\n"
                    "        校验器: %s\n        1.2 实测: 20000 个种子里 %d 个有解\n",
                    f.kind.toUtf8().constData(), f.scene,
                    f.range.text().toUtf8().constData(),
                    abbrs(f.cond.needMask).toUtf8().constData(),
                    abbrs(f.cond.refuseMask).toUtf8().constData(),
                    f.cond.atLeast, abbrs(f.cond.atLeastMask).toUtf8().constData(),
                    f.cond.atMost, abbrs(f.cond.atMostMask).toUtf8().constData(),
                    f.verdict.toUtf8().constData(), f.refHits);
    }
    return g_falseReject > 0 ? 1 : 0;
}
