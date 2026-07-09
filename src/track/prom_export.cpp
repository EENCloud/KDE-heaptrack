#include "prom_export.h"

#include <cpptrace/cpptrace.hpp>

extern "C" {
#include <een/prom_metrics.h>
}

#include <cstdio>
#include <string>
#include <unordered_map>

namespace promExport {

namespace {

// Cap on distinct (file,line) locations that ever get their own permanent
// Prometheus series -- by-file is small/bounded (a few hundred distinct
// source files at most) so it's always exported directly; by-file+line
// could in principle see one distinct series per call site across every
// linked library, so once this cap is hit we keep aggregating internally
// but stop creating new permanent gauges/counters for it. First-come,
// not re-ranked -- simple and avoids needing a periodic export thread,
// since everything here already runs under HeapTrack::op()'s single
// global lock (no additional locking needed in this file).
constexpr size_t kMaxLocationMetrics = 1000;

struct Aggregate {
  double outstandingBytes = 0;
  PromMetric* bytesGauge = nullptr;
  PromMetric* allocCounter = nullptr;
};

struct PtrInfo {
  size_t size;
  std::string file;
  uint32_t line;
};

bool g_initialized = false;
size_t g_locationMetricCount = 0;

// address -> "file:line" (or a symbol-name / "<unknown>" fallback),
// resolved once per unique leaf return address.
std::unordered_map<uintptr_t, std::pair<std::string, uint32_t>> g_resolvedLeaf;

// ptr -> what we tracked for it, so free() knows what to subtract.
std::unordered_map<void*, PtrInfo> g_live;

// file -> aggregate (always exported; bounded by distinct source files).
std::unordered_map<std::string, Aggregate> g_byFile;

// "file:line" -> aggregate (exported up to kMaxLocationMetrics distinct keys).
std::unordered_map<std::string, Aggregate> g_byLocation;

std::pair<std::string, uint32_t> resolveLeaf(uintptr_t address) {
  auto it = g_resolvedLeaf.find(address);
  if (it != g_resolvedLeaf.end())
    return it->second;

  cpptrace::safe_object_frame safeFrame;
  cpptrace::get_safe_object_frame(address, &safeFrame);
  cpptrace::object_frame objFrame = safeFrame.resolve();

  cpptrace::object_trace objTrace;
  objTrace.frames.push_back(objFrame);
  cpptrace::stacktrace trace = objTrace.resolve();

  std::pair<std::string, uint32_t> result{"<unknown>", 0};
  if (!trace.frames.empty()) {
    const auto& frame = trace.frames[0];
    if (!frame.filename.empty() && frame.line.value_or(0) > 0) {
      result = {frame.filename, frame.line.value_or(0)};
    } else if (!frame.symbol.empty()) {
      result = {frame.symbol, 0};
    }
  }

  g_resolvedLeaf.emplace(address, result);
  return result;
}

}  // namespace

void init() {
  if (g_initialized)
    return;
  g_initialized = true;
  // Nothing to eagerly create -- gauges/counters are created lazily,
  // per-file / per-location, the first time each is seen.
}

void recordAlloc(void* ptr, size_t size, const Trace& trace) {
  if (!g_initialized)
    init();

  if (trace.size() == 0)
    return;

  auto [file, line] = resolveLeaf(reinterpret_cast<uintptr_t>(*trace.begin()));

  g_live[ptr] = PtrInfo{size, file, line};

  {
    Aggregate& agg = g_byFile[file];
    agg.outstandingBytes += static_cast<double>(size);
    if (!agg.bytesGauge) {
      std::string gaugeName = "heaptrack_leaked_bytes_by_file";
      std::string counterName = "heaptrack_alloc_count_by_file";
      agg.bytesGauge = prom_gauge_new(gaugeName.c_str(), "outstanding allocated bytes by source file");
      agg.allocCounter = prom_counter_new(counterName.c_str(), "allocation count by source file");
    }
    prom_gauge_set(agg.bytesGauge, PROM_FLAG_SORTED, agg.outstandingBytes, "file", file.c_str(), NULL);
    prom_counter_inc(agg.allocCounter, PROM_FLAG_SORTED, "file", file.c_str(), NULL);
  }

  if (line > 0) {
    std::string key = file + ":" + std::to_string(line);
    auto it = g_byLocation.find(key);
    bool isNew = (it == g_byLocation.end());
    if (isNew && g_locationMetricCount >= kMaxLocationMetrics) {
      // Cap hit -- still track internally isn't worth it without a gauge
      // to report it through, so just skip creating this one.
      return;
    }
    Aggregate& agg = g_byLocation[key];
    agg.outstandingBytes += static_cast<double>(size);
    if (!agg.bytesGauge) {
      agg.bytesGauge = prom_gauge_new("heaptrack_leaked_bytes_by_location",
                                       "outstanding allocated bytes by source file+line");
      agg.allocCounter = prom_counter_new("heaptrack_alloc_count_by_location",
                                          "allocation count by source file+line");
      ++g_locationMetricCount;
    }
    std::string lineStr = std::to_string(line);
    prom_gauge_set(agg.bytesGauge, PROM_FLAG_SORTED, agg.outstandingBytes,
                   "file", file.c_str(), "line", lineStr.c_str(), NULL);
    prom_counter_inc(agg.allocCounter, PROM_FLAG_SORTED,
                     "file", file.c_str(), "line", lineStr.c_str(), NULL);
  }
}

void recordFree(void* ptr) {
  auto it = g_live.find(ptr);
  if (it == g_live.end())
    return;

  const PtrInfo& info = it->second;

  auto fileIt = g_byFile.find(info.file);
  if (fileIt != g_byFile.end()) {
    fileIt->second.outstandingBytes -= static_cast<double>(info.size);
    if (fileIt->second.bytesGauge)
      prom_gauge_set(fileIt->second.bytesGauge, PROM_FLAG_SORTED, fileIt->second.outstandingBytes,
                     "file", info.file.c_str(), NULL);
  }

  if (info.line > 0) {
    std::string key = info.file + ":" + std::to_string(info.line);
    auto locIt = g_byLocation.find(key);
    if (locIt != g_byLocation.end()) {
      locIt->second.outstandingBytes -= static_cast<double>(info.size);
      if (locIt->second.bytesGauge) {
        std::string lineStr = std::to_string(info.line);
        prom_gauge_set(locIt->second.bytesGauge, PROM_FLAG_SORTED, locIt->second.outstandingBytes,
                       "file", info.file.c_str(), "line", lineStr.c_str(), NULL);
      }
    }
  }

  g_live.erase(it);
}

}  // namespace promExport
