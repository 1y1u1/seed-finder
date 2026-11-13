#include "searchworker.h"

SearchWorker::SearchWorker(const sf::SearchSpec &spec, QObject *parent)
    : QObject(parent), m_spec(spec)
{
}

void SearchWorker::run()
{
    sf::Searcher searcher;
    const sf::SearchResult res = searcher.run(
        m_spec,
        [this](int pct, quint64 scanned, quint64 qualified) {
            emit progress(pct, scanned, qualified);
        },
        &m_cancel,
        &m_pause);
    emit finished(res);
}
