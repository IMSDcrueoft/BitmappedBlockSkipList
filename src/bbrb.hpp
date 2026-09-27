/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <iterator>
#include <limits>

#include "./bits.hpp"

namespace bbrb {
	using bitMap_t = uint8_t;
	constexpr uint64_t capacity_count = sizeof(bitMap_t) * 8;
	constexpr uint64_t index_align = (capacity_count - 1); // Align to capacity limit
	constexpr bitMap_t fullBitMap = std::numeric_limits<bitMap_t>::max(); // every slot is occupied

	/**
	 *  Sparse array built directly on std::map: the tree key is the block's baseIndex,
	 *  and each mapped value is a bitmap-managed block with inline elements (SkipListNode-like).
	 *  Blocks additionally keep prev/next pointers forming a level-0 chain, so traversal
	 *  never descends the tree - it just walks the chain in O(1) steps.
	 *
	 *  Compared with the hand-rolled skip list, the tree routing info (left/right/parent)
	 *  and the block payload live in ONE allocation, so a block lookup costs one dependent
	 *  load per tree level instead of two.
	 */
	template <typename index_t, typename value_t, typename = std::enable_if<std::is_integral_v<index_t>&& std::is_trivial_v<value_t>&& std::is_standard_layout_v<value_t>>>
	class BitmappedBlockRBMap {
	protected:
		/**
		 * @brief	Just data storage, so it's struct.
		 *			baseIndex is cached here (redundant with the map key) so the prev/next
		 *			chain can be walked without touching the tree nodes.
		 */
		struct BlockNode {
			index_t baseIndex = 0;

			BlockNode* prevBlock = nullptr;			//level-0 chain for O(1) traversal
			BlockNode* nextBlock = nullptr;

			bitMap_t bitMap = 0;				//use bitMap to manage

			value_t elements[capacity_count];	//inline elements

		public:
			static bool isIndexValid(const uint64_t index) {
				return index < bbrb::capacity_count;
			}

			static int8_t begin(const BlockNode* block) {
				if (block->bitMap == 0) return -1;
				return bits::ctz64(block->bitMap);
			}

			static int8_t end(const BlockNode* block) {
				if (block->bitMap == 0) return -1;
				uint8_t clzValue = bits::clz64(block->bitMap) - (64 - bbrb::capacity_count);
				return bbrb::capacity_count - clzValue - 1;
			}

			static int8_t next(const BlockNode* block, const int8_t index) {
				if (index >= bbrb::capacity_count) return -1;
				// 0b00101100, index = 2, nextBits = 0b00001011, nextIndex = 3
				bitMap_t nextBits = block->bitMap >> (index + 1);
				if (nextBits == 0) return -1; // ctz will return 64 if the input is 0, so we must directly return -1
				return index + bits::ctz64(nextBits) + 1;
			}

			static int8_t prev(const BlockNode* block, const int8_t index) {
				if (index >= bbrb::capacity_count || index == 0) return -1;
				// 0b00101100, index = 5, prevBits = 0b01100000, prevIndex = 3
				bitMap_t prevBits = block->bitMap << (bbrb::capacity_count - index);
				if (prevBits == 0) return -1;// clz will return 64 if the input is 0, so we must directly return -1
				uint8_t clzValue = bits::clz64(block->bitMap) - (64 - bbrb::capacity_count);
				return index - clzValue - 1;
			}
		};

	protected:
		std::map<index_t, BlockNode> blocks;	//key = baseIndex
		value_t invalid;					//you need an invalid default value
		uint64_t width = 0;					//the element count

		//single-slot quick path cache, mirrors the skip list's leftPathNodes[0]
		mutable BlockNode* lastBlock = nullptr;

		static constexpr index_t blockBaseOf(const index_t index) {
			return static_cast<index_t>(index & ~static_cast<index_t>(index_align));
		}

		void unlinkBlock(typename std::map<index_t, BlockNode>::iterator it) {
			BlockNode& block = it->second;

			//never leave the quick path pointing at a dead block
			if (this->lastBlock == &block) this->lastBlock = nullptr;

			if (block.prevBlock != nullptr) block.prevBlock->nextBlock = block.nextBlock;
			if (block.nextBlock != nullptr) block.nextBlock->prevBlock = block.prevBlock;

			this->blocks.erase(it);
		}

	public:
		/**
		 * @brief
		 * @param invalid invalid value, it should be a default value that is not used in the data
		 */
		BitmappedBlockRBMap(const value_t& invalid) {
			this->invalid = invalid;
		}

		/**
		 * @brief
		 * @param seed unused, kept for benchmark parity with the skip list
		 * @param invalid invalid value, it should be a default value that is not used in the data
		 */
		BitmappedBlockRBMap(const value_t& invalid, uint64_t seed) {
			(void)seed;
			this->invalid = invalid;
		}

		int64_t getLevel() const {
			//no tree height tracking here, kept for benchmark parity
			return 0;
		}

		uint64_t size() const {
			return this->width;
		}

		uint64_t blockCount() const {
			return this->blocks.size();
		}

		/**
		 * @brief
		 * @param index
		 * @return
		 */
		bool has(const index_t index) const {
			if (this->blocks.empty()) return false;

			const index_t base = blockBaseOf(index);
			const uint8_t offset = static_cast<uint8_t>(index - base);

			// quick path: repeated access to the same block
			BlockNode* cached = this->lastBlock;
			if (cached != nullptr && cached->baseIndex == base) {
				return (cached->bitMap >> offset) & 1;
			}

			auto it = this->blocks.find(base);
			if (it == this->blocks.end()) return false;

			this->lastBlock = const_cast<BlockNode*>(&it->second);
			return (it->second.bitMap >> offset) & 1;
		}

		/**
		 * @brief
		 * @param index
		 */
		bool erase(const index_t index) {
			if (this->blocks.empty()) return false;

			const index_t base = blockBaseOf(index);
			const uint8_t offset = static_cast<uint8_t>(index - base);
			const bitMap_t bit = static_cast<bitMap_t>(1) << offset;

			// quick path: repeated access to the same block
			BlockNode* cached = this->lastBlock;
			if (cached != nullptr && cached->baseIndex == base) {
				if (!(cached->bitMap & bit)) return false;

				cached->bitMap &= ~bit;
				--this->width;

				// the block dies, fall back to the tree to fetch its iterator
				if (cached->bitMap == 0) {
					this->unlinkBlock(this->blocks.find(base));
				}
				return true;
			}

			auto it = this->blocks.find(base);
			if (it == this->blocks.end()) return false;

			BlockNode& block = it->second;
			if (!(block.bitMap & bit)) return false;

			block.bitMap &= ~bit;
			--this->width;
			this->lastBlock = &block;

			//remove the block when it becomes empty, no splitting/merging
			if (block.bitMap == 0) {
				this->unlinkBlock(it);
			}
			return true;
		}

		/**
		 * @brief
		 * @param index
		 * @return
		 */
		value_t& operator[](const index_t index) {
			const index_t base = blockBaseOf(index);
			const uint8_t offset = static_cast<uint8_t>(index - base);
			const bitMap_t bit = static_cast<bitMap_t>(1) << offset;

			// quick path: repeated access to the same block, no tree descent at all
			BlockNode* cached = this->lastBlock;
			if (cached != nullptr && cached->baseIndex == base) {
				if (!(cached->bitMap & bit)) {
					cached->elements[offset] = this->invalid;
					cached->bitMap |= bit;
					++this->width;
				}

				return cached->elements[offset];
			}

			//single tree descent: try_emplace finds and inserts at once
			auto insertResult = this->blocks.try_emplace(base);
			auto insertIt = insertResult.first;
			BlockNode& block = insertIt->second;

			if (insertResult.second) {
				//fresh block: fill it and link into the level-0 chain using the tree neighbors
				block.baseIndex = base;
				block.bitMap = 0;
				block.prevBlock = nullptr;
				block.nextBlock = nullptr;

				if (insertIt != this->blocks.begin()) block.prevBlock = &(std::prev(insertIt)->second);
				auto nextIt = std::next(insertIt);
				if (nextIt != this->blocks.end()) block.nextBlock = &nextIt->second;
				if (block.prevBlock != nullptr) block.prevBlock->nextBlock = &block;
				if (block.nextBlock != nullptr) block.nextBlock->prevBlock = &block;
			}

			if (!(block.bitMap & bit)) {
				block.elements[offset] = this->invalid;
				block.bitMap |= bit;
				++this->width;
			}

			this->lastBlock = &block;
			return block.elements[offset];
		}

		/**
		 * @brief
		 * @param index
		 * @return
		 */
		const value_t& operator[](const index_t index) const {
			const index_t base = blockBaseOf(index);
			const uint8_t offset = static_cast<uint8_t>(index - base);

			// quick path: repeated access to the same block
			BlockNode* cached = this->lastBlock;
			if (cached != nullptr && cached->baseIndex == base) {
				return ((cached->bitMap >> offset) & 1) ? cached->elements[offset] : this->invalid;
			}

			auto it = this->blocks.find(base);
			if (it != this->blocks.end()) {
				this->lastBlock = const_cast<BlockNode*>(&it->second);
				if ((it->second.bitMap >> offset) & 1) {
					return it->second.elements[offset];
				}
			}

			return this->invalid;
		}

	public:
		template<typename Func>
		void forEach(Func func) const {
			const BlockNode* block = this->blocks.empty() ? nullptr : &this->blocks.begin()->second;
			while (block != nullptr) {
				// dense fast path: every slot is occupied, iterate sequentially without bit tests
				if (block->bitMap == bbrb::fullBitMap) {
					const index_t base = block->baseIndex;
					const value_t* elements = block->elements;
					for (uint8_t i = 0; i < bbrb::capacity_count; ++i) {
						func(elements[i], base + i);
					}
				}
				else {
					for (int8_t i = BlockNode::begin(block); i != -1; i = BlockNode::next(block, i)) {
						func(block->elements[i], block->baseIndex + i);
					}
				}
				block = block->nextBlock;
			}
		}

		template<typename Func>
		bool some(Func func) const {
			const BlockNode* block = this->blocks.empty() ? nullptr : &this->blocks.begin()->second;
			while (block != nullptr) {
				// dense fast path: every slot is occupied, iterate sequentially without bit tests
				if (block->bitMap == bbrb::fullBitMap) {
					const index_t base = block->baseIndex;
					const value_t* elements = block->elements;
					for (uint8_t i = 0; i < bbrb::capacity_count; ++i) {
						if (func(elements[i], base + i)) return true;
					}
				}
				else {
					for (int8_t i = BlockNode::begin(block); i != -1; i = BlockNode::next(block, i)) {
						if (func(block->elements[i], block->baseIndex + i)) return true;
					}
				}
				block = block->nextBlock;
			}
			return false;
		}

		template<typename Func>
		bool every(Func func) const {
			const BlockNode* block = this->blocks.empty() ? nullptr : &this->blocks.begin()->second;
			while (block != nullptr) {
				// dense fast path: every slot is occupied, iterate sequentially without bit tests
				if (block->bitMap == bbrb::fullBitMap) {
					const index_t base = block->baseIndex;
					const value_t* elements = block->elements;
					for (uint8_t i = 0; i < bbrb::capacity_count; ++i) {
						if (!func(elements[i], base + i)) return false;
					}
				}
				else {
					for (int8_t i = BlockNode::begin(block); i != -1; i = BlockNode::next(block, i)) {
						if (!func(block->elements[i], block->baseIndex + i)) return false;
					}
				}
				block = block->nextBlock;
			}
			return true;
		}

		class IterObject {
		private:
			const BitmappedBlockRBMap* owner = nullptr;
			BlockNode* node = nullptr;
			int8_t inside_index = 0;

		public:
			IterObject() = delete;
			IterObject(const BitmappedBlockRBMap* owner, BlockNode* node, int8_t inside_index)
				: owner(owner), node(node), inside_index(inside_index) {
			}

			~IterObject() = default;

			const value_t& operator*() const {
				// we don't know if user will call operator* when the block is deleted, so we return invalid in this case
				return ((this->node->bitMap >> this->inside_index) & 1) ? this->node->elements[this->inside_index] : this->owner->invalid;
			}

			index_t key() const {
				return (this->node != nullptr) ? (this->node->baseIndex + this->inside_index) : 0;
			}

			bool setValue(const value_t& value) {
				if (this->node == nullptr) return false;
				this->node->elements[this->inside_index] = value;
				this->node->bitMap |= (static_cast<bitMap_t>(1) << this->inside_index);
				return true;
			}

			IterObject& operator++() {
				if (this->node == nullptr) return *this;
				int8_t nextIndex = BlockNode::next(this->node, this->inside_index);

				if (nextIndex == -1) {
					this->node = this->node->nextBlock;
					if (this->node != nullptr) {
						this->inside_index = BlockNode::begin(this->node);
					}
					else {
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
				int8_t prevIndex = BlockNode::prev(this->node, this->inside_index);
				if (prevIndex == -1) {
					this->node = this->node->prevBlock;
					if (this->node != nullptr) {
						this->inside_index = BlockNode::end(this->node);
					}
					else {
						this->inside_index = 0;
					}
				}
				else {
					this->inside_index = prevIndex;
				}
				return *this;
			}

			bool operator==(const IterObject& other) const {
				return this->owner == other.owner && this->node == other.node && this->inside_index == other.inside_index;
			}

			bool operator!=(const IterObject& other) const {
				return !(*this == other);
			}

			explicit operator bool() const {
				return this->node != nullptr;
			}
		};

		IterObject begin() {
			if (this->blocks.empty()) return IterObject(this, nullptr, 0);
			BlockNode* block = &this->blocks.begin()->second;
			return IterObject(this, block, BlockNode::begin(block));
		}

		IterObject end() {
			return IterObject(this, nullptr, 0);
		}

		// reverse
		IterObject rbegin() {
			if (this->blocks.empty()) return IterObject(this, nullptr, 0);
			BlockNode* block = &this->blocks.rbegin()->second;
			return IterObject(this, block, BlockNode::end(block));
		}

		IterObject rend() {
			return IterObject(this, nullptr, 0);
		}

		/**
		 * @brief	first element with key >= index
		 *			one tree lower_bound to land on the right block, then walk the level-0 chain,
		 *			so a range scan costs O(log n) once instead of per element
		 * @param	index
		 * @return
		 */
		IterObject lowerBound(const index_t index) {
			if (this->blocks.empty()) return IterObject(this, nullptr, 0);

			auto it = this->blocks.lower_bound(blockBaseOf(index));
			if (it == this->blocks.end()) return IterObject(this, nullptr, 0);

			BlockNode* block = &it->second;
			//block->baseIndex may exceed index (gap), search the block from bit 0 in that case
			const uint8_t offset = (block->baseIndex >= index) ? 0 : static_cast<uint8_t>(index - block->baseIndex);
			const bitMap_t candidateBits = (offset < bbrb::capacity_count)
				? (block->bitMap & static_cast<bitMap_t>(~((1u << offset) - 1)))
				: 0;

			if (candidateBits != 0) {
				return IterObject(this, block, static_cast<int8_t>(bits::ctz64(candidateBits)));
			}

			//nothing at/after index inside this block, the chain is already key-sorted
			block = block->nextBlock;
			if (block == nullptr) return IterObject(this, nullptr, 0);
			return IterObject(this, block, BlockNode::begin(block));
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
	};
}
