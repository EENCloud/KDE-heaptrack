#pragma once

#include "trace.h"

#include <cpptrace/cpptrace.hpp>

#include <een/prom_metrics.h>
#include <een++/core.h>

namespace een {

// SymbolQueueCapacity: capacity of the *second* queue (see symResolveQueue_)
// feeding the real-symbol resolver thread. Deliberately much smaller than
// QueueCapacity -- it holds distinct call-site addresses awaiting real DWARF
// resolution, deduplicated via submittedForResolution_ (so a hot call site
// only ever occupies one slot for its entire pending lifetime), not one
// entry per alloc/free event like the main queue, so its natural occupancy
// is orders of magnitude lower.
template <size_t MaxAttributionFrames = 100, size_t QueueCapacity = 1'000, size_t MaxLocationMetrics = 1'000, size_t SymbolQueueCapacity = 256>
class PromTracker final {
public:

#pragma pack(push, 0)
  struct QueueEntry {
    bool isAlloc = false;
    void* ptr = nullptr;
    size_t size = 0;  // only meaningful when isAlloc; processFree gets its size from live()
    std::array<uintptr_t, MaxAttributionFrames> frames{};  // only meaningful when isAlloc
    uint8_t frameCount = 0;
  };

  // Deliberately NOT a resolved source file/line -- see resolveAddress. Just
  // which binary/shared-object the address falls in, plus its ASLR-independent
  // offset within that object (stable across restarts of the same binary,
  // unlike a raw absolute in-memory address).
  struct ResolvedLoc {
    std::string objectPath;
    uintptr_t offset;
  };

  // What pickAttribution() picked: the raw frame address (needed later to
  // key resolvedSymbols_/submittedForResolution_ -- see attributionLabel)
  // paired with its already-cheaply-resolved module+offset (needed for the
  // basename+offset fallback label and the byFile_ aggregation key).
  // Non-owning pointer into resolvedLeaf_, same trivially-copyable reasoning
  // as PtrInfo below.
  struct Attribution {
    uintptr_t address;
    const ResolvedLoc* loc;
  };

  // A ptr's tracked info is a size, the raw attribution address, and a
  // *non-owning* pointer into the immortal resolvedLeaf_ cache below (see
  // resolveAddress). live_/resolvedLeaf_/isMainBinary_/byFile_/byLocation_
  // are all touched by exactly one thread (the resolver thread -- see the
  // resolver_.id() == TID::current() checks in recordAlloc/recordFree), so
  // there's no data race to guard against here, but the non-owning pointer
  // still matters: it keeps PtrInfo trivially copyable, in case this code
  // is ever called from a context where that's re-checked.
  //
  // address is stored (this struct didn't used to carry it) because
  // attributionLabel() needs the *raw* address to check resolvedSymbols_ --
  // ResolvedLoc only carries the already-cheap-resolved module+offset, not
  // the original address, and the two live in separate caches. Keeping it
  // means processFree's label computation can end up disagreeing with what
  // processAlloc used for the very same ptr, if real symbol resolution
  // completes in between the two -- see attributionLabel's and
  // byLocation_'s comments. That's accepted drift, not a bug this field is
  // trying to prevent.
  struct PtrInfo {
    size_t size;
    uintptr_t address;
    const ResolvedLoc* loc;
  };

  // One Aggregate per distinct label value (one per file, one per
  // file+line). Deliberately holds NO PromMetric* of its own -- see
  // byFileBytesGauge_/byLocationBytesGauge_'s comment for why creating a
  // PromMetric per label, rather than once per metric *name*, is a real bug
  // and not just a style choice.
  struct Aggregate {
    double outstandingBytes = 0;
    // Built once (via prom_make_label), the first time this label is seen,
    // then reused forever through the _full call variants. prom_gauge_set/
    // prom_counter_inc (the plain, non-_full forms) each build a brand new
    // GBytes label from scratch on *every single call*, repeat or not --
    // prom_metrics.h's own header explicitly documents the _full pattern for
    // exactly this reason ("if you are going to use the exact same set of
    // name/values a lot, create the label once"). Skipping this would mean
    // every drained queue entry allocates again here, no matter how
    // carefully the rest of this file avoids it.
    GBytes* label = nullptr;
  };
#pragma pack(pop)

  PromTracker() {
    // cpptrace's own docs (docs/signal-safe-tracing.md) mandate this: despite
    // get_safe_object_frame() being documented signal-safe/allocation-free in
    // steady state, its *first-ever* call in the process can trigger lazy
    // dynamic-loader binding (PLT resolution for _dl_find_object itself),
    // which internally calls regular, non-signal-safe malloc. Doing that one
    // call here -- single-threaded, before any hook is live and while a plain
    // malloc is still just a plain malloc -- absorbs that cost safely, so the
    // collator thread's first real resolveAddress() call never pays it.
    cpptrace::safe_object_frame warmup;
    cpptrace::get_safe_object_frame(reinterpret_cast<cpptrace::frame_ptr>(&PromTracker::warmup), &warmup);
  }

  ~PromTracker() {
    // First statement, before anything else tears down: any recordAlloc/
    // recordFree call still in flight on another thread (this object is
    // never explicitly stopped before destruction) sees this and no-ops
    // instead of touching a half- or fully-destructed *this.
    constructed_ = false;
  }

  // True when the calling thread is one of this model's own collator threads
  // (resolver_ / symbolResolver_ -- DWARF resolution, hash-map growth,
  // std::string construction, etc). The malloc hook (libheaptrack.cpp) checks
  // this FIRST -- before unwinding -- and routes such allocations to
  // recordTrackerAlloc instead of recordAlloc, so a collator thread never pays
  // a stack unwind for, nor re-enters tracking on, its own bookkeeping
  // allocations. (Doing the unwind first and dropping later is what wedged the
  // resolver thread in production.)
  //
  // Guarded by constructed_ so it's a safe `false` when called (via the malloc
  // hook, from any thread) before this global's constructor has run or after
  // its destructor has begun: constructed_ is zero-initialized (false) ahead
  // of dynamic init and reset to false first thing in the destructor, and the
  // Thread members it reads are only alive in between.
  bool isTrackerThread() const {
    if (!constructed_)
      return false;
    return resolver_.id() == TID::current() || symbolResolver_.id() == TID::current();
  }

  // Intrinsic tracking overhead: an allocation made by one of our own collator
  // threads (see isTrackerThread). Called straight from the malloc hook in
  // place of recordAlloc, so it MUST be allocation-free and non-blocking --
  // atomics only, no map, no queue, no unwind -- or it re-enters the hook and
  // recurses. Cumulative on purpose: we keep no ptr->size map (its node
  // allocations are exactly the recursion hazard), so this is total bytes/count
  // this fork's own threads have churned, not a current-outstanding figure.
  // Published from drainQueue.
  void recordTrackerAlloc(size_t size) {
    trackerBytes_.fetch_add(size, std::memory_order_relaxed);
    trackerAllocs_.fetch_add(1, std::memory_order_relaxed);
  }

  void recordAlloc(void* ptr, size_t size, const Trace& trace) {
    // Any call arriving during/after destruction is a no-op -- see
    // constructed_. Tracker-thread allocations never reach here: the hook
    // filters them into recordTrackerAlloc (see isTrackerThread) before this
    // is ever called.
    if (!constructed_)
      return;

    if (trace.size() == 0)
      return;

    // Called from the malloc/free hook on every allocation in every thread of
    // the watched process, so it must be cheap and must never block: copy the
    // raw frame addresses (already captured by heaptrack's own unwinder -- no
    // unwinding is done here) into a fixed QueueEntry and hand it to the
    // resolver thread through the lock-free MPSC queue. tryPush is a single
    // non-blocking attempt -- if the queue is momentarily full the entry is
    // dropped, never retried (a spin-retry would just burn a producer thread's
    // CPU once the queue is genuinely saturated). QueueCapacity is sized
    // generously to make drops rare, but "rare" is the ceiling, not "never".
    QueueEntry entry;
    entry.isAlloc = true;
    entry.ptr = ptr;
    entry.size = size;
    entry.frameCount = static_cast<uint8_t>(std::min(trace.size(), static_cast<int>(MaxAttributionFrames)));
    for (uint8_t i = 0; i < entry.frameCount; ++i)
      entry.frames[i] = reinterpret_cast<uintptr_t>(trace[i]);
    while (!queue_.tryPush(entry)) {
      if (!constructed_) return;
    }
  }

  void recordFree(void* ptr) {
    if (!constructed_)
      return;

    // Same reasoning as recordAlloc: must not block or do real work here.
    // Tracker-thread frees never reach here -- the hook filters them out.
    QueueEntry entry;
    entry.isAlloc = false;
    entry.ptr = ptr;
    while (!queue_.tryPush(entry)) {
      if (!constructed_) return;
    }
  }

private:
  // Only exists so the constructor's warmup call above has a stable function
  // pointer to resolve against -- see the constructor's comment.
  static void warmup() {}

  // HEAPTRACK_TRACKED_MODULES restricts which modules the symbol resolver
  // thread will ever spend real DWARF-resolution time/memory on (see
  // isTrackedModule/attributionLabel) -- it has zero effect on the existing
  // cheap module+offset attribution (resolveAddress/pickAttribution/
  // locLabel), which always runs regardless. Comma-separated module
  // basenames, e.g. "archiver,libstdc++.so.6,libeen.so".
  //
  // Empty/unset means "no filter, resolve everything" -- NOT "resolve
  // nothing". Chosen deliberately: this fork already made the equivalent
  // call for the tracker as a whole (see prom_export.cpp -- no runtime
  // opt-in flag, prom metrics always export, full stop), so a user who's
  // never heard of this specific env var still gets real symbols wherever
  // the resolver thread can keep up, rather than the feature silently doing
  // nothing until they discover a specific variable name. Setting this only
  // *narrows* the expensive real-resolution work to modules actually of
  // interest; it never narrows the cheap fallback.
  //
  // Parsed exactly once, via trackedModules_'s member initializer further
  // down -- i.e. during this object's own single-threaded construction,
  // before resolver_ or symbolResolver_ exist to possibly call
  // isTrackedModule() concurrently. Deliberately NOT a function-local magic
  // static read lazily on first call from attributionLabel() (which runs on
  // resolver_, reachable the moment that thread starts): this session's own
  // history (see git log on this file's prom_export.cpp predecessor -- the
  // old enabled()/HEAPTRACK_PROM_METRICS magic-static bool, and separately
  // a brief stint where the main queue itself was a magic-static
  // RotatingBuffer) already hit a real, confirmed deadlock from exactly
  // this shape once: a heap-allocating magic static, first-touched from the
  // hot alloc/free path, whose own backing allocation re-enters heaptrack's
  // malloc hook on the same thread that's still holding the compiler's
  // hidden guard variable for that same static -- every other thread's
  // first call then piles up behind that guard forever, since the one
  // thread that could finish releasing it is stuck. enabled()'s bool was
  // safe as a magic static only because a bool never allocates; a
  // std::unordered_set<std::string> does (bucket storage, plus any string
  // over SSO length), so this gets the same eager, single-threaded
  // construction treatment the main queue itself ended up needing.
  static std::unordered_set<std::string> parseTrackedModules() {
    std::unordered_set<std::string> modules;
    opt<std::string> raw = plat::getenv("HEAPTRACK_TRACKED_MODULES");
    if (!raw)
      return modules;  // unset -- empty set, see comment above: "resolve everything"
    size_t start = 0;
    while (start <= raw->size()) {
      size_t comma = raw->find(',', start);
      size_t end = (comma == std::string::npos) ? raw->size() : comma;
      if (end > start)
        modules.insert(raw->substr(start, end - start));
      if (comma == std::string::npos)
        break;
      start = comma + 1;
    }
    return modules;
  }

  // See trackedModules_'s comment for the empty-means-everything default.
  bool isTrackedModule(const std::string& objectPath) {
    if (trackedModules_.empty())
      return true;
    file::Path p{objectPath};
    return trackedModules_.contains(p.filename());
  }

  // HEAPTRACK_RESOLVE_SYMBOLS: toggles whether the symbol-resolver thread ever
  // does real DWARF resolution at all. Default-on (matches prior behavior).
  // Set to "0"/"false"/"off" to force every location label to the cheap raw
  // module+offset form (locLabel) instead of ever submitting to
  // symResolveQueue_ -- useful for measuring how much CPU/memory the
  // in-process resolver costs versus resolving addresses externally
  // afterward (e.g. via heaptrack_leaks against a local copy of the
  // watched binary + debug info).
  static bool parseResolveSymbolsEnabled() {
    opt<std::string> raw = plat::getenv("HEAPTRACK_RESOLVE_SYMBOLS");
    if (!raw)
      return true;
    return !(*raw == "0" || *raw == "false" || *raw == "off");
  }

  // Drains whatever's queued; returns whether it did any work (so the
  // caller knows whether to sleep before checking again).
  bool drainQueue() {
    // Publish the intrinsic-overhead counters once per drain cycle, from this
    // top-level point (safe: never called from inside another prom_metrics
    // call, unlike the malloc hook that maintains the atomics). See
    // recordTrackerAlloc.
    prom_gauge_set_full(trackerBytesGauge_, static_cast<double>(trackerBytes_.load(std::memory_order_relaxed)), emptyLabel());
    prom_gauge_set_full(trackerAllocsGauge_, static_cast<double>(trackerAllocs_.load(std::memory_order_relaxed)), emptyLabel());

    bool did = false;
    while (auto e = queue_.tryPop()) {
      if (e->isAlloc)
        processAlloc(*e);
      else
        processFree(*e);
      did = true;
    }
    return did;
  }

  void resolverLoop() {
    while (!stopRequested()) {
      bool did = drainQueue();
      if (!did)
        sleep(20ms);
    }
    drainQueue();
  }

  // Same shape as drainQueue/resolverLoop, deliberately: pop everything
  // currently available, sleep only when there was nothing to do. What's
  // different is cost-per-entry, not structure -- drainQueue's per-entry
  // cost is a handful of map/gauge updates (near-zero), while this one's
  // (resolveSymbol) is real DWARF parsing and can cost tens of ms. That's
  // fine specifically because this loop runs on its own thread, fully
  // decoupled from the collator's hot path -- the entire reason this second
  // thread exists.
  bool drainSymbolQueue() {
    bool did = false;
    while (auto address = symResolveQueue_.tryPop()) {
      resolveSymbol(*address);
      did = true;
    }
    return did;
  }

  void symbolResolverLoop() {
    while (!stopRequested()) {
      bool did = drainSymbolQueue();
      if (!did)
        sleep(20ms);
    }
    drainSymbolQueue();
  }

  // Last parent dir + filename (e.g. "allocator/alloc_drives.c"), not the
  // bare filename alone -- plenty of source files in this codebase share a
  // basename across different subdirectories, and the full absolute path
  // cpptrace hands back is a build-tree path (buildtrees/archiver/src/...),
  // not useful as a label. Path::parent()/Path::filename() via operator/
  // does the join; Path IS-A std::string (see file/Path.h) so this returns
  // cleanly as one.
  static std::string shortFile(const std::string& filename) {
    file::Path p{filename};
    return p.parent().filename() / p.filename();
  }

  // Combines symbol name AND file:line when both are available -- "just a
  // function name" isn't enough to actually find the call site in the
  // source tree, which is the whole point of paying for real DWARF
  // resolution over the cheap basename+offset fallback (locLabel). Falls
  // back to whichever piece cpptrace actually found (symbol alone, file:line
  // alone, or file alone), and leaves buf all-zero (empty C string) if
  // cpptrace resolved the address but found none of those (e.g. a fully
  // stripped object). Truncates to fit -- see resolvedSymbols_'s comment for
  // why a fixed array and not a std::string. Stateless; static like
  // warmup().
  static void formatSymbol(const cpptrace::stacktrace_frame& frame, std::array<char, 96>& buf) {
    if (!frame.symbol.empty() && !frame.filename.empty() && frame.line.has_value()) {
      snprintf(buf.data(), buf.size(), "%s (%s:%u)", frame.symbol.c_str(), shortFile(frame.filename).c_str(),
                static_cast<unsigned>(frame.line.value()));
    } else if (!frame.symbol.empty()) {
      snprintf(buf.data(), buf.size(), "%s", frame.symbol.c_str());
    } else if (!frame.filename.empty()) {
      if (frame.line.has_value())
        snprintf(buf.data(), buf.size(), "%s:%u", shortFile(frame.filename).c_str(), static_cast<unsigned>(frame.line.value()));
      else
        snprintf(buf.data(), buf.size(), "%s", shortFile(frame.filename).c_str());
    }
    // else: leave buf all-zero -- see caller (resolveSymbol) for how that's
    // told apart from "still pending".
  }

  // The real resolution resolveAddress() deliberately avoids (see its own
  // comment): opens/parses ELF+DWARF via libdwarf, allocates, can cost tens
  // of ms on a given object's first resolution. Safe only here, never on
  // resolver_ -- any malloc() this triggers re-enters heaptrack's hook on
  // THIS thread, which records it like any other allocation and no-ops via
  // the symbolResolver_.id() == TID::current() check in recordAlloc/
  // recordFree, same as resolver_'s own allocations already do.
  //
  // Deliberately does NOT reuse resolveAddress()/resolvedLeaf_ to get from
  // address to {object_path, offset}: that cache is documented (see
  // resolveAddress) as touched only by resolver_, with no lock, precisely
  // because it was cheap enough that a second writer was never worth
  // guarding against. Reaching into it from this thread too would make
  // that comment a lie and add a real, new data race. A second, independent
  // get_safe_object_frame() call is cheap, allocation-free, and safe to
  // call from any thread (same guarantee resolveAddress/isInMainBinary
  // already rely on) -- duplicating that one cheap call is a far better
  // trade than adding a lock around a struct that's on the hot collator
  // path just to share it with this one.
  void resolveSymbol(uintptr_t address) {
    cpptrace::safe_object_frame safeFrame;
    cpptrace::get_safe_object_frame(address, &safeFrame);

    std::array<char, 96> buf{};
    try {
      // safe_object_frame::resolve() -> object_frame is itself still just a
      // signal-unsafe-but-cheap conversion (fixed char buffers to
      // std::string) -- the real DWARF work happens in object_trace::
      // resolve() -> stacktrace below, which is why a single-frame
      // object_trace gets built just to call that.
      cpptrace::object_trace trace;
      trace.frames.push_back(safeFrame.resolve());
      cpptrace::stacktrace resolved = trace.resolve();
      if (!resolved.frames.empty())
        formatSymbol(resolved.frames[0], buf);
    } catch (...) {
      // Real parsing of an arbitrary shared object's debug info is exactly
      // the kind of complex format-parsing code that can legitimately throw
      // on malformed/unusual input -- cpptrace doesn't mark resolve()
      // noexcept, unlike its signal-safe API. This thread isn't on any path
      // recordAlloc/recordFree/resolver_ depend on, so it can't deadlock
      // anything by being slow, but an *uncaught* exception here would
      // std::terminate() the entire watched process over a Prometheus
      // label. Swallow it and leave buf empty -- handled the same as
      // "resolved but genuinely nothing found" below.
    }

    if (buf[0] == '\0')
      return;  // nothing usable -- submittedForResolution_ ensures this
                // address is never retried; see attributionLabel.

    auto guard = resolvedSymbolsMutex_.acquire();
    resolvedSymbols_[address] = buf;
  }

  void processFree(const QueueEntry& e) {
    auto it = live_.find(e.ptr);
    if (it == live_.end())
      return;
    PtrInfo info = it->second;
    live_.erase(it);

    totalBytes_ -= static_cast<double>(info.size);
    prom_gauge_set_full(totalBytesGauge_, totalBytes_, emptyLabel());

    const ResolvedLoc& loc = *info.loc;

    auto fileIt = byFile_.find(loc.objectPath);
    if (fileIt != byFile_.end()) {
      fileIt->second.outstandingBytes -= static_cast<double>(info.size);
      prom_gauge_set_full(byFileBytesGauge_, fileIt->second.outstandingBytes, fileIt->second.label);
    }

    // Independently re-derives the label from whatever attributionLabel
    // currently knows, same as processAlloc -- see PtrInfo's comment on why
    // this can (rarely, harmlessly) disagree with the label this same ptr's
    // allocation was originally counted under.
    std::string label = attributionLabel(info.address, loc);
    auto locIt = byLocation_.find(label);
    if (locIt != byLocation_.end()) {
      locIt->second.outstandingBytes -= static_cast<double>(info.size);
      prom_gauge_set_full(byLocationBytesGauge_, locIt->second.outstandingBytes, locIt->second.label);
    }
  }

  void processAlloc(const QueueEntry& e) {
    totalBytes_ += static_cast<double>(e.size);
    prom_gauge_set_full(totalBytesGauge_, totalBytes_, emptyLabel());
    prom_counter_inc_full(totalAllocCounter_, emptyLabel());

    Attribution attr = pickAttribution(e);
    const ResolvedLoc& loc = *attr.loc;
    live_[e.ptr] = PtrInfo{e.size, attr.address, attr.loc};

    auto& fileAgg = byFile_[loc.objectPath];
    fileAgg.outstandingBytes += static_cast<double>(e.size);
    if (!fileAgg.label)
      fileAgg.label = prom_make_label(PROM_FLAG_SORTED, "module", loc.objectPath.c_str(), NULL);
    prom_gauge_set_full(byFileBytesGauge_, fileAgg.outstandingBytes, fileAgg.label);
    prom_counter_inc_full(byFileAllocCounter_, fileAgg.label);

    // See attributionLabel: may return a real resolved symbol instead of
    // the plain basename+offset fallback, if the resolver thread has
    // already caught up to this address.
    std::string label = attributionLabel(attr.address, loc);
    auto it = byLocation_.find(label);
    bool isNew = (it == byLocation_.end());
    if (isNew && locationMetricCount_ >= MaxLocationMetrics) {
      // Cap hit -- still tracking internally isn't worth it without a gauge
      // to report it through, so just skip creating this one.
      return;
    }
    auto& locAgg = byLocation_[label];
    locAgg.outstandingBytes += static_cast<double>(e.size);
    if (!locAgg.label) {
      locAgg.label = prom_make_label(PROM_FLAG_SORTED, "loc", label.c_str(), NULL);
      ++locationMetricCount_;
    }
    prom_gauge_set_full(byLocationBytesGauge_, locAgg.outstandingBytes, locAgg.label);
    prom_counter_inc_full(byLocationAllocCounter_, locAgg.label);
  }

  // Resolves once per unique address and caches forever in resolvedLeaf_
  // (immortal for the same reason this whole object leaks its state --
  // worker threads in the watched process can still be allocating/freeing
  // during process teardown). Only ever called from the resolver thread, so
  // a plain map lookup/insert needs no locking.
  //
  // Deliberately does NOT do real DWARF symbol resolution (no
  // object_frame::resolve()/stacktrace resolution here). That call opens/
  // parses ELF+DWARF via libdwarf, which allocates -- and that allocation
  // could re-enter (see recordAlloc's comment), and running it inside the
  // traced process at all means paying real (possibly tens of ms on first
  // resolution) DWARF work inline in the collator's drain loop. Instead this
  // only calls get_safe_object_frame(), which is allocation-free and
  // signal-safe (confirmed: it's a pure `_dl_find_object` link-map lookup into
  // fixed-size buffers, no DWARF, no malloc -- see cpptrace's own
  // docs/signal-safe-tracing.md) and exports the raw {object_path, offset}
  // pair as an opaque label. A separate external process resolves that to
  // file:line by reading the same binary from disk -- see locLabel.
  const ResolvedLoc& resolveAddress(uintptr_t address) {
    auto it = resolvedLeaf_.find(address);
    if (it != resolvedLeaf_.end())
      return it->second;

    cpptrace::safe_object_frame safeFrame;
    cpptrace::get_safe_object_frame(address, &safeFrame);

    ResolvedLoc result{safeFrame.object_path, safeFrame.address_relative_to_object_start};
    return resolvedLeaf_.emplace(address, std::move(result)).first->second;
  }

  // Cheap check (no DWARF symbol resolution, just the object file's own
  // path) for whether an address falls inside the watched binary itself
  // rather than one of its shared libraries. Used to filter candidate
  // frames before calling resolveAddress() -- see pickAttribution.
  bool isInMainBinary(uintptr_t address) {
    auto it = isMainBinary_.find(address);
    if (it != isMainBinary_.end())
      return it->second;

    cpptrace::safe_object_frame safeFrame;
    cpptrace::get_safe_object_frame(address, &safeFrame);
    bool result = plat::modulePath() == file::Path{safeFrame.object_path};
    isMainBinary_.emplace(address, result);
    return result;
  }

  // Walks the handful of frames captured with this allocation (see
  // MaxAttributionFrames) and resolves the first one that's actually inside
  // the watched binary, rather than always taking *trace.begin() (the
  // immediate malloc/realloc/free caller) -- see QueueEntry's comment for
  // why that's usually a library's own container/buffer code, not
  // "meaningful" application code. Falls back to the original leaf frame if
  // none of the captured frames are in the main binary at all (e.g. an
  // allocation triggered entirely from within a library's own internals).
  Attribution pickAttribution(const QueueEntry& e) {
    for (uint8_t i = 0; i < e.frameCount; ++i) {
      if (isInMainBinary(e.frames[i]))
        return Attribution{e.frames[i], &resolveAddress(e.frames[i])};
    }
    return Attribution{e.frames[0], &resolveAddress(e.frames[0])};
  }

  // "loc" label: object basename + hex offset, e.g. "archiver+0x445566" -- an
  // intentionally unintelligible-to-humans, ASLR-independent identifier. A
  // separate process resolves this to file:line by reading the same binary
  // off disk (see the big comment on resolveAddress for why that resolution
  // step must never happen here).
  std::string locLabel(const ResolvedLoc& loc) {
    file::Path p{loc.objectPath};
    char hex[24];
    snprintf(hex, sizeof(hex), "0x%lx", static_cast<unsigned long>(loc.offset));
    return render(p.filename(), "+", hex);
  }

  // The symbol-aware version of locLabel: checks resolvedSymbols_ first (the
  // real symbol/file:line the *second* thread resolved, if it's landed yet)
  // and only falls back to the plain basename+offset form above when
  // nothing's there yet. Also the sole trigger point for handing an address
  // to the symbol resolver thread in the first place.
  //
  // Called from both processAlloc and processFree, each time freshly
  // re-checking resolvedSymbols_ rather than reusing whatever the matching
  // alloc decided -- so the two calls for the same ptr *can* disagree if
  // resolution completes in between (see PtrInfo's and byLocation_'s
  // comments). Deliberately not fixed: pinning the label per-ptr to
  // guarantee alloc/free agreement would mean threading a resolved-or-not
  // flag (or the label itself) through PtrInfo and never re-checking again,
  // which defeats half the point of resolving symbols live -- a long-lived
  // allocation would be stuck reporting under whatever label existed at its
  // *alloc* time forever, even long after resolution landed. A little
  // metric drift across a resolution event is the accepted cost of keeping
  // this simple (see byLocation_'s comment: two series briefly existing for
  // the same call site is expected, not a bug to engineer away).
  std::string attributionLabel(uintptr_t address, const ResolvedLoc& loc) {
    auto guard = resolvedSymbolsMutex_.acquire();

    auto it = resolvedSymbols_.find(address);
    if (it != resolvedSymbols_.end())
      // std::array<char,96> is guaranteed NUL-terminated here: resolveSymbol
      // only ever inserts via snprintf (always NUL-terminates within the
      // given size) and only when there's actually something to write (see
      // its own comment) -- a found entry is therefore never empty.
      return std::string(it->second.data());

    if (resolveSymbolsEnabled_ && submittedForResolution_.find(address) == submittedForResolution_.end() && isTrackedModule(loc.objectPath)) {
      // First time this address has reached here: hand it to the symbol
      // resolver thread. Only marked submitted on a *successful* push --
      // see symResolveQueue_'s comment for why a dropped push must NOT be
      // marked submitted (it needs to stay eligible for a later retry,
      // driven by whatever alloc/free next happens to pick this address).
      if (symResolveQueue_.tryPush(address))
        submittedForResolution_.insert(address);
    }
    return locLabel(loc);
  }

  // The no-label case (totals) still needs a cached label -- built once, an
  // empty one, and reused via the _full variants for the same reason every
  // Aggregate caches its own.
  GBytes* emptyLabel() {
    static GBytes* g = prom_make_label(PROM_FLAG_SORTED, NULL);
    return g;
  }

  std::unordered_map<uintptr_t, ResolvedLoc> resolvedLeaf_;

  // Cheap (no DWARF resolution) per-address cache of "is this frame inside
  // the watched binary itself" -- see isInMainBinary/pickAttribution.
  std::unordered_map<uintptr_t, bool> isMainBinary_;
  std::unordered_map<void*, PtrInfo> live_;
  std::unordered_map<std::string, Aggregate> byFile_;
  std::unordered_map<std::string, Aggregate> byLocation_;

  size_t locationMetricCount_{};
  double totalBytes_{};

  // ONE PromMetric* per metric *name*, shared across every distinct label
  // (every file, every file+line) -- created exactly once, eagerly, as part
  // of this object's own construction (the member initializers just below),
  // not lazily on first touch. This is NOT the same thing as one per
  // Aggregate: libeen's prom_gauge_new/prom_counter_new do not look up an
  // existing metric by name before creating one -- prom_metric_new_c()
  // (prom_metrics.c) unconditionally g_slice_new0's a fresh PromMetric and
  // g_tree_replace()'s it into the context's registry under that name,
  // which drops the tree's ref on whatever was previously registered
  // there. Call prom_gauge_new("heaptrack_leaked_bytes_by_module", ...) more
  // than once (the bug this file originally had, when gauges were created
  // lazily on first touch per distinct file) and every call after the first
  // silently evicts the previous one from the registry -- it keeps a live,
  // updatable PromMetric (our Aggregate still holds a ref), but
  // prom_metrics_render() never walks it again, so only the *last-created*
  // one ever appears in the exported text. Confirmed live: a synthetic test
  // with 3 known call sites (9230, 5000, 3000, 1230 allocations
  // respectively) showed a different PromMetric* pointer for every single
  // one and only the chronologically-last survived to render, exactly
  // matching this. Creating each of these exactly once, here, at
  // construction, sidesteps the whole class of bug.
  PromMetric* byFileBytesGauge_{prom_gauge_new("heaptrack_leaked_bytes_by_module", "outstanding allocated bytes by object/binary path (unresolved -- see locLabel)")};
  PromMetric* byFileAllocCounter_{prom_counter_new("heaptrack_alloc_count_by_module", "allocation count by object/binary path (unresolved -- see locLabel)")};
  PromMetric* byLocationBytesGauge_{prom_gauge_new("heaptrack_leaked_bytes_by_location", "outstanding allocated bytes by object+offset (unresolved -- see locLabel)")};
  PromMetric* byLocationAllocCounter_{prom_counter_new("heaptrack_alloc_count_by_location", "allocation count by object+offset (unresolved -- see locLabel)")};
  PromMetric* totalBytesGauge_{prom_gauge_new("heaptrack_leaked_bytes_total", "total outstanding allocated bytes across the whole process")};
  PromMetric* totalAllocCounter_{prom_counter_new("heaptrack_alloc_count_total", "total allocation count across the whole process")};

  // Intrinsic tracking overhead -- cumulative bytes/allocs churned by this
  // fork's own collator threads (see recordTrackerAlloc), atomically bumped
  // from the malloc hook and published from drainQueue. Gauges (set to the
  // running total) rather than counters so the value can be published straight
  // from the atomic without delta bookkeeping.
  std::atomic<size_t> trackerBytes_{0};
  std::atomic<size_t> trackerAllocs_{0};
  PromMetric* trackerBytesGauge_{prom_gauge_new("heaptrack_tracker_overhead_bytes", "cumulative bytes allocated by the tracking system's own collator threads (DWARF resolution, hash-map growth, etc), not application memory")};
  PromMetric* trackerAllocsGauge_{prom_gauge_new("heaptrack_tracker_overhead_count", "cumulative allocation count by the tracking system's own collator threads")};

  memory::RotatingBuffer<QueueEntry, QueueCapacity> queue_;

  // See parseTrackedModules()'s comment (above, next to warmup()) for the
  // full reasoning -- the magic-static deadlock hazard this sidesteps, and
  // the empty-means-everything default. Declared here, before constructed_/
  // resolver_/symbolResolver_, purely so it's fully built before either
  // thread could possibly call isTrackedModule(); nothing below it depends
  // on it in the other direction.
  const std::unordered_set<std::string> trackedModules_{parseTrackedModules()};

  // See parseResolveSymbolsEnabled comment. Declared alongside
  // trackedModules_ for the same reason: must be initialized before
  // resolver_/symbolResolver_ start (declaration order matters here).
  const bool resolveSymbolsEnabled_{parseResolveSymbolsEnabled()};

  // address -> resolved symbol text, written by symbolResolver_
  // (resolveSymbol), read by resolver_ (attributionLabel) -- unlike
  // resolvedLeaf_/isMainBinary_ above, this one is genuinely touched by two
  // different threads, hence the mutex below. Fixed-size, non-allocating
  // buffer rather than std::string: entries here are immortal for the same
  // reason resolvedLeaf_'s are (worker threads in the watched process can
  // still be allocating/freeing during process teardown), so every entry
  // either pays a heap allocation once, forever, or -- this -- pays a flat
  // 96 bytes in-line (bumped from 64 so "function (file:line)" combined
  // labels -- see formatSymbol -- usually fit without truncating either
  // half). Long symbol names (deeply templated C++ types especially)
  // truncate to fit; accepted, this is a metric label, not a debugger. Only
  // ever contains addresses that resolved to something useful --
  // resolveSymbol simply never inserts a "nothing found" result, so a
  // present entry is always non-empty (see attributionLabel).
  // submittedForResolution_ below, not this map, is what actually prevents
  // endlessly retrying an address that resolves to nothing.
  std::unordered_map<uintptr_t, std::array<char, 96>> resolvedSymbols_;

  // Guards resolvedSymbols_ AND submittedForResolution_ -- one lock for
  // both rather than two, since attributionLabel always touches them
  // together (check resolvedSymbols_, maybe check-and-mark
  // submittedForResolution_, in the same breath) and resolveSymbol only
  // ever touches resolvedSymbols_ briefly. Uncontended in practice:
  // resolver_ takes it once per drained queue entry, symbolResolver_ takes
  // it once per *resolved* address (far rarer, and each hold is a single
  // map insert). een::Mutex (not std::mutex), acquired via .acquire()'s RAII
  // guard.
  Mutex resolvedSymbolsMutex_;

  // Addresses already handed to symResolveQueue_, so a hot call site
  // doesn't get pushed again on every single alloc/free that picks it as
  // its attribution frame while resolution is still pending (or has
  // permanently failed to find anything -- see resolveSymbol). Only ever
  // marked on a *successful* tryPush (see attributionLabel), so a dropped
  // submission (queue momentarily full) naturally gets retried by whatever
  // later alloc/free next picks this same address, rather than being
  // permanently stuck unresolved.
  std::unordered_set<uintptr_t> submittedForResolution_;

  // Single-producer (resolver_, via attributionLabel), single-consumer
  // (symbolResolver_) -- RotatingBuffer's MPSC guarantees cover this as a
  // special case. Entries are deduplicated addresses
  // (submittedForResolution_), not one per alloc/free event like queue_, so
  // this stays shallow under normal operation; a burst of many distinct
  // *new* call sites at once (e.g. early process startup) is the only
  // realistic way to fill it, and like every other queue in this file, a
  // full push is dropped, never blocked/retried in place.
  memory::RotatingBuffer<uintptr_t, SymbolQueueCapacity> symResolveQueue_;

  // Declared before resolver_ deliberately: Thread's constructor starts the
  // thread immediately (running resolverLoop concurrently with the rest of
  // *this object's own construction), so everything resolverLoop/drainQueue/
  // processAlloc/processFree could possibly touch -- including constructed_
  // -- must already be initialized by the time resolver_'s member
  // initializer runs. Members initialize in declaration order regardless of
  // where they appear in this list, so order here is load-bearing. Same
  // applies to symbolResolver_ below, for the same reason.
  bool constructed_{true};

  // No IgnoreGlobalStop: this collator thread has no reason to opt out of
  // normal SIGINT/SIGTERM stop propagation, unlike some other een::Thread
  // users. resolverLoop's stopRequested() call is the plain (non-tag)
  // overload to match.
  Thread resolver_{"HeapTrackResolver", std::bind(&PromTracker::resolverLoop, this)};

  // Second, fully decoupled thread doing real (slow, allocating) DWARF
  // symbol resolution -- see resolveSymbol/drainSymbolQueue. Same
  // stop-propagation reasoning as resolver_ (no IgnoreGlobalStop, plain
  // stopRequested()).
  //
  // Declared -- and therefore destructed -- last: ~PromTracker() runs
  // member destructors in reverse declaration order, so this joins before
  // resolver_ does. That's backwards from the "producer stops before its
  // consumer" ordering that would guarantee symbolResolver_'s very last
  // drainSymbolQueue() catches everything resolver_ ever pushed -- with
  // this ordering, resolver_ (the producer into symResolveQueue_, via
  // attributionLabel) can still be running while symbolResolver_ is
  // stopping, so a handful of addresses submitted right at teardown can be
  // left sitting in symResolveQueue_ forever, never resolved. Accepted:
  // by the time either thread's destructor runs, the process is already
  // exiting (see this class's own history -- these threads are never
  // explicitly stopped before destruction, only at normal process-exit
  // static destruction), so nothing is left to read those metrics anyway.
  // Not worth the ordering gymnastics that "fixing" this would cost to
  // avoid a handful of never-read labels.
  Thread symbolResolver_{"HeapTrackSymResolver", std::bind(&PromTracker::symbolResolverLoop, this)};
};

}  // namespace een
