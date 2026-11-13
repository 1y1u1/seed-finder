// Regression harness: runs Searcher::run on fixed specs and prints a deterministic digest,
// so results can be compared byte-for-byte before/after refactoring.
#include "../seedfinder3/seedengine.h"

#include <cstdio>

namespace {

void dump(const char *tag, const sf::SearchResult &r)
{
    std::printf("[%s] scanned=%llu qualified=%llu active=%d waves=%d..%d stopped=%d enough=%d err=%s\n",
                tag, (unsigned long long)r.scanned, (unsigned long long)r.qualified, r.activeWaves,
                r.waveBegin, r.waveEnd, int(r.stopped), int(r.enoughFound),
                r.error.isEmpty() ? "-" : r.error.toUtf8().constData());
    for (int i = 0; i < r.buckets.size(); ++i) {
        const sf::CountBucket &b = r.buckets[i];
        std::printf("   matched=%-3d total=%-8llu examples=", b.matched,
                    (unsigned long long)b.total);
        for (int k = 0; k < b.examples.size(); ++k)
            std::printf("%s%08X", k ? "," : "", b.examples[k]);
        std::printf("\n");
    }
}

sf::RuleBand mkBand(int b, int e, quint32 need, int needTotal)
{
    sf::RuleBand rb;
    rb.beginFlag = b;
    rb.endFlag = e;
    rb.cond.needMask = need;
    rb.cond.needTotal = needTotal;
    return rb;
}

// aggregate digest that is thread-count independent
void digest(const char *tag, const sf::SearchResult &r)
{
    quint64 total = 0;
    for (int i = 0; i < r.buckets.size(); ++i) total += r.buckets[i].total;
    std::printf("[%s] scanned=%llu qualified=%llu buckets=%d total=%llu active=%d err=%s\n", tag,
                (unsigned long long)r.scanned, (unsigned long long)r.qualified, r.buckets.size(),
                (unsigned long long)total, r.activeWaves,
                r.error.isEmpty() ? "-" : r.error.toUtf8().constData());
}

} // namespace

int main()
{
    // A: single band, single thread (fully deterministic output)
    {
        sf::SearchSpec s;
        s.scene = 2;
        s.beginFlag = 10;
        s.endFlag = 12;
        s.span = 200000;
        s.outputSize = 5;
        s.autoStop = 0;
        s.threads = 1;
        s.bands.append(mkBand(10, 12, 1u << Z_GIGA, 1));
        dump("A", sf::Searcher().run(s, nullptr, nullptr));
    }

    // B: multi band + hard conditions, single thread
    {
        sf::SearchSpec s;
        s.scene = 4;
        s.beginFlag = 0;
        s.endFlag = 50;
        s.span = 120000;
        s.outputSize = 8;
        s.autoStop = 0;
        s.threads = 1;
        s.bands.append(mkBand(0, 10, 1u << Z_GIGA, 1));
        s.bands.append(mkBand(6, 20, (1u << Z_ZOMBONI) | (1u << Z_GARG), 1));
        sf::RuleBand h = mkBand(0, 50, 1u << Z_BUCKET, 1);
        h.cond.refuseMask = 1u << Z_DANCE;
        s.hard.append(h);
        dump("B", sf::Searcher().run(s, nullptr, nullptr));
    }

    // C: autoStop (single thread => deterministic stop point)
    {
        sf::SearchSpec s;
        s.scene = 2;
        s.beginFlag = 10;
        s.endFlag = 12;
        s.span = 400000;
        s.outputSize = 3;
        s.autoStop = 3;
        s.threads = 1;
        s.bands.append(mkBand(10, 12, 1u << Z_GIGA, 1));
        const sf::SearchResult r = sf::Searcher().run(s, nullptr, nullptr);
        std::printf("[C] qualified=%llu enough=%d stopped=%d scanned=%llu\n",
                    (unsigned long long)r.qualified, int(r.enoughFound), int(r.stopped),
                    (unsigned long long)r.scanned);
    }

    // D: shortest possible range (1 wave)
    {
        sf::SearchSpec s;
        s.scene = 0;
        s.beginFlag = 4;
        s.endFlag = 6;
        s.span = 200000;
        s.outputSize = 10;
        s.autoStop = 0;
        s.threads = 1;
        s.bands.append(mkBand(4, 6, 1u << Z_NEWS, 1));
        dump("D", sf::Searcher().run(s, nullptr, nullptr));
    }

    // E: multi thread, only aggregate digest (order independent)
    {
        sf::SearchSpec s;
        s.scene = 2;
        s.beginFlag = 0;
        s.endFlag = 50;
        s.span = 2000000;
        s.outputSize = 20;
        s.autoStop = 0;
        s.bands.append(mkBand(0, 20, 1u << Z_GIGA, 1));
        s.bands.append(mkBand(10, 40, (1u << Z_ZOMBONI), 1));
        digest("E", sf::Searcher().run(s, nullptr, nullptr));
    }

    // F: error paths
    {
        sf::SearchSpec s;
        s.scene = 2;
        s.beginFlag = 12;
        s.endFlag = 10;
        s.bands.append(mkBand(12, 10, 1u << Z_GIGA, 1));
        digest("F1", sf::Searcher().run(s, nullptr, nullptr));
    }
    {
        sf::SearchSpec s;
        s.scene = 2;
        s.span = 10;
        s.seedBegin = 0xFFFFFFFFu;
        s.bands.append(mkBand(10, 12, 1u << Z_GIGA, 1));
        digest("F2", sf::Searcher().run(s, nullptr, nullptr));
    }

    // G: viewDetail digest
    {
        sf::RuleTable rank, hard;
        QVector<sf::RuleBand> rb;
        rb.append(mkBand(8, 20, 1u << Z_GIGA, 1));
        sf::RuleBand hb = mkBand(0, 50, 1u << Z_CONE, 1);
        QVector<sf::RuleBand> hv;
        hv.append(hb);
        rank.build(rb, 1, 25);
        hard.build(hv, 1, 25);
        const std::vector<sf::DetailRow> rows =
            sf::viewDetail(0x1234ABCDu, 1, 13, 2, 1, 25, rank, hard);
        std::printf("[G] rows=%d\n", int(rows.size()));
        for (size_t i = 0; i < rows.size(); ++i) {
            const sf::DetailRow &d = rows[i];
            std::printf("   lv=%-3d flag=%-3d bits=%08X rule=%d/%d hard=%d/%d\n", d.level, d.flag,
                        d.bits, int(d.hasRule), int(d.matched), int(d.hasHard), int(d.hardOk));
        }
    }

    const QString sc = sf::Searcher::selfCheck();
    std::printf("selfcheck=%s\n", sc.isEmpty() ? "OK" : sc.toUtf8().constData());
    return 0;
}
