# MemoryGraph SDK

iOS OOM diagnostics — captures memory snapshots before Jetsam kills the app, producing compact binary reports (.mgbin).

## Features

- **Two-phase OOM detection**: memory pressure dispatch source + high-frequency `os_proc_available_memory()` polling
- **Zero-malloc collection**: all snapshot data written through a pre-allocated memory pool — safe to run when the app is at its memory limit
- **Thread suspension**: freezes all other threads during collection for data consistency
- **ObjC/Swift object identification**: ISA pointer validation against a pre-built class set, with compile-time mask selection (arm64/arm64e/x86_64)
- **Top-N class ranking**: min-heap extraction of the largest classes by total allocation size
- **Statistical leak analysis**: flags classes exceeding configurable thresholds for instance count, heap size ratio, or object count ratio
- **Reference graph analysis** (optional): Tarjan's SCC algorithm detects retain cycles with ivar name resolution
- **Binary report format** (MGRP): ~15x smaller than equivalent JSON, with a human-readable text converter
- **App Store compliant mode**: `MG_APP_STORE_COMPLIANT` flag uses only public APIs

## Architecture

```
include/
  memory_graph.h          Public API (init, start, stop, trigger, report conversion)

src/
  mg_core.c               SDK lifecycle, snapshot orchestration
  mg_pool.{c,h}           Pre-allocated memory pool (mmap-backed, 8-byte aligned)
  mg_trigger.{c,h}        Two-phase OOM detection (dispatch source + timer polling)
  mg_thread.{c,h}         Mach thread suspension/resumption
  mg_vm_region.{c,h}      VM region enumeration via mach_vm_region
  mg_heap.{c,h}           Malloc zone enumeration, ObjC object identification
  mg_leak.{c,h}           Statistical leak analysis
  mg_ref_graph.{c,h}      Reference graph builder + Tarjan's SCC cycle detection
  mg_ivar_map.{c,h}       ObjC ivar offset-to-name lookup (for cycle annotation)
  mg_binary.{c,h}         Binary writer + string table (MGRP format)
  mg_binary_reader.{c,h}  Binary reader + text formatter (offline tool)
  mg_report.{c,h}         Report assembly (data sections -> binary file)
  mg_compat.{c,h}         App Store compliant fallback implementations

tools/
  mg2crash.c              CLI tool: .mgbin -> .crash conversion
```

### Module Dependency Graph

```
memory_graph.h (public API)
    |
    v
mg_core.c -----> mg_pool -----> (mmap)
    |-------> mg_trigger -----> (dispatch, os/proc)
    |-------> mg_thread ------> (mach/thread_act)
    |-------> mg_vm_region ---> (mach/mach_vm)
    |-------> mg_heap --------> (malloc/malloc, objc/runtime)
    |-------> mg_leak
    |-------> mg_ref_graph ---> mg_ivar_map -> (objc/runtime)
    |-------> mg_report ------> mg_binary
    |                           mg_binary_reader (offline)
    +-------> mg_compat (App Store mode)
```

### Collection Flow

```
mg_trigger_now() / OOM trigger
  |
  Phase A: mg_class_set_build()      — threads running
           mg_ref_graph_create()
           mg_ivar_map_build()
  |
  Phase B: mg_threads_suspend()      — freeze all other threads
  |
  Phase C: mg_vm_region_collect()    — enumerate VM regions
           mg_heap_collect()         — enumerate heap + identify objects
           mg_ref_graph_scan_references() — scan inter-object pointers
  |
  Phase D: mg_threads_resume()       — unfreeze threads
  |
  Phase E: mg_leak_analyze()         — statistical analysis
           mg_ref_graph_find_cycles() — Tarjan's SCC
  |
  Phase F: mg_report_write()         — write .mgbin
```

## Binary Report Format (MGRP)

```
Offset  Size   Field
──────  ────   ─────
0       4      Magic: 0x4D475250 ("MGRP")
4       2      Version (currently 1)
6       2      Flags (bit 0: truncated, bit 1: ref graph enabled)
8       4      Section count
12      4      Reserved

[Sections follow, each prefixed with type(1B) + payload_length(4B)]

Section types:
  1  STRING_TABLE    Shared string table (must be first)
  2  METADATA        Timestamp, trigger reason, version
  3  DEVICE          Model, OS version, physical RAM
  4  APP             Memory footprint, available memory
  5  VM_SUMMARY      Total virtual/resident + per-tag stats
  6  VM_REGIONS      Individual VM region records
  7  HEAP            Zone stats + top-N class rankings
  8  LEAK            Leak suspects with reason codes
  9  REF_GRAPH       Retain cycles with ivar annotations
```

All integers are little-endian. Strings are stored as uint16 indices into the string table.

## API Reference

```c
#include <MemoryGraph/memory_graph.h>

// Configuration with sensible defaults
mg_config_t cfg = mg_config_default();
cfg.memory_threshold      = 100 * 1024 * 1024;  // 100 MB
cfg.pool_size             = 50 * 1024 * 1024;   // 50 MB pre-allocated
cfg.enable_reference_graph = true;               // detect retain cycles

// Lifecycle
mg_init(&cfg);
mg_set_callback(my_report_handler);  // called on next launch
mg_start();                          // begin monitoring
mg_trigger_now();                    // manual snapshot
mg_stop();
mg_destroy();

// Pending report (next launch)
if (mg_has_pending_report()) {
    const char *path = mg_pending_report_path();
    // upload, display, etc.
    mg_clear_pending_report();
}

// Convert binary to human-readable text
mg_report_to_text("report.mgbin", "report.txt");
```

### Error Codes

| Code | Name | Value |
|------|------|-------|
| `MG_OK` | Success | 0 |
| `MG_ERR_ALREADY_INIT` | Already initialized | -1 |
| `MG_ERR_NOT_INIT` | Not initialized | -2 |
| `MG_ERR_ALLOC` | Allocation failure | -3 |
| `MG_ERR_IO` | I/O error | -4 |
| `MG_ERR_INVALID_ARG` | Invalid argument | -5 |

## Command-Line Tool (mg2crash)

Standalone CLI for converting binary reports to human-readable `.crash` files. Links only the binary reader — no Apple framework dependencies.

```bash
make cli                                    # build tools/mg2crash

mg2crash report.mgbin                       # -> report.crash (auto rename)
mg2crash report.mgbin output.crash          # explicit output path
mg2crash report.mgbin -                     # write to stdout
mg2crash --validate report.mgbin            # validate format only
mg2crash *.mgbin                            # batch convert
```

Exit codes: 0 = success, 1 = usage error, 2 = conversion failure.

## Build

```bash
# macOS host build (for testing)
make test

# Build mg2crash CLI tool
make cli

# With AddressSanitizer + UBSan
make test-asan

# Clean
make clean
```

## Integration (CocoaPods)

```ruby
# Full version (uses Mach APIs)
pod 'MemoryGraph'

# App Store compliant (public APIs only)
pod 'MemoryGraph/AppStoreCompliant'
```

The App Store compliant subspec defines `MG_APP_STORE_COMPLIANT=1`, which disables:
- Direct `task_threads()` / `thread_suspend()` calls
- `malloc_get_all_zones()` heap enumeration
- ObjC runtime introspection for ivar mapping

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Pool allocation | O(1) bump pointer, 8-byte aligned |
| Class set lookup | O(log N) binary search on sorted array |
| ISA validation | 1 binary search per object (compile-time mask selection) |
| Heap enumeration | ARM64 `PRFM` prefetch hides ISA cache miss latency |
| Class stats aggregation | O(1) amortized hash table (pointer-key, open addressing) |
| Top-N extraction | O(K log N) min-heap, N=50 typical |
| String table add/find | O(1) amortized FNV-1a hash |
| Reference scanning | ARM64 NEON: 2 pointers/cycle, vectorized filter |
| Retain cycle detection | O(V + E) Tarjan's SCC |
| Cycle dedup | O(1) hash pre-filter + strcmp fallback |
| Report size | ~3.5 KB typical (vs ~50 KB JSON equivalent) |
| Thread suspension | < 100ms target, 30s hard timeout |

### ARM64 Optimizations

Two hot paths use ARM64-specific optimizations (transparent fallback on x86_64):

- **NEON vectorized pointer scanning** (`mg_ref_graph_scan_references`): loads 2 pointers per iteration via `vld1q_u64`, filters zero/tagged/unaligned values in parallel with `vceqq_u64`/`vtstq_u64`/`vorrq_u64`, then calls `addr_lookup` only for candidates that survive. ~2x throughput on the reference scanning phase.
- **ISA pointer prefetch** (`range_recorder`): issues `PRFM PLDL1STRM` for the next heap block's ISA address while processing the current one, hiding ~100+ cycle cache miss latency for random heap addresses.

## License

MIT
