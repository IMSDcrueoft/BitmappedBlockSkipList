/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#pragma once
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <iostream>
#include <memory_resource>

#include "../third-party/slabAllocator/includes/bits.h"
#include "../third-party/slabAllocator/includes/arena_slab.h"

namespace bbsl {
	class Xoroshiro64StarStar {
	private:
		uint64_t state;

	public:
		Xoroshiro64StarStar(uint64_t seed = 0x2BD65925FA21F4A3) : state(seed) {}

		void seed(uint64_t seed) {
			state = seed;
		}

		uint64_t next() {
			const uint64_t s0 = state;
			uint64_t s1 = s0 << 55;

			state = s0 ^ (s0 << 23);
			state ^= (state >> 26) ^ (s1 >> 9);

			return ((s0 * 0x9E3779B97F4A7C15) >> 32) * 0xBF58476D1CE4E5B9;
		}
	};

	using bitMap_t = uint8_t;
	constexpr uint64_t capacity_count = sizeof(bitMap_t) * 8;
	constexpr uint64_t index_align = (capacity_count - 1); // Align to capacity limit

	/**
	 *  When the number of elements in the bottom layer > 2 ^ (current level count), add a new level.
	 *  Conversely, if the number of blocks in the bottom layer < 2 ^ (current level count - 1), remove the topmost level.
	 *  Therefore, even when used as a regular array, it still functions as a skip list with a expected complexity of (log(n / capacityLimit) + 1) to (log(n) + 1).
	 *
	 *  Only inserting sparse and vector too empty creates new nodes
	 *  and when deleted, they are deleted in place instead of splitting
	 *  avoiding the complexity caused by merging and splitting
	 */
	template <typename index_t, typename value_t, typename = std::enable_if<std::is_integral_v<index_t>&& std::is_default_constructible_v<value_t>&& std::is_copy_assignable_v<value_t>>>
	class BitmappedBlockSkipList {
	protected:
		/**
		 * @brief	It's just for storing data, so it's struct
		 * @tparam value_t	It shouldn't be a particularly short type, otherwise the node is larger than the data
		 *
		 * In order to compress the memory occupied by a single node, we do not apply STL containers
		 */
		struct SkipListNode {
			index_t baseIndex;					//The array is offset by the index, which is almost unmodified
			uint8_t node_capacity;				//real capacity = *2
			uint8_t level;						//height
			bitMap_t bitMap;					//use bitMap to manage

			value_t* elements = nullptr;		//separate fixed-size storage, element addresses survive reallocs
			uint32_t* nodes = nullptr;			//pointer point to compressed node pointers: right = level*2, left = level*2 + 1
		protected:
			static inline uintptr_t segmentBase = UINTPTR_MAX;
			// note that allocator never returns zero, zo uint32_t(0) is nullptr
			static constexpr uint32_t compressed_nullptr = 0;
	
			static SkipListNode* acquire_node() {
				if (SkipListNode::segmentBase == UINTPTR_MAX) {
					if constexpr (sizeof(uintptr_t) == 8) {
						if (!arenaSlab_init(&arenaSlabDefault, 36)) {
							std::cerr << "Failed to initialize arena slab allocator!\n";
							exit(1);
						}
						SkipListNode::segmentBase = arenaSlab_segmentBase(&arenaSlabDefault);
					}
					else {
						if (!arenaSlab_init(&arenaSlabDefault, 28)) {
							std::cerr << "Failed to initialize arena slab allocator!\n";
							exit(1);
						}
						SkipListNode::segmentBase = 0;
					}
				}

				void* pointer = arenaSlab_alloc(&arenaSlabDefault, sizeof(SkipListNode));
				if (pointer == nullptr) {
					std::cerr << "Memory allocation failed!\n";
					exit(1);
				}

				return static_cast<SkipListNode*>(pointer);
			}

			static void restore_node(SkipListNode* node) {
				arenaSlab_free(&arenaSlabDefault, node);
			}

		public:
			static bool isNonNull(SkipListNode* node) {
				return reinterpret_cast<uintptr_t>(node) > SkipListNode::segmentBase;
			}

			static uint32_t compressPointer(SkipListNode* node) {
				if constexpr (sizeof(uintptr_t) == 8) {
					return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(node) >> 4);
				}
				else {
					return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(node));
				}
			}

			static SkipListNode* decompressPointer(uint32_t compressedPtr) {
				if constexpr (sizeof(uintptr_t) == 8) {
					return reinterpret_cast<SkipListNode*>(SkipListNode::segmentBase | (static_cast<uintptr_t>(compressedPtr) << 4));
				}
				else {
					return reinterpret_cast<SkipListNode*>(static_cast<uintptr_t>(compressedPtr));
				}
			}

			static SkipListNode* create(const index_t baseIndex, const uint8_t level, const bool withElements = true) {
				const uint8_t capacity = bits_ceil(level + 1);

				//the node is fixed size, allocated once and never moves
				SkipListNode* node = SkipListNode::acquire_node();

				//T is the element type, counts are object counts: 2 pointers per level, upto 64 * 4B = 256B
				node->nodes = static_cast<uint32_t*>(arenaSlab_alloc(&arenaSlabDefault, sizeof(uint32_t) * (capacity << 1)));
				if (node->nodes == nullptr) {
					std::cerr << "Memory allocation failed!\n";
					exit(1);
				}

				//the buffer is allocated once and never moves, so any default constructible
				//and copy assignable type works, triviality is not required anymore
				node->elements = withElements ? new value_t[bbsl::capacity_count] : nullptr;

				node->baseIndex = baseIndex;
				node->bitMap = 0;
				node->node_capacity = capacity;
				node->level = level;
				std::fill_n(node->nodes, capacity << 1, compressed_nullptr);
				return node;
			}

			static void destroy(SkipListNode* node) {
				delete[] node->elements;
				arenaSlab_free(&arenaSlabDefault, node->nodes);
				SkipListNode::restore_node(node);
			}

			/**
			 * @brief	raises the level by one, reallocating the pointer array when capacity runs out.
			 *			the node itself never moves, so neighbor pointers stay valid.
			 */
			void grow() {
				++this->level;
				if (this->level < this->node_capacity) {
					this->nodes[this->level << 1] = compressed_nullptr;
					this->nodes[(this->level << 1) | 1] = compressed_nullptr;
					return;
				}

				const uint8_t newCapacity = this->node_capacity << 1;
				this->nodes = static_cast<uint32_t*>(arenaSlab_realloc(&arenaSlabDefault, this->nodes, sizeof(uint32_t) * (newCapacity << 1)));
				if (this->nodes == nullptr) {
					std::cerr << "Memory allocation failed!\n";
					exit(1);
				}
				this->node_capacity = newCapacity;
				std::fill_n(this->nodes + (this->level << 1), (newCapacity - this->level) << 1, compressed_nullptr);
			}

			/**
			 * @brief if the node is empty
			 * @return
			 */
			bool isEmpty() {
				return this->bitMap == 0;
			}

			/**
			 * @brief
			 * @param index
			 * @return
			 */
			bool hasElement(const uint8_t index) {
				return (index < bbsl::capacity_count) && bits_get(this->bitMap, index);
			}

			/**
			 * @param index
			 * @param value the reference of value
			 * only when it is valid, the value will be set
			 */
			void getElement(const uint8_t index, value_t& value) const {
				//if (index >= bbsl::capacity_count) return;
				//set value
				if (bits_get(this->bitMap, index)) value = this->elements[index];
			}

			/**
			 * @param index
			 * @param value
			 */
			void setElement(const uint8_t index, const value_t& value) {
				//if (index >= bbsl::capacity_count) return;

				//set value and bitmap
				this->elements[index] = value;
				bits_set_one(this->bitMap, index);
			}

			/**
			 * @brief logic delete
			 * @param index
			 */
			void deleteElement(const uint8_t index) {
				//if (index >= bbsl::capacity_count) return;
				bits_set_zero(this->bitMap, index);
			}

			void decreaseLevel() {
				--this->level;
			}

			SkipListNode* getLeftNode(const uint8_t level) const {
				return SkipListNode::decompressPointer(this->nodes[(level << 1) | 1]);
			}

			SkipListNode* getRightNode(const uint8_t level) const {
				return SkipListNode::decompressPointer(this->nodes[(level << 1)]);
			}

			void setLeftNode(const uint8_t level, SkipListNode* node) {
				this->nodes[(level << 1) | 1] = SkipListNode::compressPointer(node);
			}

			void setRightNode(const uint8_t level, SkipListNode* node) {
				this->nodes[(level << 1)] = SkipListNode::compressPointer(node);
			}

			static bool isIndexValid(const uint64_t index) {
				return index < bbsl::capacity_count;
			}

			static int8_t begin(const SkipListNode* node) {
				if (node->bitMap == 0) return -1;
				return bits_ctz64(node->bitMap);
			}

			static int8_t end(const SkipListNode* node) {
				if (node->bitMap == 0) return -1;
				uint8_t clzValue = bits_clz64(node->bitMap) - (64 - bbsl::capacity_count);
				return bbsl::capacity_count - clzValue - 1;
			}

			static int8_t next(const SkipListNode* node, const int8_t index) {
				if (index >= bbsl::capacity_count) return -1;
				// 0b00101100, index = 2, nextBits = 0b00001011, nextIndex = 3
				bitMap_t nextBits = node->bitMap >> (index + 1);
				if (nextBits == 0) return -1; // ctz will return 64 if the input is 0, so we must directly return -1
				return index + bits_ctz64(nextBits) + 1;
			}

			static int8_t prev(const SkipListNode* node, const int8_t index) {
				if (index >= bbsl::capacity_count || index == 0) return -1;
				// 0b00101100, index = 5, prevBits = 0b01100000, prevIndex = 3
				// prevBits = the set bits below index, shifted to the top of the window;
				// prev is the highest of them mapped back: (63 - clz64(prevBits)) - (8 - index)
				bitMap_t prevBits = static_cast<bitMap_t>(node->bitMap << (bbsl::capacity_count - index));
				if (prevBits == 0) return -1; // clz will return 64 if the input is 0, so we must directly return -1
				return static_cast<int8_t>((63 - bits_clz64(prevBits)) - (bbsl::capacity_count - index));
			}
		};

		// limit node size
		static_assert(sizeof(SkipListNode) <= 32, "Node size exceeds limit");
	protected:
		mutable SkipListNode* leftPathNodes[32] = { nullptr };
		bbsl::Xoroshiro64StarStar rng;

		//heap allocated, start at level 0 and grow via grow() alongside the list height
		SkipListNode* sentryHead;
		SkipListNode* sentryTail;

		uint64_t width = 0;//the node count
		int64_t level = 0;//the height

		value_t invalid;//you need an invalid default value

		//check if need add level
		void increaseLevel() {
			// 1. level up sentry, grow() never moves them, only their pointer arrays may realloc
			this->sentryHead->grow();
			this->sentryTail->grow();
			++this->level;

			// 2. get nodes witch level == this->level - 1
			SkipListNode* node = this->sentryHead->getRightNode(this->level - 1);
			SkipListNode* left = this->sentryHead;

			while (node->level < (this->level - 1)) {
				node = node->getRightNode(this->level - 1);
			}

			//at least one node
			bool promoted = false;

			while (node != this->sentryTail) {
				// 50% percent
				if ((this->rng.next() & 1) || !promoted) {
					// grow() never moves the node, neighbors stay valid
					node->grow();

					// connect node
					node->setLeftNode(this->level, left);
					left->setRightNode(this->level, node);
					left = node;

					promoted = true;
				}
				node = node->getRightNode(this->level - 1);
			}

			//connect
			left->setRightNode(this->level, this->sentryTail);
			this->sentryTail->setLeftNode(this->level, left);
		}

		//check if need sub level
		void decreaseLevel() {
			//level down all node that level == this.level
			SkipListNode* node = this->sentryHead;

			//the tail's right slot is a null slot: decompressed it reads segmentNull, so terminate via isNonNull
			while (SkipListNode::isNonNull(node)) {
				SkipListNode* right = node->getRightNode(this->level);
				node->decreaseLevel();
				node = right;
			}

			--this->level;
		}

		/**
		 * @brief
		 * @return
		 */
		uint8_t getRandomLevel() {
			//limit level [0-31]
			const auto count = bits_ctz64(this->rng.next()) & 31;
			return (count <= this->level) ? count : this->level;
		}

		/**
		 * @brief	find the node with the maximum baseIndex <= index, recording only the level-0 cache slot
		 *			used by read paths, so they skip all other path stores
		 * @param	index
		 * @return
		 */
		SkipListNode* findNodeNoPath(const index_t index) const {
			SkipListNode* node = this->sentryHead;
			auto curLevel = this->level;

			while (curLevel >= 0) {
				auto next = node->getRightNode(curLevel);
				// check next node, if it is the tail sentinel, then go down a level
				if (next != this->sentryTail && next->baseIndex <= index) {
					node = next;
				}
				else {
					--curLevel;
				}
			}

			// keep the level-0 cache slot warm, so sequential access keeps hitting the quick path
			this->leftPathNodes[0] = node;
			return node;
		}

		/**
		 * @brief
		 * @param index
		 * @return
		 */
		SkipListNode* findLeftNode(const index_t index) const {
			SkipListNode* node = this->sentryHead;
			auto curLevel = this->level;

			while (curLevel >= 0) {
				auto next = node->getRightNode(curLevel);
				// check next node, if it is nullptr, then go down a level
				if (next != this->sentryTail && next->baseIndex <= index) {
					node = next;
				}
				else {
					this->leftPathNodes[curLevel] = node;
					--curLevel;
				}
			}

			return this->leftPathNodes[0];
		}

		/**
		 * @brief
		 * @param index
		 * @return
		 */
		SkipListNode* insertNode(const index_t index) {
			//make node
			const auto level = this->getRandomLevel();
			SkipListNode* newNode = SkipListNode::create(index, level);

			//connect
			SkipListNode* left = nullptr, * right = nullptr;

			//sentry level is ennough right now
			for (auto i = 0; i <= level; ++i) {
				// must call findLeftNode before insertNode, so leftPathNodes are valid
				left = this->leftPathNodes[i];
				right = left->getRightNode(i);

				newNode->setLeftNode(i, left);
				left->setRightNode(i, newNode);
				newNode->setRightNode(i, right);
				right->setLeftNode(i, newNode);
			}

			++this->width;
			//sentries are pre-sized for level 31, so the height is capped there;
			//the width check is the rare event, it gates the cap check off the hot path
			if (this->width >= (1ULL << this->level) && this->level < 31) {
				increaseLevel();
			}

			return newNode;
		}

		/**
		 * @brief
		 * @param node
		 */
		void removeNode(SkipListNode* node) {
			SkipListNode* left = nullptr, * right = nullptr;

			for (auto i = 0; i <= node->level; ++i) {
				// must call findLeftNode before removeNode, so leftPathNodes are valid
				left = (this->leftPathNodes[i] != node) ? this->leftPathNodes[i] : node->getLeftNode(i);
				right = node->getRightNode(i);

				left->setRightNode(i, right);
				right->setLeftNode(i, left);
			}

			SkipListNode::destroy(node);
			--this->width;

			// remind: we set path node after remove, so we never get invalid path node0
			this->leftPathNodes[0] = nullptr;

			constexpr auto minLevel = 6;
			if (this->level < minLevel) return;

			if (this->width < (1ULL << (this->level - 1))) {
				this->decreaseLevel();
			}
		}
	public:
		/**
		 * @brief
		 * @param invalid invalid value, it should be a default value that is not used in the data
		 */
		BitmappedBlockSkipList(const value_t& invalid) {
			this->invalid = invalid;

			//sentries start small, their pointer arrays grow alongside the list height
			this->sentryHead = SkipListNode::create(0, 0, false);
			this->sentryTail = SkipListNode::create(0, 0, false);

			this->sentryHead->setRightNode(0, this->sentryTail);
			this->sentryTail->setLeftNode(0, this->sentryHead);
		}

		/**
		 * @brief
		 * @param seed
		 * @param invalid invalid value, it should be a default value that is not used in the data
		 */
		BitmappedBlockSkipList(const value_t& invalid, uint64_t seed) {
			this->invalid = invalid;

			//sentries start small, their pointer arrays grow alongside the list height
			this->sentryHead = SkipListNode::create(0, 0, false);
			this->sentryTail = SkipListNode::create(0, 0, false);

			this->sentryHead->setRightNode(0, this->sentryTail);
			this->sentryTail->setLeftNode(0, this->sentryHead);

			rng.seed(seed);
		}

		~BitmappedBlockSkipList() {
			// release one by one
			SkipListNode* node = this->sentryHead->getRightNode(0);
			while (SkipListNode::isNonNull(node) && node != this->sentryTail) {
				SkipListNode* next = node->getRightNode(0);
				SkipListNode::destroy(node);
				node = next;
			}
			SkipListNode::destroy(this->sentryHead);
			SkipListNode::destroy(this->sentryTail);
		}

		int64_t getLevel() {
			return this->level;
		}

		/**
		 * @brief
		 * @param index
		 * @return
		 */
		bool has(const index_t index) const {
			if (this->width == 0) return false;

			SkipListNode* node = this->findNodeNoPath(index);
			// now node is the maximum node with baseIndex <= index
			if (node != this->sentryHead && node->baseIndex <= index && SkipListNode::isIndexValid(index - node->baseIndex)) {
				return node->hasElement(index - node->baseIndex);
			}
			return false;
		}

		/**
		 * @brief
		 * @param index
		 */
		bool erase(const index_t index) {
			if (this->width == 0) return false;

			SkipListNode* node = this->findNodeNoPath(index);
			// now node is the maximum node with baseIndex <= index
			if (node != this->sentryHead && node->baseIndex <= index && SkipListNode::isIndexValid(index - node->baseIndex)) {
				uint8_t offset = static_cast<uint8_t>(index - node->baseIndex);
				if (node->hasElement(offset)) {
					node->deleteElement(offset);

					//remove node
					if (node->isEmpty()) {
						// unlinking needs the descent path, rebuild it here
						this->findLeftNode(index);
						this->removeNode(node);
					}
					return true;
				}
			}
			return false;
		}

		/**
		 * @brief
		 * @param index
		 * @return
		 */
		value_t& operator[](const index_t index) {
			// quick path: we dont need full node path when setting exist element, so we directly find left node[0] and check
			SkipListNode* cachedNode = this->leftPathNodes[0];
			if (cachedNode != nullptr && cachedNode != this->sentryHead && cachedNode->baseIndex <= index && SkipListNode::isIndexValid(index - cachedNode->baseIndex)) {
				uint8_t offset = static_cast<uint8_t>(index - cachedNode->baseIndex);
				if (!cachedNode->hasElement(offset)) {
					cachedNode->setElement(offset, this->invalid);
				}

				return cachedNode->elements[offset];
			}

			SkipListNode* node = this->findLeftNode(index);

			if (node != this->sentryHead && node->baseIndex <= index && SkipListNode::isIndexValid(index - node->baseIndex)) {
				uint8_t offset = static_cast<uint8_t>(index - node->baseIndex);
				if (!node->hasElement(offset)) {
					node->setElement(offset, this->invalid);
				}

				return node->elements[offset];
			}

			//align to capacity
			const index_t offsetIndex = index & index_align;

			SkipListNode* newNode = this->insertNode(index - offsetIndex);
			newNode->setElement(offsetIndex, this->invalid);
			return newNode->elements[offsetIndex];
		}

		/**
		 * @brief
		 * @param index
		 * @return
		 */
		const value_t& operator[](const index_t index) const {
			// quick path: we dont need full node path when setting exist element, so we directly find left node[0] and check
			SkipListNode* cachedNode = this->leftPathNodes[0];
			if (cachedNode != nullptr && cachedNode != this->sentryHead && cachedNode->baseIndex <= index && SkipListNode::isIndexValid(index - cachedNode->baseIndex)) {
				uint8_t offset = static_cast<uint8_t>(index - cachedNode->baseIndex);

				if (cachedNode->hasElement(offset)) {
					return cachedNode->elements[offset];
				}
			}

			SkipListNode* node = this->findNodeNoPath(index);
			if (node != this->sentryHead && node->baseIndex <= index && SkipListNode::isIndexValid(index - node->baseIndex)) {
				uint8_t offset = static_cast<uint8_t>(index - node->baseIndex);

				if (node->hasElement(offset)) {
					return node->elements[offset];
				}
			}

			return this->invalid;
		}

	public:
		template<typename Func>
		void forEach(Func func) const {
			SkipListNode* node = this->sentryHead->getRightNode(0);
			while (SkipListNode::isNonNull(node) && node != this->sentryTail) {
				// dense fast path: every slot is occupied, iterate sequentially without bit tests
				if (node->bitMap == std::numeric_limits<bitMap_t>::max()) {
					const index_t base = node->baseIndex;
					const value_t* elements = node->elements;
					for (uint8_t i = 0; i < bbsl::capacity_count; ++i) {
						func(elements[i], base + i);
					}
				}
				else {
					for (int8_t i = SkipListNode::begin(node); i != -1; i = SkipListNode::next(node, i)) {
						func(node->elements[i], node->baseIndex + i);
					}
				}
				node = node->getRightNode(0);
			}
		}

		template<typename Func>
		bool some(Func func) const {
			SkipListNode* node = this->sentryHead->getRightNode(0);
			while (SkipListNode::isNonNull(node) && node != this->sentryTail) {
				// dense fast path: every slot is occupied, iterate sequentially without bit tests
				if (node->bitMap == std::numeric_limits<bitMap_t>::max()) {
					const index_t base = node->baseIndex;
					const value_t* elements = node->elements;
					for (uint8_t i = 0; i < bbsl::capacity_count; ++i) {
						if (func(elements[i], base + i)) return true;
					}
				}
				else {
					for (int8_t i = SkipListNode::begin(node); i != -1; i = SkipListNode::next(node, i)) {
						if (func(node->elements[i], node->baseIndex + i)) return true;
					}
				}
				node = node->getRightNode(0);
			}
			return false;
		}

		template<typename Func>
		bool every(Func func) const {
			SkipListNode* node = this->sentryHead->getRightNode(0);
			while (SkipListNode::isNonNull(node) && node != this->sentryTail) {
				// dense fast path: every slot is occupied, iterate sequentially without bit tests
				if (node->bitMap == std::numeric_limits<bitMap_t>::max()) {
					const index_t base = node->baseIndex;
					const value_t* elements = node->elements;
					for (uint8_t i = 0; i < bbsl::capacity_count; ++i) {
						if (!func(elements[i], base + i)) return false;
					}
				}
				else {
					for (int8_t i = SkipListNode::begin(node); i != -1; i = SkipListNode::next(node, i)) {
						if (!func(node->elements[i], node->baseIndex + i)) return false;
					}
				}
				node = node->getRightNode(0);
			}
			return true;
		}

		class IterObject {
		private:
			BitmappedBlockSkipList* skiplist = nullptr;
			SkipListNode* node = nullptr;
			int8_t inside_index = 0;

		public:
			IterObject() = delete;
			IterObject(BitmappedBlockSkipList* skiplist, SkipListNode* node, int8_t inside_index)
				: skiplist(skiplist), node(node), inside_index(inside_index) {
			}

			~IterObject() = default;

			const value_t& operator*() const {
				// we don't know if user will call operator* when the node is deleted, so we return invalid in this case
				return node->hasElement(inside_index) ? node->elements[inside_index] : skiplist->invalid;
			}

			index_t key() const {
				return (this->node != nullptr) ? (this->node->baseIndex + this->inside_index) : 0;
			}

			bool setValue(const value_t& value) {
				if (this->node == nullptr) return false;
				this->node->setElement(this->inside_index, value);
				return true;
			}

			IterObject& operator++() {
				if (this->node == nullptr) return *this;
				int8_t nextIndex = SkipListNode::next(this->node, this->inside_index);

				if (nextIndex == -1) {
					this->node = this->node->getRightNode(0);
					if (SkipListNode::isNonNull(this->node) && this->node != this->skiplist->sentryTail) {
						this->inside_index = SkipListNode::begin(this->node);
					}
					else {
						this->node = nullptr;
						this->inside_index = 0;
					}
				}
				else {
					this->inside_index = nextIndex;
				}

				return *this;
			}

			IterObject& operator--() {
				if (this->node == nullptr) return *this;
				int8_t prevIndex = SkipListNode::prev(this->node, this->inside_index);
				if (prevIndex == -1) {
					this->node = this->node->getLeftNode(0);
					if (SkipListNode::isNonNull(this->node) && this->node != this->skiplist->sentryHead) {
						this->inside_index = SkipListNode::end(this->node);
					}
					else {
						this->node = nullptr;
						this->inside_index = 0;
					}
				}
				else {
					this->inside_index = prevIndex;
				}
				return *this;
			}

			bool operator==(const IterObject& other) const {
				return this->skiplist == other.skiplist && this->node == other.node && this->inside_index == other.inside_index;
			}

			bool operator!=(const IterObject& other) const {
				return !(*this == other);
			}

			explicit operator bool() const {
				return this->node != nullptr;
			}
		};

		IterObject begin() {
			SkipListNode* node = this->sentryHead->getRightNode(0);

			if (node == this->sentryTail) {
				return IterObject(this, nullptr, 0);
			}
			else {
				return IterObject(this, node, SkipListNode::begin(node));
			}
		}

		IterObject end() {
			return IterObject(this, nullptr, 0);
		}

		// reverse
		IterObject rbegin() {
			SkipListNode* node = this->sentryTail->getLeftNode(0);

			if (node == this->sentryHead) {
				return IterObject(this, nullptr, 0);
			}
			else {
				return IterObject(this, node, SkipListNode::end(node));
			}
		}

		IterObject rend() {
			return IterObject(this, nullptr, 0);
		}

		/**
		 * @brief	first element with key >= index
		 *			one descent, then walk the level-0 chain, so a range scan costs O(log n) once instead of per element
		 * @param	index
		 * @return
		 */
		IterObject lowerBound(const index_t index) {
			if (this->width == 0) return IterObject(this, nullptr, 0);

			SkipListNode* node = this->findNodeNoPath(index);
			// now node is the maximum node with baseIndex <= index (or the head sentinel)
			if (node != this->sentryHead && node->baseIndex <= index && SkipListNode::isIndexValid(index - node->baseIndex)) {
				const uint8_t offset = static_cast<uint8_t>(index - node->baseIndex);
				// first set bit at or after offset inside this block
				const bitMap_t candidateBits = node->bitMap & static_cast<bitMap_t>(~((1u << offset) - 1));
				if (candidateBits != 0) {
					return IterObject(this, node, static_cast<int8_t>(bits_ctz64(candidateBits)));
				}
			}

			// nothing at/after index inside the node, the chain is key sorted: next block's begin
			SkipListNode* next = node->getRightNode(0);
			if (next == this->sentryTail || !SkipListNode::isNonNull(next)) return IterObject(this, nullptr, 0);
			return IterObject(this, next, SkipListNode::begin(next));
		}

		/**
		 * @brief	first element with key > index
		 * @param	index
		 * @return
		 */
		IterObject upperBound(const index_t index) {
			if (index == std::numeric_limits<index_t>::max()) return IterObject(this, nullptr, 0);
			return this->lowerBound(index + 1);
		}

#ifdef BBSL_MEMORY_STATS
		/**
		 * @brief	traversal-based memory accounting: walks the level-0 chain and sums the
		 *			requested bytes of every node header, pointer array and element buffer
		 *			(sentries included; slab class rounding counts as allocator overhead, not here).
		 *			compiled only when BBSL_MEMORY_STATS is defined before including this header.
		 */
		struct MemoryUsage {
			size_t bytes;
			uint64_t allocations;
		};

		MemoryUsage memoryUsage() const {
			const auto arrayBytes = [](const SkipListNode* node) {
				return sizeof(uint32_t) * (static_cast<size_t>(node->node_capacity) << 1);
			};

			MemoryUsage usage{ 0, 0 };
			SkipListNode* node = this->sentryHead;
			while (SkipListNode::isNonNull(node) && node != this->sentryTail) {
				usage.bytes += sizeof(SkipListNode) + arrayBytes(node);
				usage.allocations += 2; // node header + pointer array
				if (node->elements != nullptr) {
					usage.bytes += sizeof(value_t) * bbsl::capacity_count;
					++usage.allocations;
				}
				node = node->getRightNode(0);
			}

			// the tail sentinel
			usage.bytes += sizeof(SkipListNode) + arrayBytes(this->sentryTail);
			usage.allocations += 2;
			return usage;
		}
#endif
	};
}