#include "memory/scan_session.hpp"
#include "memory/value_format.hpp"
#include <array>
#include <cstdio>
#include <cstring>

namespace app {

void ScanSession::flushDisplay(const mem::Process& proc)
{
    totalFound_ = raw_.size();
    display_.clear();
    const size_t show = raw_.size() < kDisplayCap ? raw_.size() : kDisplayCap;
    display_.reserve(show);
    for (size_t i = 0; i < show; ++i)
    {
        DisplayEntry e{};
        e.address = raw_[i].address;
        // Scalars fit in the snapshot. Strings can be longer than 8 bytes, so
        // re-read them from memory at the full searched length.
        const size_t scalarW = mem::value_size(lastVt_);
        if (scalarW != 0)
        {
            e.value = formatValueStr(raw_[i].snapshot, scalarW, lastVt_, lastUtf16_);
        }
        else
        {
            const size_t width = lastNeedleLen_;
            std::vector<uint8_t> buf(width ? width : 1);
            if (proc.is_open() && width &&
                mem::read_raw(proc, raw_[i].address, buf.data(), width))
            {
                e.value = formatValueStr(buf.data(), width, lastVt_, lastUtf16_);
            }
            else
            {
                // Process gone: fall back to the snapshot's first bytes.
                const size_t sw = width < sizeof(raw_[i].snapshot)
                                ? width : sizeof(raw_[i].snapshot);
                e.value = formatValueStr(raw_[i].snapshot, sw, lastVt_, lastUtf16_);
            }
        }
        snprintf(e.prev, sizeof(e.prev), "?");
        display_.push_back(std::move(e));
    }
}

void ScanSession::firstScan(const mem::Process& proc, mem::ScanType st,
                            mem::ValueType vt, const uint8_t* needle,
                            size_t needleLen, mem::TriState wf, mem::TriState xf,
                            bool caseSensitive, bool utf16, const uint8_t* mask)
{
    if (!proc.is_open() || running_) return;

    lastVt_        = vt;
    lastUtf16_     = utf16;
    lastNeedleLen_ = needleLen;
    runIsFirst_ = true;
    cancel_     = false;
    cancelled_  = false;
    running_    = true;
    done_       = false;

    std::vector<uint8_t> nd(needle, needle + needleLen);
    std::vector<uint8_t> mk;
    if (mask) mk.assign(mask, mask + needleLen);
    const mem::Process*  p = &proc;

    thread_ = std::thread([this, p, st, vt, nd = std::move(nd),
                           mk = std::move(mk), wf, xf, caseSensitive, utf16]()
    {
        auto res = mem::scan_first(*p, st, vt, nd.data(), nd.size(), wf, xf,
                                   caseSensitive, utf16,
                                   mk.empty() ? nullptr : mk.data(), &cancel_);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            staged_ = std::move(res);
        }
        cancelled_ = cancel_.load(); // publish before done_ so poll() sees it
        done_      = true;
        running_   = false;
    });
    thread_.detach();

    firstScanDone_ = true;
}

void ScanSession::nextScan(const mem::Process& proc, mem::ScanType st,
                           mem::ValueType vt, const uint8_t* needle,
                           size_t needleLen, bool caseSensitive, bool utf16,
                           const uint8_t* mask)
{
    if (!proc.is_open() || running_ || !firstScanDone_) return;

    lastVt_        = vt;
    lastUtf16_     = utf16;
    lastNeedleLen_ = needleLen;
    runIsFirst_ = false;
    cancel_     = false;
    cancelled_  = false;
    running_    = true;
    done_       = false;

    std::vector<uint8_t> nd(needle, needle + needleLen);
    std::vector<uint8_t> mk;
    if (mask) mk.assign(mask, mask + needleLen);
    const mem::Process*  p = &proc;
    auto prev = raw_; // snapshot for the worker

    thread_ = std::thread([this, p, st, vt, nd = std::move(nd),
                           mk = std::move(mk), prev = std::move(prev),
                           caseSensitive, utf16]()
    {
        auto res = mem::scan_next(*p, prev, st, vt, nd.data(), nd.size(),
                                  caseSensitive, utf16,
                                  mk.empty() ? nullptr : mk.data(), &cancel_);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            staged_ = std::move(res);
        }
        cancelled_ = cancel_.load(); // publish before done_ so poll() sees it
        done_      = true;
        running_   = false;
    });
    thread_.detach();
}

void ScanSession::reset()
{
    if (running_) return;
    raw_.clear();
    display_.clear();
    totalFound_    = 0;
    firstScanDone_ = false;
}

void ScanSession::poll(const mem::Process& proc)
{
    if (!done_) return;
    done_ = false;

    // Cancelled: drop partial results. A first scan has no baseline, so revert
    // to pre-scan state; a next scan keeps the previous results.
    if (cancelled_)
    {
        if (runIsFirst_) firstScanDone_ = false;
        cancel_    = false;
        cancelled_ = false;
        return;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    raw_ = std::move(staged_);
    flushDisplay(proc);
}

void ScanSession::waitIdle()
{
    cancel_ = true; // teardown: don't block on a long scan
    while (running_) Sleep(10);
}

} // namespace app
