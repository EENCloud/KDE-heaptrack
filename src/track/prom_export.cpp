#include "prom_export.h"
#include "PromTracker.hpp"
#include <een/prom_metrics.h>
#include <een++/core.h>

using namespace een;

namespace promExport {

namespace {

PromTracker<> Tracker;

} // namespace

// Called exactly once, from heaptrack_preload.cpp's hooks::init() bootstrap.
// Tracker is already fully constructed by this point (a plain global,
// built at load time), so there's nothing left for this function to do --
// kept only because heaptrack_preload.cpp already calls it as its bootstrap
// point, and removing the export would mean touching that call site too.
void init() {
}

void recordAlloc(void* ptr, size_t size, const Trace& trace) {
  Tracker.recordAlloc(ptr, size, trace);
}

void recordFree(void* ptr) {
  Tracker.recordFree(ptr);
}

}  // namespace promExport
