#include "memory/scan_session.hpp"
#include "memory/value_format.hpp"
#include <array>
#include <cstdio>
#include <cstring>

namespace app {

void ScanSession::flushDisplay()
{
    totalFound_ = raw_.size();
    display_.clear();
    const size_t show = raw_.size() < kDisplayCap ? raw_.size() : kDisplayCap;
    display_.reserve(show);
    for (size_t i = 0; i < show; ++i)
    {
        DisplayEntry e{};
        e.address = raw_[i].address;
        // Scalars show their fixed width; String/Pattern show only as many bytes
        // as the searched needle (capped at the 8-byte snapshot), like CE -- not
        // the whole snapshot.
        size_t width = mem::value_size(lastVt_);
        if (width == 0) width = lastNeedleLen_;
        if (width > sizeof(raw_[i].snapshot)) width = sizeof(raw_[i].snapshot);
        formatValue(raw_[i].snapshot, width, lastVt_, e.value, sizeof(e.value), lastUtf16_);
        snprintf(e.prev, sizeof(e.prev), "?");
        display_.push_back(e);
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

void ScanSession::poll()
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
    flushDisplay();
}

void ScanSession::waitIdle()
{
    cancel_ = true; // teardown: don't block on a long scan
    while (running_) Sleep(10);
}

} // namespace app
