#include <sys/cdefs.h>

#include <fstream>

#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "debug/PrcpDump.hh"
#include "debug/SNN.hh"
#include "snn.hh"

using namespace boost;

SNN *SNNParams::create()
{
    return new SNN(this);
}

void SNN::uncondBranch(ThreadID tid, Addr pc, void *&bp_history) {
    bp_history = new BPHistory(globalHistory[tid],
            emptyLocalHistory, InvalidTableIndex,
            true, InvalidPredictionID, table.front().theta + 1);
    updateGHR(tid, true);
}

void SNN::btbUpdate(
        ThreadID tid, Addr branch_addr, void *&bp_history) {
    globalHistory[tid][0] = false;
    auto index = computeIndex(branch_addr);
    table[index].localHistory[0] = false;
}


void SNN::squash(ThreadID tid, void *bp_history) {
    auto history = static_cast<BPHistory *>(bp_history);
    globalHistory[tid] = history->globalHistory;

    if (history->tableIndex != InvalidTableIndex) {
        table[history->tableIndex].localHistory = history->localHistory;
    }

    delete history;
}

unsigned SNN::getGHR(ThreadID tid, void *bp_history) const {
    return 0;
}

uint32_t SNN::computeIndex(Addr addr) {
    return static_cast<uint32_t>((addr >> 2) % tableSize);
}

void SNN::updateGHR(ThreadID tid, bool taken) {
    globalHistory[tid] <<= 1;
    globalHistory[tid][0] = taken;
}

SNN::SNN(const SNNParams *params)
        : BPredUnit(params),
          globalHistoryLen(params->denseGlobalHistoryLen +
                           params->sparseGHNSegs * params->sparseGHSegLen),
          tableSize(params->tableSize),
          emptyLocalHistory(1),
          globalHistory(params->numThreads,
                  dynamic_bitset<>(globalHistoryLen)),
          table(tableSize, Neuron(params))
{
    uint32_t count = 0;
    for (auto &entry: table) {
        if (count++ == probeIndex) {
            entry.probing = true;
        }
    }
}

bool SNN::lookup(ThreadID tid, Addr branch_addr, void *&bp_bistory) {
    tryDump();

    uint32_t index = computeIndex(branch_addr);
    dynamic_bitset<> &ghr = globalHistory[tid];
    Neuron &entry = table.at(index);

    if (entry.probing && Debug::SNN) {
        DPRINTF(SNN, "Inst[0x%llx] with Pred[%llu]\n",
                branch_addr, predictionID);
        std::cout << "Using local: " << entry.localHistory
                  << ", global: " << globalHistory[tid] << std::endl;
    }


    int32_t prediction_val = entry.predict(ghr);
    bool result = prediction_val >= 0;
    bp_bistory = new BPHistory(ghr, entry.localHistory, index,
            result, predictionID++, prediction_val);

    updateGHR(tid, result);

    return result;
}


void SNN::update(ThreadID tid, Addr branch_addr, bool taken,
        void *bp_history, bool squashed) {
    auto history = static_cast<BPHistory *>(bp_history);

    auto index = computeIndex(branch_addr);
    Neuron &entry = table.at(index);
    assert(entry.valid);

    if (squashed) {
        globalHistory[tid] = history->globalHistory << 1;
        globalHistory[tid][0] = taken;
        if (history->tableIndex != InvalidTableIndex) {
            entry.localHistory = history->localHistory << 1;
            entry.localHistory[0] = taken;
        }
        return;
    }

    if (entry.probing && Debug::SNN) {
        DPRINTF(SNN, "Inst[0x%llx] with Pred[%llu], ",
                branch_addr, history->predictionID);
        DPRINTFR(SNN, "correct:%d\n", history->predTaken == taken);
    }

    entry.fit(history, taken);

    if (entry.probing) {
        DPRINTF(SNN, "New prediction:\n");
    }
    entry.predict(history->globalHistory);
//    if (entry.probing && Debug::SNN) {
//        std::cout << "New local: " << entry.localHistory << std::endl;
//    }

    delete history;
}

void SNN::dumpParameters() const{
    int count = 0;
    for (const auto &n: table) {
        DPRINTFR(PrcpDump, "%d,", count++);
        n.dump();
        DPRINTFR(PrcpDump, "\n");
    }
}

void SNN::tryDump() {
    if (__glibc_unlikely(nextDumpTick == 0)) {
        nextDumpTick = curTick() + 500*10000;
    }
    if (__glibc_unlikely(curTick() >= nextDumpTick)) {
        DPRINTFR(PrcpDump, "==dump==\n");
        dumpParameters();
        nextDumpTick += 500*10000;
    }
}


int32_t SNN::Neuron::predict(boost::dynamic_bitset<> &ghr)
{
    int32_t sum = denseWeights.back().read(); // bias
    for (int i = 0; i < denseGHLen; i++) {
        sum += b2s(ghr[i]) * denseWeights[i].read();
    }
    for (int i = 0; i < sparseGHSegLen; i++) {
        uint32_t ptr = activeStart + i;
        sum += b2s(ghr[ptr]) * activeWeights[i].read();
    }
    for (int i = 0; i < sparseGHNSegs; i++) {
        uint32_t ptr = sparseSegs[i].ptr;
        sum += b2s(ghr[ptr]) * sparseSegs[i].weight.read();
        // if invalid, then weight is always 0
    }

    if (probing) {
        DPRINTFR(SNN, "sum: %d\n", sum);
    }
    return sum;
}

void SNN::Neuron::fit(BPHistory *bp_history, bool taken) {


    //<editor-fold desc="trivial">
    if (taken == bp_history->predTaken &&
        abs(bp_history->predictionValue) > theta) {
        return;
    }
    if (probing) {
        DPRINTFR(SNN, "Old prediction: %d, theta: %d\n",
                 bp_history->predictionValue, theta);
    }

    if (taken) {
        denseWeights.back().increment();
    } else {
        denseWeights.back().decrement();
    }
    //</editor-fold>

    const auto &ghr = bp_history->globalHistory;

    for (int i = 0; i < denseGHLen; i++) {
        denseWeights[i].add(b2s(taken) * b2s(ghr[i]));
    }
    for (int i = 0; i < sparseGHNSegs; i++) {
        uint32_t ptr = sparseSegs[i].ptr;
        sparseSegs[i].weight.add(
                b2s(taken) * b2s(ghr[ptr]) * sparseSegs[i].valid);
    }
    for (int i = 0; i < sparseGHSegLen; i++) {
        uint32_t ptr = activeStart + i;
        activeWeights[i].add(b2s(taken) * b2s(ghr[ptr]));
    }
    activeTime ++;
    if (activeTime >= activeTerm) {
        activeTime = 0;
        auto &seg_to_update = sparseSegs[activeStart / sparseGHSegLen];
        uint32_t max_index = 0;
        int max = abs(activeWeights.front().read());
        for (uint32_t i = 0; i < sparseGHSegLen; i++) {
            const auto & counter = activeWeights[i];
            if (abs(counter.read()) > max) {
                max = abs(counter.read());
                max_index = i;
            }
        }
        if (!seg_to_update.valid) {
            seg_to_update.valid = true;
            seg_to_update.weight.add(activeWeights[max_index].read());
            seg_to_update.ptr = max_index;
            theta += 2;

        } else {
            if (seg_to_update.ptr == max_index) {
                // do not update
            } else {
                seg_to_update.ptr = max_index;
                seg_to_update.weight.reset();
                seg_to_update.weight.add(activeWeights[max_index].read());
            }
        }

        if (activeStart / sparseGHSegLen != sparseGHNSegs - 1) {
            activeStart += sparseGHSegLen;
        } else {
            activeStart = denseGHLen;
        }

        for (auto & counter: activeWeights) {
            counter.reset();
        }
    }
}

SNN::Neuron::Neuron(const SNNParams *params)
        : denseGHLen(params->denseGlobalHistoryLen),
          sparseGHSegLen(params->sparseGHSegLen),
          sparseGHNSegs(params->sparseGHNSegs),
          localHistory(params->localHistoryLen),
          denseWeights(denseGHLen + 1, SignedSatCounter(params->ctrBits, 0)),
          activeStart(denseGHLen),
          activeWeights(sparseGHSegLen, SignedSatCounter(params->ctrBits, 0)),
          activeTerm(params->activeTerm),
          activeTime(0),
          sparseSegs(sparseGHNSegs,
                  {false, 0, SignedSatCounter(params->ctrBits, 0)}),
          theta(static_cast<int32_t>(
                  1.93 * (denseGHLen+sparseGHSegLen) + 14.0))
          //               dense GH   active GH
{
}

int SNN::Neuron::b2s(bool taken) {
    // 1 -> 1; 0 -> -1
    return (taken << 1) - 1;
}

void SNN::Neuron::dump() const{
    for (const auto &w: denseWeights) {
        DPRINTFR(PrcpDump, "%d,", w.read());
    }
}

