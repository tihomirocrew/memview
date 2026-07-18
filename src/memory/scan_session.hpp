#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include "memory/memory.hpp"

namespace app {

// Runs scans on a worker thread and exposes a capped, display-ready view.
// Each frame (main thread) call poll() to pick up finished results; start with
// firstScan()/nextScan(); reset() clears everything.
class ScanSession {
public:
    struct DisplayEntry {
        uintptr_t   address;
        std::string value;      // full value, sized to the searched length
        char        prev[32];
    };

    static constexpr size_t kDisplayCap = 2000; // max rows kept for the UI

    // Non-blocking; spawns a worker. `needle` is ignored for value-less types;
    // for String, needleLen is the byte length. For ArrayOfBytes, `mask` selects
    // which bits must match (null otherwise). `proc` must outlive the scan.
    void firstScan(const mem::Process& proc, mem::ScanType st,
                   mem::ValueType vt, const uint8_t* needle, size_t needleLen,
                   mem::TriState wf = mem::TriState::DontCare,
                   mem::TriState xf = mem::TriState::DontCare,
                   bool caseSensitive = true, bool utf16 = false,
                   const uint8_t* mask = nullptr);
    void nextScan (const mem::Process& proc, mem::ScanType st,
                   mem::ValueType vt, const uint8_t* needle, size_t needleLen,
                   bool caseSensitive = true, bool utf16 = false,
                   const uint8_t* mask = nullptr);

    void reset();

    // Stop the running scan (no-op if idle). Partial results are dropped: a
    // first scan reverts to pre-scan state, a next scan keeps the prior results.
    void cancel() { cancel_ = true; }

    // Main thread: promote staged results to the display list if a scan finished.
    // Needs `proc` to re-read long string values the 8-byte snapshot can't hold.
    void poll(const mem::Process& proc);

    // Block until any running scan finishes (call before teardown).
    void waitIdle();

    bool   running()       const { return running_; }
    bool   firstScanDone() const { return firstScanDone_; }
    size_t totalFound()    const { return totalFound_; }
    size_t lastNeedleLen() const { return lastNeedleLen_; }
    bool   capped()        const { return totalFound_ > kDisplayCap; }

    const std::vector<DisplayEntry>& results() const { return display_; }

private:
    void flushDisplay(const mem::Process& proc);

    std::vector<mem::ScanResult> raw_;       // full result set (main thread)
    std::vector<DisplayEntry>    display_;    // capped view for UI
    size_t                       totalFound_    = 0;
    bool                         firstScanDone_ = false;
    mem::ValueType               lastVt_        = mem::ValueType::Int32;
    bool                         lastUtf16_     = false; // string preview decoding
    size_t                       lastNeedleLen_ = 0;     // String/Pattern display width
    bool                         runIsFirst_    = false; // current run is a first scan

    // Worker-thread handoff.
    std::thread                  thread_;
    std::mutex                   mutex_;
    std::vector<mem::ScanResult> staged_;
    std::atomic<bool>            running_   { false };
    std::atomic<bool>            done_      { false };
    std::atomic<bool>            cancel_    { false }; // set by cancel(), polled by worker
    std::atomic<bool>            cancelled_ { false }; // worker observed the cancel
};

} // namespace app
