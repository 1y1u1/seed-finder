// C++ interface of the GPU search core (no CUDA headers, so other .cpp files
// can include it). Deliberately Qt-free: nvcc reads this header, so keeping it
// plain keeps nvcc happy.
//
// NOTE: this file must stay pure ASCII. nvcc's cudafe++ reads sources in the
// system code page (936 here), and a UTF-8 Chinese comment can end with a byte
// that GBK decodes to 0x5C (backslash), which swallows the following line.
#pragma once

#include <string>
#include <vector>

// Description of one wave level. Conditions of that level live in the CondG
// array, laid out flat.
struct LevelDesc
{
    int level = 0;                  // wave level (1 based)
    unsigned tableStride = 0;       // = 101 * (level - levelFirst)
    unsigned rankOff = 0;           // first rank condition in conds
    unsigned rankCnt = 0;
    unsigned hardOff = 0;           // first hard condition in conds
    unsigned hardCnt = 0;
};

// Same shape as sf::WaveCondition, but POD so it can be uploaded directly.
struct CondG
{
    unsigned needMask = 0;
    unsigned needTotal = 0;
    unsigned refuseMask = 0;
    unsigned atLeastMask = 0;
    unsigned atLeast = 0;
    unsigned atMostMask = 0;
    unsigned atMost = 0;
};

// GPU availability; callers should fall back to the CPU path when this is 0.
int gpuDeviceCount();
void gpuDeviceInfo(char *name, int nameLen, int *sms, double *gbps);

// Tuning knobs. blockSize must be a multiple of 32; chainsPerThread interleaves
// several independent MT chains in one thread so that the serial dependency of
// each chain is hidden by the others. strided != 0 gives thread t the chains
// t, t+T, t+2T, ... which keeps the table stores coalesced across threads.
void gpuSetTuning(int blockSize, int chainsPerThread, int strided = 1);
void gpuGetTuning(int *blockSize, int *chainsPerThread, int *strided);

class GpuSearcher
{
public:
    GpuSearcher();
    ~GpuSearcher();

    // Upload the condition tables. Called once per search; scan() then never
    // touches host memory except for the result buffer.
    // lvFirst is the first level that actually needs a card table: levels may
    // contain level <= 0 entries whose bits are constant, and those must not be
    // used as the table base.
    bool prepare(const std::vector<LevelDesc> &levels,
                 const std::vector<CondG> &conds,
                 unsigned allow, unsigned off32, int lvFirst, std::string *error);

    // Scan a contiguous seed range. out[i] == 0 means the seed was killed by a
    // hard condition, otherwise it holds (matched waves + 1).
    bool scan(unsigned seedBegin, unsigned seedCount,
              std::vector<unsigned short> &out, std::string *error);

    // Split of the last scan() in milliseconds, for tuning.
    void lastTiming(double *buildMs, double *evalMs) const;

private:
    struct Impl;
    Impl *m_impl = nullptr;
};

// Verification helper: build N card tables on the GPU, 8 words per table
// (d1[2..9]).
bool gpuDumpTables(unsigned r0, unsigned allow, unsigned count,
                   std::vector<unsigned> &out, std::string *error);
