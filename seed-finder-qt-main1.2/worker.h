#pragma once

#include <mutex>
#include <string>
#include <vector>
#include <iostream>
#include <QObject>
#include <future>

const long long inv = 2083697005;
const long long SEED_TOTAL = 0xFFFFFFFF;

// 各场景允许出现的僵尸位掩码 (与 d_page::allow_base 一致, bit i = 第 i 种僵尸)
// 场景索引: 0=DE 1=NE 2=PE/FE 3=FE 4=RE/ME 5=ME 6=MGE/AQE 7=AQE
inline int scene_allow_mask(int scene)
{
    static const int allow_base[8] = {
        0x0f0ffaff, 0x0f0ff8ff, 0x0f0fffff, 0x0f0fffff,
        0x0f0fda7f, 0x0f0fda7f, 0x0f0ffaff, 0x0f0ffaff
    };
    return allow_base[scene & 7];
}

enum zombie {
    REGULAR,
    CONE,
    POLE,
    BUCKET,
    NEWS,
    DOOR,
    FOOTBALL,
    DANCE,
    SNORKEL,
    ZOMBONI,
    DOLPHIN,
    JACK,
    BALLOON,
    DIGGER,
    POGO,
    BUNGEE,
    LADDER,
    CATAPULT,
    GARG,
    GIGA
};

struct SeedInfo {
    uint16_t count = 0;                 // 符合标准的关卡出现了多少次
    std::vector<uint32_t> seeds;        // 符合条件的出怪数
    uint64_t seed_count = 0;            // 总共符合条件的种子数

    SeedInfo(uint16_t c, const std::vector<uint32_t> &s, uint64_t sc) : count(c), seeds(s), seed_count(sc) {}

    bool operator<(const SeedInfo &other) const
    {
        return (count < other.count);
    }
};


struct task {
    uint32_t begin;
    uint32_t end;
    task(uint32_t b, uint32_t e) : begin(b), end(e) {}
};


class SeedCalc : public QObject
{
    Q_OBJECT
public:
    std::vector<std::array<bool, 20>> view_detail();
    SeedCalc(int uid, int mode, int scene, int level_start, int level_end, uint32_t seed_start,
             uint64_t seed_end, int need_mask, int refuse_mask, int atleast_mask, int atmost_mask,
             int n_atleast, int n_atmost,
             int output_size, QObject *parent = nullptr)
        : QObject(parent)
    {
        this->offset = uid + mode;
        this->scene = scene;
        this->level_start = level_start;
        this->level_end = level_end;
        this->seed_start = seed_start;
        this->seed_end = seed_end;
        this->need_mask = need_mask;
        this->refuse_mask = refuse_mask;
        this->atleast_mask = atleast_mask;
        this->atmost_mask = atmost_mask;
        this->n_atleast = n_atleast;
        this->n_atmost = n_atmost;
        this->output_size = output_size;

        this->cur = (offset + seed_start) * inv;
        uint64_t seed_span = seed_end - seed_start;
        this->rest = seed_span;
        this->block = seed_span / 100;
        this->nxt_disp = rest - block;
        this->progress = 0;
        this->stop_flag = false;
    }
    SeedCalc(int uid, int mode, int scene, int level_start, int level_end, uint32_t seed, QObject *parent = nullptr)
        : QObject(parent)
    {
        this->offset = uid + mode;
        this->scene = scene;
        this->level_start = level_start;
        this->level_end = level_end;
        this->seed = seed;
    }
    void stop()
    {
        mtx.lock();
        stop_flag = true;
        mtx.unlock();
    }
signals:
    void progress_updated(int val);
    void output_result(std::vector<SeedInfo> result);
    void show_debug_info(QString str);
public slots:
    void calc();
private:
    void add_seed_info(uint16_t *seed_count_lst, uint32_t size, uint32_t r_val_max);
    int offset, level_start, level_end, scene, need_mask, refuse_mask, atleast_mask, atmost_mask,
        n_atleast, n_atmost, output_size, seed, progress = 0;
    uint32_t seed_start, cur = 0;
    uint32_t block = 1;
    uint64_t seed_end = 0;
    long long rest, nxt_disp = 0;
    bool stop_flag = false;
    std::vector<SeedInfo> result;
    std::mutex mtx;
    task get();
    void work();
};
