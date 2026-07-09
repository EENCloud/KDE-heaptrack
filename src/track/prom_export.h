/*
    Adjunct to heaptrack: exports live per-file / per-file+line outstanding
    allocation metrics via libeen's Prometheus API, so a preloaded process
    can be watched for leaks continuously instead of via an offline trace.

    Always called from within HeapTrack::op(), which already holds a single
    process-wide mutex around every call -- no additional locking needed
    here.
*/
#ifndef PROM_EXPORT_H
#define PROM_EXPORT_H

#include "trace.h"

#include <cstddef>

namespace promExport {

// Idempotent; safe to call multiple times (e.g. lazily on first use).
void init();

// trace's leaf frames are resolved (once per unique leaf address, cached)
// to file:line and aggregated by file and by file+line.
void recordAlloc(void* ptr, size_t size, const Trace& trace);
void recordFree(void* ptr);

}  // namespace promExport

#endif  // PROM_EXPORT_H
