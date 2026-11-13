#pragma once

#include <QObject>
#include <atomic>

#include "seedengine.h"

// 把搜索引擎放到后台线程里跑, 通过信号回传进度与结果
class SearchWorker : public QObject
{
    Q_OBJECT

public:
    explicit SearchWorker(const sf::SearchSpec &spec, QObject *parent = nullptr);

    void requestStop() { m_cancel.store(true, std::memory_order_relaxed); }
    // 暂停/继续: 只让工作线程在 chunk 边界原地等待, 不销毁线程也不丢进度
    void setPaused(bool on) { m_pause.store(on, std::memory_order_relaxed); }

public slots:
    void run();

signals:
    void progress(int percent, quint64 scanned, quint64 qualified);
    void finished(const sf::SearchResult &result);

private:
    sf::SearchSpec m_spec;
    std::atomic<bool> m_cancel{ false };
    std::atomic<bool> m_pause{ false };
};

Q_DECLARE_METATYPE(sf::SearchResult)
