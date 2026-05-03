# Bitmapped Block Skip List (BBSL)

[![MIT License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)

A **high‑performance sparse array** that combines skip list indexing with **fixed‑size block storage**. Each block uses a bitmap to track occupied slots and stores elements **inline** — delivering exceptional cache locality and traversal speed.

I designed it to serve as the underlying support for **Arrays** for the **interpreter**, used to avoid performance cliffs caused by switching between dense and sparse modes.

## ✨ Features

- **Inline block storage** – Elements are stored directly inside each node, eliminating an extra pointer indirection and improving cache efficiency.
- **Bitmap indexing** – A configurable bitmap tracks which slots within a block are occupied, enabling O(1) presence checks.
- **Skip list indexing** – Blocks are organized as a skip list, providing expected O(log n) search, insert, and delete.
- **Self‑adjusting levels** – Skip list height automatically scales with the number of blocks.
- **STL‑style iterators** – Forward and reverse iteration with begin/end/rbegin/rend support.
- **Functional traversal** – `forEach`, `some`, `every` methods for efficient bulk operations.

## 🚀 Quick Start

```cpp
#include "bbsl.hpp"

// Create a sparse array with -1 as "empty" value
bbsl::BitmappedBlockSkipList<int, int> arr(-1);

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
arr.forEach([](const int& value, int index) {
    std::cout << index << ": " << value << "\n";
});
```

## 🧠 Design Highlights

### Inline Block Storage
Each `SkipListNode` contains a fixed‑size inline array:
- `elements[]` – Stored directly in the node (no separate heap allocation per node)
- `bitMap` – Bitmask indicating occupied slots
- `baseIndex` – Starting index of the block (aligned to block size)

This design eliminates an extra pointer dereference and dramatically improves cache locality during traversal.

### Configurable Block Size
The block size is determined by the bitmap type:
```cpp
using bitMap_t = uint16_t;  // 16 slots per block
// or
using bitMap_t = uint32_t;  // 32 slots per block
// or
using bitMap_t = uint8_t;   // 8 slots per block
```

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
| `SkipListNode`          | Block containing inline elements, bitmap, and level pointers |
| `BitmappedBlockSkipList` | Main container with sentinel head/tail and automatic level adjustment |
| `Xoroshiro64StarStar`   | Fast RNG for probabilistic level assignment |
| `bits.hpp`              | Optimized bit operations (popcount, ctz, clz, etc.) |

## 📈 When to Use BBSL

### ✅ Ideal Use Cases
- **Read‑heavy workloads** – Fast queries and iteration
- **Dense or semi‑dense data** – Blocks with good occupancy
- **Traversal‑intensive operations** – Full scans, aggregations, transformations
- **Cache‑sensitive applications** – Game engines, real‑time systems
- **Small to medium element types** – Inline storage works efficiently

### ⚠️ Considerations
- **Extremely sparse data** – Consider whether block alignment suits your access pattern
- **Very large element types** – Inline storage may waste memory if occupancy is low

## 📄 License

MIT License – see the [LICENSE](LICENSE) file for details.