/*
    SPDX-FileCopyrightText: 2014-2017 Milian Wolff <mail@milianw.de>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

/**
 * @file libheaptrack.cpp
 *
 * @brief Feed heap allocation events into the live Prometheus allocation model.
 *
 * This EEN fork keeps only the live per-callsite Prometheus leak metrics (see
 * PromTracker). Upstream heaptrack's offline trace-file machinery -- the
 * LineWriter, the TraceTree, the periodic timestamp/RSS timer thread, and the
 * single global lock they all required -- is gone. Every hook now does nothing
 * but hand the event to s_model, whose intake queue is lock-free, so there is
 * no global lock to contend on and, by construction, none of the collator-
 * thread-vs-lock deadlocks that machinery was prone to.
 */

#include "libheaptrack.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "PromTracker.hpp"
#include "trace.h"
#include "util/config.h"

namespace {

// The live allocation model -- the entire point of this fork. A plain global,
// constructed at library-load time; its recordAlloc/recordFree are safe to call
// before this constructor has run or after its destructor has begun (both guard
// a zero-initialized constructed_ flag), which the malloc hook relies on during
// startup and teardown.
een::PromTracker<> s_model;

// When paused (heaptrack_pause / heaptrack_stop), every hook short-circuits and
// the model sees nothing -- resume clears it. Cheap atomic check on the hot path.
std::atomic<bool> s_paused {false};

bool paused()
{
    return s_paused.load(std::memory_order_relaxed);
}

}

extern "C" {

void heaptrack_init(const char* /*outputFileName*/, heaptrack_callback_t initBeforeCallback,
                    heaptrack_callback_initialized_t /*initAfterCallback*/, heaptrack_callback_t /*stopCallback*/)
{
    // Resolve the real allocation functions (initBeforeCallback) and prime the
    // unwinder once, before any hook fires. There is no output file to open and
    // no timer thread to start -- the model is a load-time global.
    if (initBeforeCallback) {
        initBeforeCallback();
    }

    static const bool once = [] {
        Trace::setup();
        return true;
    }();
    (void)once;
}

void heaptrack_stop()
{
    // No trace file to flush/close; just stop feeding the model. Its collator
    // threads are torn down at static destruction.
    s_paused.store(true, std::memory_order_relaxed);
}

void heaptrack_pause()
{
    s_paused.store(true, std::memory_order_relaxed);
}

void heaptrack_resume()
{
    s_paused.store(false, std::memory_order_relaxed);
}

void heaptrack_malloc(void* ptr, size_t size)
{
    if (paused() || !ptr) {
        return;
    }
    if (s_model.isTrackerThread()) {
        // One of our own collator threads allocating for its own bookkeeping.
        // Book it as intrinsic tracking overhead (an atomic bump) and return
        // WITHOUT unwinding. Running trace.fill() here for our own allocations
        // is what wedged the resolver thread: it re-enters this hook on every
        // hash-map node it allocates and paid a full libunwind stack walk each
        // time, only to be dropped. Must stay allocation-free here (atomics
        // only) so it can't recurse back into this hook.
        s_model.recordTrackerAlloc(size);
        return;
    }

    Trace trace;
    trace.fill(2 + HEAPTRACK_DEBUG_BUILD * 2);
    s_model.recordAlloc(ptr, size, trace);
}

void heaptrack_free(void* ptr)
{
    if (paused() || !ptr) {
        return;
    }
    if (s_model.isTrackerThread()) {
        // Our own thread freeing; nothing to record here (we deliberately keep
        // no ptr->size map for tracker allocations -- that map's own node
        // allocations are exactly what would re-enter this hook).
        return;
    }
    s_model.recordFree(ptr);
}

void heaptrack_realloc(void* ptr_in, size_t size, void* ptr_out)
{
    if (paused() || !ptr_out) {
        return;
    }
    if (s_model.isTrackerThread()) {
        s_model.recordTrackerAlloc(size);
        return;
    }

    // A realloc is a free of the old pointer plus an allocation of the new one.
    Trace trace;
    trace.fill(2 + HEAPTRACK_DEBUG_BUILD * 3);
    if (ptr_in) {
        s_model.recordFree(ptr_in);
    }
    s_model.recordAlloc(ptr_out, size, trace);
}

void heaptrack_realloc2(uintptr_t ptr_in, size_t size, uintptr_t ptr_out)
{
    heaptrack_realloc(reinterpret_cast<void*>(ptr_in), size, reinterpret_cast<void*>(ptr_out));
}

void heaptrack_invalidate_module_cache(heaptrack_invalidate_module_cache_callback /*callback*/)
{
    // Module/section addresses were only needed to write the offline trace's
    // module lines. The model resolves addresses itself via cpptrace, so there
    // is nothing to invalidate.
}

void heaptrack_warning(heaptrack_warning_callback_t /*callback*/)
{
    // Warnings were written into the trace stream; there is no stream now.
}
}
