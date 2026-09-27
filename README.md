# Bitmapped Block Skip List (BBSL)

[![MIT License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)

A **high‑performance sparse array** that combines skip list indexing with **fixed‑size block storage**. Each block uses a bitmap to track occupied slots and keeps its elements in a dedicated, allocation‑stable buffer — delivering predictable memory usage and fast traversal.

I designed it to serve as the underlying support for **Arrays** for the **interpreter**, used to avoid performance cliffs caused by switching between dense and sparse modes.

## ✨ Features

- **Fixed‑size blocks** – Each node covers exactly 8 slots tracked by a byte bitmap, enabling O(1) presence checks and gap‑free iteration over occupied slots.
- **Stable element storage** – Every block owns a dedicated buffer for its 8 slots. It is allocated once and never moves, so element addresses survive all structural changes.
- **Skip list indexing** – Blocks are organized as a skip list, providing expected O(log n) search, insert, and delete. Height auto‑scales with the block count (capped at level 31).
- **Compressed node references** – All neighbor links are stored as 32‑bit offsets into one contiguous virtual‑address segment, halving pointer memory versus raw 64‑bit pointers.
- **Bundled slab allocator** – Nodes and link arrays are served by a self‑contained arena/slab allocator (`third-party/slabAllocator`) that reserves one large VA segment up front and commits on demand.
- **STL‑style iterators** – Forward and reverse iteration with begin/end/rbegin/rend support.
- **Functional traversal** – `forEach`, `some`, `every` methods for efficient bulk operations.
- **Optional memory accounting** – `BBSL_MEMORY_STATS` compiles in a `memoryUsage()` that reports exact requested bytes via a chain walk.

## 🚀 Quick Start

```cpp
#define BBSL_MEMORY_STATS          // optional: enables memoryUsage()
#include "src/bbsl.hpp"

// Create a sparse array with -1 as "empty" value
bbsl::BitmappedBlockSkipList<uint64_t, int> arr(-1);

// Write elements
arr[100] = 42;
arr[500] = 73;
arr[1'000'000] = 999;   // large indices are fine

// Read elements
int val = arr[100];     // 42
val = arr[200];         // -1 (empty)

// Delete
arr.erase(100);
bool exists = arr.has(100);   // false

// Iteration (STL-style)
for (auto it = arr.begin(); it != arr.end(); ++it) {
    std::cout << it.key() << " -> " << *it << "\n";
}

// Functional traversal
arr.forEach([](const int& value, uint64_t index) {
    std::cout << index << ": " << value << "\n";
});

// Optional: exact memory footprint (requires BBSL_MEMORY_STATS)
auto usage = arr.memoryUsage();
std::cout << usage.bytes << " bytes in " << usage.allocations << " allocations\n";
```

## 🧠 Design Highlights

### Fixed‑Size Nodes, Separate Element Buffers
Each `SkipListNode` is a compact 32‑byte header:
- `baseIndex` – First index covered by the block (8‑slot aligned)
- `node_capacity` / `level` – Link capacity and current tower height
- `bitMap` – Occupied slots within the block
- `elements` – Own heap buffer of 8 slots (`new value_t[8]`), allocated once and never reallocated
- `nodes` – Neighbor link array (`right = level*2`, `left = level*2 + 1`), grown on demand

Element addresses stay valid for the lifetime of the block; growing a node's tower never moves the node or its elements.

### Compressed Node References (8B → 4B)
Neighbor links and descent caches store node references as `uint32_t`:
- The allocator reserves one aligned VA segment and hands out 16‑byte‑aligned blocks, so the low 4 bits of every node address are zero.
- Encode: `ref = addr >> 4` · Decode: `base | (ref << 4)` — one shift plus one OR.
- `ref == 0` is null: the segment start is occupied by allocator metadata and is never handed out.
- Decode results are tested with `isNonNull()`; raw pointers remain usable everywhere else (iterators, descent caches) because nodes never move.

### Bundled Arena Slab Allocator
`third-party/slabAllocator` is a self‑contained C11 allocator:
- One reserved VA segment per process (64 GB on x64, shared by all container instances), committed in 16 KB arenas on demand.
- Five slot size classes (16/32/64/128/256 B) with per‑class free lists; realloc grows through the class ladder and copies.
- Node headers (32 B) and link arrays (≤ 256 B) live here; element buffers stay on the regular heap because their size is unbounded (`value_t` can be arbitrarily large).
- Windows (`VirtualAlloc`) and POSIX (`mmap`) backends included. A 64‑bit OS is required for the compressed layout; 32‑bit builds fall back to an uncompressed 256 MB segment.

### Bitmap Operations
```cpp
// Fast iteration over occupied slots only
for (int8_t i = SkipListNode::begin(node); i != -1; i = SkipListNode::next(node, i)) {
    process(node->elements[i]);  // Only iterate existing elements
}
```

### STL‑Compatible Iterators
- Forward iterator (`begin()` / `end()`)
- Reverse iterator (`rbegin()` / `rend()`)
- Bidirectional traversal support (`operator++`, `operator--`)

## 🔧 Implementation Details

| Component               | Description |
|-------------------------|-------------|
| `SkipListNode`          | 32 B block header: bitmap, capacity, level, element buffer and link array pointers |
| `BitmappedBlockSkipList` | Main container with small-start sentinels, automatic level adjustment (level ≤ 31) and descent-path caching |
| `Xoroshiro64StarStar`   | Fast RNG for probabilistic level assignment |
| `arena_slab.c/h`        | Arena/slab allocator backing node and link storage (`third-party/slabAllocator`) |
| `palloc_os.c/h`         | OS virtual-memory backend (VirtualAlloc / mmap) for the allocator segment |
| `bits.h/c`              | Bit operations (power-of-two ceil, popcount, ctz, clz) shared with the allocator |

### Build Notes
- Requires C++17. Add `test.cpp` plus the three allocator `.c` files (compiled as C11) to your build.
- Measured footprint (1 M dense `int` elements, requested bytes): **~10.3 B/elem** in 3 allocations per node — versus 48 B/elem for `std::map`. See `BBSL_MEMORY_STATS` above to reproduce via `memoryUsage()`.

## 📈 When to Use BBSL

### ✅ Ideal Use Cases
- **Read‑heavy workloads** – Fast queries and iteration
- **Dense or semi‑dense data** – Blocks with good occupancy
- **Traversal‑intensive operations** – Full scans, aggregations, transformations
- **Cache‑sensitive applications** – Game engines, real‑time systems, interpreter runtimes
- **Small to medium element types** – Each block commits a fixed 8‑slot buffer

### ⚠️ Considerations
- **Extremely sparse data** – Blocks are 8‑slot aligned; scattered keys can leave blocks mostly empty (worst case ≈ one element per block)
- **Very large element types** – Every touched block allocates a full 8‑slot element buffer up front
- **Multi‑threading** – The container and its allocator are single‑threaded by design

## 📄 License

MIT License – see the [LICENSE](LICENSE) file for details.
The bundled `third-party/slabAllocator` carries its own MIT license (`third-party/slabAllocator/LICENSE`).
