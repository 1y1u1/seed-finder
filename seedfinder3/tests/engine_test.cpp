#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>

#include <thread>

#include "seedengine.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    // ---- 1. 交叉校验 ----
    const QString err = sf::Searcher::selfCheck();
    if (err.isEmpty()) {
        out << "selfCheck: OK" << endl;
    } else {
        out << "selfCheck: FAILED -> " << err << endl;
        return 1;
    }

    // ---- 2. 一致性: evaluateSeed 与 viewDetail 对同一批种子结果一致 ----
    {
        QVector<sf::RuleBand> bands;
        sf::RuleBand b;
        b.beginFlag = 0;
        b.endFlag = 50;
        b.cond.needMask = (1u << Z_GIGA);
        b.cond.refuseMask = (1u << Z_ZOMBONI);
        b.cond.atLeastMask = (1u << Z_CONE) | (1u << Z_BUCKET);
        b.cond.atLeast = 1;
        b.cond.sanitize();
        bands << b;

        sf::RuleTable table;
        table.build(bands, 0, 25);
        const sf::WaveOracle oracle(2);
        const sf::RuleTable globals;

        int bad = 0;
        quint32 seed = 0x12345678u;
        for (int k = 0; k < 300; ++k, seed += 77771u) {
            const sf::SeedEval ev = sf::evaluateSeed(seed, 1, 13, oracle, table, globals);
            const std::vector<sf::DetailRow> rows = sf::viewDetail(seed, 1, 13, 2, 0, 25, table, sf::RuleTable());
            int cnt = 0;
            for (size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].hasRule && rows[i].matched) ++cnt;
            }
            if (cnt != ev.matched) ++bad;
        }
        out << "evaluate vs detail: " << (bad == 0 ? "OK" : "MISMATCH") << " (bad=" << bad << ")" << endl;
        if (bad) return 1;
    }

    // ---- 3. 性能: 100 万种子 x 25 波 ----
    {
        sf::SearchSpec spec;
        spec.uid = 1;
        spec.mode = 13;
        spec.scene = 2;
        spec.beginFlag = 0;
        spec.endFlag = 50;
        spec.seedBegin = 0;
        spec.span = 300000;
        spec.outputSize = 20;

        sf::RuleBand b;
        b.beginFlag = 0;
        b.endFlag = 50;
        b.cond.needMask = 0;
        b.cond.refuseMask = (1u << Z_GIGA);
        b.cond.sanitize();
        spec.bands << b;

        // 单线程基准
        spec.threads = 1;
        sf::Searcher s;
        QElapsedTimer t;
        t.start();
        const sf::SearchResult r1 = s.run(spec, nullptr, nullptr);
        const qint64 ms1 = t.elapsed();

        // 多线程
        spec.threads = 0;
        t.restart();
        const sf::SearchResult r8 = s.run(spec, nullptr, nullptr);
        const qint64 ms8 = t.elapsed();

        bool same = (r1.buckets.size() == r8.buckets.size());
        if (same) {
            for (int i = 0; i < r1.buckets.size(); ++i) {
                if (r1.buckets[i].matched != r8.buckets[i].matched
                    || r1.buckets[i].total != r8.buckets[i].total
                    || r1.buckets[i].examples != r8.buckets[i].examples) {
                    same = false;
                    break;
                }
            }
        }
        out << "determinism(1 vs N threads): " << (same ? "OK" : "MISMATCH") << endl;
        out << "perf: 1 thread " << ms1 << "ms, auto(" << std::thread::hardware_concurrency()
            << ") " << ms8 << "ms" << endl;
        out << "  full=" << r8.fullCount() << " active=" << r8.activeWaves
            << " buckets=" << r8.buckets.size() << endl;
        for (int i = 0; i < r8.buckets.size() && i < 3; ++i) {
            const sf::CountBucket &cb = r8.buckets[i];
            out << "  matched=" << cb.matched << " total=" << cb.total;
            if (!cb.examples.isEmpty()) {
                out << " e.g. " << QString::number(cb.examples[0], 16).toUpper();
            }
            out << endl;
        }
        if (!same) return 1;
    }

    // ---- 4. 100 万种子满区间性能 ----
    {
        sf::SearchSpec spec;
        spec.uid = 1;
        spec.mode = 13;
        spec.scene = 2;
        spec.beginFlag = 0;
        spec.endFlag = 50;
        spec.seedBegin = 0;
        spec.span = 1000000;
        spec.outputSize = 20;
        sf::RuleBand b;
        b.beginFlag = 0;
        b.endFlag = 50;
        b.cond.refuseMask = (1u << Z_GIGA);
        b.cond.sanitize();
        spec.bands << b;
        sf::Searcher s;
        QElapsedTimer t;
        t.start();
        const sf::SearchResult r = s.run(spec, nullptr, nullptr);
        out << "1M seeds x 25 waves: " << (t.elapsed() / 1000.0) << "s, full=" << r.fullCount() << endl;
    }

    out << "ALL OK" << endl;
    return 0;
}
