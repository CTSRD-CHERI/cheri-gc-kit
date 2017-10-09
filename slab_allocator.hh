/*-
 * Copyright (c) 2017 David T Chisnall
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract FA8750-10-C-0237
 * ("CTSRD"), as part of the DARPA CRASH research programme.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "utils.hh"
#include "config.hh"
#include "lock.hh"
#include "page.hh"
#include "BitSet.hh"
#include "bucket_size.hh"
#include <stdio.h>
#include <bitset>
#include <memory>
#include <stdlib.h>
#include <inttypes.h>

#undef fprintf

#define unlikely(x) __builtin_expect(x, false)
#define likely(x) __builtin_expect(x, true)

namespace {

/**
 * Reimplementation of `std::max`.  This is to work around the fact that
 * `std::max` is not marked constexpr in libc++.
 */
template<typename T>
constexpr T max(T a, T b)
{
	return a > b ? a : b;
}


template<typename Header>
struct Allocator;
/**
 * Class encapsulating a large table indexing from address to allocator.  We
 * rely on the VM subsystem to lazily allocate the pages in the array and
 * assign large regions (e.g. 8MiB) to allocators, allowing the map from region
 * to allocator to be small.
 *
 * This design is inspired by SuperMalloc.
 */
template<size_t ChunkBits, size_t ASBits, size_t PageSize, typename Header>
class PageMetadata
{
	/**
	 * The array that stores the allocators.  This consumer a lot of virtual
	 * address space.
	 */
	std::array<Allocator<Header>*, 1ULL<<(ASBits - ChunkBits)> array;
	/**
	 * Convenience type for the current template instantiation.
	 */
	using self_type = PageMetadata<ChunkBits, ASBits, PageSize, Header>;
	static const int address_bits = sizeof(size_t) * 8;
	/**
	 * The index in the array of a virtual address.  Allocators are strongly
	 * aligned, so we ignore the top bits in the address space that aren't
	 * mapped (for example, the top 16 if we have only a 48-bit virtual address
	 * space).  We then ignore the low bits, that just give you an offset
	 * within a chunk.
	 */
	size_t index_for_vaddr(vaddr_t a)
	{
		// Trim off any high bits that might accidentally be set.
		a <<= address_bits - ASBits;
		a >>= address_bits - ASBits;
		a >>= log2<chunk_size>();
		return a;
	}
	/**
	 * Private constructor, prevents construction other than via `create`.
	 */
	PageMetadata() {}
	public:
	/**
	 * Create an instance of this class.  It is important to avoid running the
	 * constructors, because they'd write about 1GB of zeroes.
	 */
	static self_type *create()
	{
		// Assert that we didn't accidentally add a vtable for this, so that we
		// can allocate it safely in zeroed memory.
		static_assert(!std::is_polymorphic<self_type>::value,
			"Page metadata class must not have a vtable!\n");
		auto *pa = (PageAllocator<self_type>().allocate(1));
		ASSERT(pa);
		return pa;
	}
	/**
	 * Return the allocator for a given address, or null if there is no
	 * allocator yet.
	 */
	Allocator<Header> *allocator_for_address(vaddr_t addr)
	{
		return array.at(index_for_vaddr(addr));
	}
	/**
	 * Set the allocator for a specific address.
	 */
	void set_allocator_for_address(Allocator<Header> *allocator, vaddr_t addr)
	{
		ASSERT((allocator == nullptr) || (allocator_for_address(addr) == nullptr));
		array.at(index_for_vaddr(addr)) = allocator;
	}
};

/**
 * Fast iterator state, used with iterators that request multiple objects from
 * the underlying collections in a single call.
 */
template<typename Header>
struct allocator_fast_iterator
{
	/**
	 * The index of the end of the buffer.
	 */
	size_t end = 0;
	/**
	 * The index within the buffer of the current element.
	 */
	size_t buffer_idx = 0;
	/**
	 * The length of data in the buffer.
	 */
	size_t buffer_length = 0;
	/**
	 * The number of elements that the buffer has space for.
	 */
	static const size_t buffer_size = 64;
	/**
	 * The type of each element.
	 */
	using alloc = std::pair<void*, Header*>;
	/**
	 * Internal storage for the buffer elements.
	 */
	std::array<alloc, buffer_size> buffer;
	/**
	 * Compare two iterators.
	 */
	bool operator!=(const allocator_fast_iterator &other)
	{
		return (end != other.end) || (buffer_idx != other.buffer_idx);
	}

};

/**
 * Interface for allocators.  This provides a generic interface for all allocators.
 */
template<typename Header>
struct Allocator
{
	std::atomic<Allocator<Header>*> next;
	/**
	 * Allocate an object of the specified size.  For small allocations, this
	 * will always return the fixed size that the allocator can handle.
	 */
	virtual void *alloc(size_t) { return nullptr; }
	/**
	 * Returns the size of allocations from this pool, or zero if this is not a
	 * fixed-size allocator.
	 */
	virtual size_t object_size(void *) { return 0; }
	/**
	 * Free an object in this allocator.  Returns true if the allocator has
	 * just transitioned from a full state to a non-full state, at which point
	 * it can be added to a list of allocators from which to allocate new
	 * objects of this size.
	 */
	virtual bool free(void *) { return false; }
	/**
	 * Return whether the allocator is full (i.e. unable to allocate anything
	 * else).
	 */
	virtual bool full() { return true; }
	/**
	 * Returns the bucket to which this allocator corresponds.  If this is not
	 * a fixed-size allocator, this returns -1.
	 */
	virtual int bucket() const { return -1; }
	/**
	 * Returns a pointer to the allocation for the address and, via the second
	 * argument, a pointer to the header for the object.
	 *
	 * Note that fixed-sized allocators may not give the bounds of the object,
	 * but rather the bounds of a fixed-size allocation.
	 */
	virtual void *allocation_for_address(vaddr_t, Header *&) { return nullptr; }
	/**
	 * Fill the provided fast iteration state.  The index in the state should
	 * be updated.
	 */
	virtual void fill_fast_iterator(allocator_fast_iterator<Header> &) {}
};

/**
 * Template for storing either an array of headers, or nothing if the
 * header type is void.  This allows the small allocator template to be
 * instantiated with or without per-object headers, without suffering any
 * space overhead.  In an optimised build, there should also be no run-time
 * overhead (though there will be more work for the optimisers to delete
 * dead code).
 *
 * FIXME: This should be moved into a superclass that's shared with
 * `SmallAllocationHeader`, rather than copied and pasted.
 */
template<typename Header, size_t Size, class Enable = void>
struct HeaderList {};
/**
 * Template specialisation with a void header type.  This provides a
 * stub implementation that has a size of zero and a trivial method.
 */
template<typename Header, size_t Size>
struct HeaderList<Header, Size, typename std::enable_if<std::is_void<Header>::value>::type>
{
	/**
	 * A struct containing no members has size 1, but a struct containing a
	 * zero-length array has size 0 in the Itanium ABI.
	 */
	int unused[0];
	/**
	 * If the header type is `void`, always return null for the header at
	 * any index.
	 */
	Header *header_at_index(size_t idx)
	{
		return nullptr;
	}
};
/**
 * Template specialisation for non-`void` header types.
 */
template<typename Header, size_t Size>
struct HeaderList<Header, Size, typename std::enable_if<!std::is_void<Header>::value>::type>
{
	/**
	 * The array storing the headers.
	 */
	std::array<Header, Size> array;
	/**
	 * Accessor, returns the header at the specified index.
	 */
	Header *header_at_index(size_t idx)
	{
		cheri::capability<Header> h = &array.at(idx);
		h.set_bounds(1);
		return h.get();
	}
};

// Compile-time check that the header list doesn't use any space when the
// header type is void.
static_assert(sizeof(HeaderList<void, 100>) == 0, "Compiler has odd ABI!\n");

/**
 * The small allocation header.  This contains all of the metadata for a small
 * allocator, but not the memory that will be allocated.
 *
 * Small allocators allocate fixed-sized chunks of memory from a pool that is
 * split into fixed-sized folios.
 */
template<size_t AllocSize, size_t ChunkSize, typename Header>
class SmallAllocationHeader
{
	/**
	 * Compute the greatest common divisor of two values, as a compile-time
	 * constant.  This is used to determine the folio size.
	 */
	static constexpr size_t gcd(size_t a, size_t b)
	{
		return b == 0 ? a : gcd(b, a % b);
	}
	public:
	/**
	 * To avoid having to track allocations that span a page boundary, we use a
	 * folio that is the least common multiple of the page size and alloc size.
	 */
	static const size_t folio_size = page_size * AllocSize / gcd(page_size, AllocSize);
	/**
	 * The number of allocations in each folio.
	 */
	static const size_t allocs_per_folio = folio_size / AllocSize;
	/**
	 * The number of folios managed by this allocator.
	 */
	static const int folios_per_chunk = ChunkSize / folio_size;
	/**
	 * The total number of allocations per chunk.
	 */
	static const int allocs_per_chunk = allocs_per_folio * folios_per_chunk;
	/**
	 * Lock protecting this allocator.
	 */
	UncontendedSpinlock<long> lock;
	// FIXME: These should pass!
	//static_assert(allocs_per_folio > 1, "Folios the same size as allocs don't make sense");
	//static_assert(ChunkSize / folio_size > 1, "Chunks that only hold one folio don't make sense!");
	/**
	 * Metadata describing a folio.  Folios that have free space are stored in
	 * a free list.  There is one free list for each number of possible free
	 * elements in a list.  This allows folios to be trivially sorted by the
	 * amount of free space (allocation moves a folio from one free list to the
	 * next).  We aim to fill allocations from the most-full folio, to minimise
	 * internal fragmentation.
	 *
	 * These linked lists use the index in the `folios` array, rather
	 * than pointers.  Doing so allows us to use 16-bit integers for these
	 * values, on any architecture.
	 */
	struct folio
	{
		/**
		 * Flag representing a not-present list element.
		 */
		static const uint16_t not_present = 0xffff;
		/**
		 * The index in the `folios` array of the previous folio in the
		 * list.
		 */
		uint16_t prev;
		/**
		 * The index in the `folios` array of the next folio in this list.
		 */
		uint16_t next;
		/**
		 * The number of free folios in this list.
		 */
		uint16_t free_count;
		/**
		 * Bitfield of the free allocations in this list.
		 *
		 * Note: This is misnamed.  Bits are *set* for allocated space and
		 * *unset* for free, so this should really be called `allocated` or
		 * something similar.
		 */
		BitSet<allocs_per_folio> free;
	};
	/**
	 * All of the folio metadata.
	 */
	std::array<folio, folios_per_chunk> folios;
	// Compile-time checks that uint16_t is actually big enough.
	static_assert(folios_per_chunk < ((1<<16) - 1),
			"Too many folios to use uint16_t for folios list indexes");
	static_assert(allocs_per_folio < ((1<<16) - 1),
			"Too many allocs per folio to use uint16_t for free count");
	/**
	 * List of headers.
	 */
	HeaderList<Header, allocs_per_chunk> headers;
	public:
	/**
	 * Returns the header at the specified index.
	 */
	Header *header_at_index(size_t idx)
	{
		return headers.header_at_index(idx);
	}
	/**
	 * A conservative approximation of the bucket that has the most free space.
	 * The bucket with the most free space will always be after this, but it
	 * may not be exactly here.
	 */
	uint16_t free_head = 1;
	/**
	 * The total number of free allocations in this allocator.
	 */
	uint32_t free_allocs_total;
	// Check that the number of list entries is small enough that we can store
	// all of the allocations.
	static_assert(folios_per_chunk * allocs_per_folio < 1ULL<<(sizeof(free_allocs_total)*8), "Index value too small");
	/**
	 * An array of indexes into the `folios` array.  Each entry in this represents one 
	 */
	std::array<uint16_t, allocs_per_folio+1> free_lists;
	/**
	 * Constructor.  The parameter is the size of the subclass (or the size of
	 * this class, if there is no subclass).  The allocator reserves all of the
	 * allocations that would cover that space.
	 *
	 * Note: Ideally, we wouldn't reserve space for those, but then we end up
	 * having to solve a constraints problem at compile time (shrinking the
	 * size also shrinks the metadata size).  This is possible with templates,
	 * but it's not trivial.
	 */
	SmallAllocationHeader(size_t size)
	{
		for (auto i=0 ; i<allocs_per_folio ; i++)
		{
			free_lists[i] = folio::not_present;
		}
		const int folios_for_header = ((size + (folio_size-1)) / folio_size) + 5;
		// FIXME: We probably shouldn't reserve the entire folio.
		for (uint16_t i=0 ; i<folios_for_header ; i++)
		{
			folio &l = folios[i];
			l.free_count = 0;
			l.prev = i-1;
			l.next = i+1;
		}
		for (uint16_t i=folios_for_header ; i<folios_per_chunk ; i++)
		{
			folio &l = folios[i];
			l.prev = i-1;
			l.next = i+1;
			l.free_count = allocs_per_folio;
		}
		free_allocs_total = (folios_per_chunk-folios_for_header) * allocs_per_folio;
		// The last freelist (for pages that are completely empty)
		free_lists[allocs_per_folio] = folios_for_header;
		free_head = allocs_per_folio;
		folios[folios_for_header].prev = folio::not_present;
		folios[folios_per_chunk-1].next = folio::not_present;
		// The first freelist (for pages that are completely full)
		free_lists[0] = 0;
		folios[folios_for_header-1].next = folio::not_present;
		folios[0].prev = folio::not_present;
#if 0
		fprintf(stderr, "Header for %d byte allocations is %d bytes\n", (int)AllocSize, (int)size);
		fprintf(stderr, "%d folios of %d bytes (%d allocs per folio)\n", (int)folios_per_chunk, (int)folio_size, (int)allocs_per_folio);
		fprintf(stderr, "Each list entry is %d bytes\n", (int)sizeof(folio));
		fprintf(stderr, "Overhead: %.2lf%%\n", (double)size/ChunkSize*100);
#endif
	}
	/**
	 * Marks an allocation as free.
	 */
	bool free_allocation(size_t offset)
	{
		// FIXME: We should abort if offset % AllocSize is non-zero
		int idx = offset / AllocSize;
		int folio_idx = offset / folio_size;
		int alloc_in_folio = idx % allocs_per_folio;
		folio &l = folios[folio_idx];
		do {} while (!try_run_locked(lock, [&]()
			{
				remove_list_entry(folio_idx);
				l.free_count++;
				ASSERT(l.free[alloc_in_folio]);
				l.free.clear(alloc_in_folio);
				ASSERT(!l.free[alloc_in_folio]);
				// TODO: By placing this back at the head of the list, we
				// ensure that it will be reallocated quickly.  To reduce the
				// danger of use-after-free, we probably want the opposite
				// policy (note that this will also have to be done with
				// caching)
				insert_list_entry(folio_idx);
				free_allocs_total--;
				if (l.free_count == alloc_in_folio)
				{
					cheri::capability<void> folio_pages(reinterpret_cast<void*>(this));
					folio_pages.set_offset(offset);
					folio_pages.set_bounds(AllocSize * allocs_per_folio);
					zero_pages(folio_pages);
				}
			}));
		return free_allocs_total == 0;
	}
	/**
	 * Return the offset of a free allocation and mark it as allocated.
	 * Returns -1 if it is impossible to satisfy the allocation.  This can
	 * happen even if the caller checks whether this is full, because another
	 * thread may call `reserve_allocation` in parallel.
	 */
	size_t reserve_allocation()
	{
		uint16_t folio_index = folio::not_present;
		size_t offset = 0;
		// Grab the lock and try to find an allocation.
		do {} while (!try_run_locked(lock, [&]()
			{
				// FIXME: Figure out why the free head isn't being put in the
				// correct place.
				free_head = 1;
				// Scan back along free lists to find the most-full folio that
				// contains some free space.
				while (free_lists[free_head] == folio::not_present)
				{
					free_head++;
					if (free_head > allocs_per_folio)
					{
						return;
					}
				}
				ASSERT(free_head != 0);
				if (free_head == 0)
				{
					return;
				}
				ASSERT(free_lists[free_head] != folio::not_present);
				folio_index = free_lists[free_head];
				folio &l = folios[folio_index];
				ASSERT(l.free_count != 0);
				remove_list_entry(folio_index);
				l.free_count--;
				insert_list_entry(folio_index);
				offset = l.free.first_zero();
				if (offset >= allocs_per_folio)
				{
					fprintf(stderr, "Free list head is %d (%d), found full folio on free list\n", (int)free_head, (int)allocs_per_folio);
				}
				ASSERT(offset < allocs_per_folio);
				free_head--;
				ASSERT(!l.free[offset]);
				l.free.set(offset);
				ASSERT(l.free[offset]);
				free_allocs_total--;
			}));
		if (folio_index == folio::not_present)
		{
			return -1;
		}
		ASSERT(folio_index * folio_size + (offset * AllocSize) > sizeof(*this));
		return folio_index * folio_size + (offset * AllocSize);
	}
	/**
	 * Remove an entry from the free list that currently contains it.
	 */
	__attribute__((always_inline))
	void remove_list_entry(uint16_t folio_idx)
	{
		folio &l = folios[folio_idx];
		if (l.prev == folio::not_present)
		{
			free_lists[l.free_count] = l.next;
		}
		else
		{
			folios[l.prev].next = l.next;
		}
		if (l.next != folio::not_present)
		{
			folios[l.next].prev = l.prev;
		}
	}
	/**
	 * Insert a folio into the correct free list.
	 */
	__attribute__((always_inline))
	void insert_list_entry(uint16_t folio_idx)
	{
		folio &l = folios[folio_idx];
		l.prev = folio::not_present;
		l.next = free_lists[l.free_count];
		if (l.next != folio::not_present)
		{
			folios[free_lists[l.free_count]].prev = folio_idx;
		}
		free_lists[l.free_count] = folio_idx;
	}
	template<size_t sz>
	size_t allocations(std::array<size_t, sz> &vals, size_t start)
	{
		size_t written = 0;
		while ((written < sz) && (start < allocs_per_chunk))
		{
			size_t folio_idx = start / allocs_per_folio;
			size_t start_idx = start % allocs_per_folio;
			size_t out_idx = start - start_idx;
			start += allocs_per_folio - start_idx;
			if (folio_idx >= folios_per_chunk)
			{
				break;
			}
			folio &f = folios[folio_idx];
			if (f.free_count == allocs_per_folio)
			{
				continue;
			}
			// Find each set bit in the bitmap
			for (int i=start_idx ; i<allocs_per_folio ; i=f.free.one_after(i))
			{
				if (written == sz)
				{
					return written;
				}
				if (f.free[i])
				{
					vals.at(written++) = out_idx + i;
				}
			}
		}
		return written;
	}
};

/**
 * Large allocation header.  Large allocations have roughly the same structure
 * as small allocators, but don't attempt to reduce 
 */
template<size_t AllocSize, size_t ChunkSize, typename Header>
class LargeAllocationHeader
{
	public:
	/**
	 * The number of allocations in each folio.
	 */
	static const size_t allocs_per_chunk = ChunkSize / AllocSize;
	/**
	 * Lock protecting this allocator.
	 */
	UncontendedSpinlock<long> lock;
	/**
	 * Bitfield of the free allocations in this list.
	 *
	 * Note: This is misnamed.  Bits are *set* for allocated space and
	 * *unset* for free, so this should really be called `allocated` or
	 * something similar.
	 */
	BitSet<allocs_per_chunk> free;
	/**
	 * List of headers.
	 */
	HeaderList<Header, allocs_per_chunk> headers;
	public:
	/**
	 * Returns the header at the specified index.
	 */
	Header *header_at_index(size_t idx)
	{
		return headers.header_at_index(idx);
	}
	/**
	 * The total number of free allocations in this allocator.
	 */
	uint32_t free_allocs_total;
	/**
	 * Constructor.  The parameter is the size of the subclass (or the size of
	 * this class, if there is no subclass).  The allocator reserves all of the
	 * allocations that would cover that space.
	 *
	 * Note: Ideally, we wouldn't reserve space for those, but then we end up
	 * having to solve a constraints problem at compile time (shrinking the
	 * size also shrinks the metadata size).  This is possible with templates,
	 * but it's not trivial.
	 */
	LargeAllocationHeader(size_t size)
	{
		size_t allocs_for_header = (size + AllocSize - 1) / AllocSize;
		for (auto i=0 ; i<allocs_for_header ; i++)
		{
			free.set(i);
		}
		free_allocs_total = allocs_per_chunk - allocs_for_header;
	}
	/**
	 * Marks an allocation as free.
	 */
	void free_allocation(size_t offset)
	{
		// FIXME: We should abort if offset % AllocSize is non-zero
		do {} while (!try_run_locked(lock, [&]()
			{
				int idx = offset / AllocSize;
				free.clear(idx);
				free_allocs_total++;
				cheri::capability<void> pages(reinterpret_cast<void*>(this));
				pages.set_offset(offset);
				pages.set_bounds(AllocSize);
				zero_pages(pages);
			}));
	}
	/**
	 * Return the offset of a free allocation and mark it as allocated.
	 * Returns -1 if it is impossible to satisfy the allocation.  This can
	 * happen even if the caller checks whether this is full, because another
	 * thread may call `reserve_allocation` in parallel.
	 */
	size_t reserve_allocation()
	{
		size_t offset = -1;
		// Grab the lock and try to find an allocation.
		do {} while (!try_run_locked(lock, [&]()
			{
				if (free_allocs_total > 0)
				{
					offset = free.first_zero();
					free.set(offset);
					free_allocs_total--;
				}
			}));
		return offset * AllocSize;
	}
	template<size_t sz>
	size_t allocations(std::array<size_t, sz> &vals, size_t start)
	{
		size_t written = 0;
		// Find each set bit in the bitmap
		for (int i=start ; i<allocs_per_chunk ; i=free.one_after(i))
		{
			if (written == sz)
			{
				return written;
			}
			if (free[i])
			{
				vals.at(written++) = i;
			}
		}
		return written;
	}
};


/**
 * Fixed-sized allocator.  This implements the methods defined in the abstract
 * allocator, but delegates most of the implementation to the AllocHeader.
 *
 * This class is used for small, medium, and large allocations as described in
 * the SuperMalloc paper.  The only difference between small and medium
 * allocators is the way in which their size is created, large allocators have
 * simpler metadata.  
 */
template<size_t AllocSize, typename ChunkHeader, typename Header>
class FixedAllocator final : public Allocator<Header>,
                             public ChunkHeader,
                             public PageAllocated<FixedAllocator<AllocSize, ChunkHeader, Header>>
{
	using self_type = FixedAllocator<AllocSize, ChunkHeader, Header>;
	/**
	 * Returns the size bucket for this allocator.
	 */
	int bucket() const override
	{
		return bucket_for_size(AllocSize);
	}
	/**
	 * Returns whether the bucket is free.
	 */
	bool full() override
	{
		return ChunkHeader::free_allocs_total == 0;
	}
	/**
	 * Allocate a new object.  The bounds of the returned allocation will be
	 * constrained by the argument, but the amount of space returned is
	 * determined by the template parameter for this allocator.
	 */
	void *alloc(size_t sz) override
	{
		ASSERT(sz <= AllocSize);
		size_t offset = ChunkHeader::reserve_allocation();
		if (offset == -1)
		{
			return nullptr;
		}
		cheri::capability<char> ptr(reinterpret_cast<char*>(this) + (offset));
		ptr.set_bounds(sz);
		return reinterpret_cast<void*>(ptr.get());
	};
	/**
	 * The size of all objects in this allocator is fixed.
	 */
	size_t object_size(void *) override
	{
		return AllocSize;
	};
	/**
	 * Find the allocation for a given address and the object header.  The
	 * object header is stored non-contiguously with the object, to avoid the
	 * need to add alignment padding.
	 */
	void *allocation_for_address(vaddr_t addr, Header *&header) override
	{
		addr -= (vaddr_t)this;
		size_t idx = addr / AllocSize;
		header = ChunkHeader::header_at_index(idx);
		cheri::capability<char> ptr(reinterpret_cast<char*>(this) + (idx * AllocSize));
		ptr.set_bounds(AllocSize);
		return reinterpret_cast<void*>(ptr.get());
	}
	/**
	 * Collect the objects and headers for iteration.
	 */
	void fill_fast_iterator(allocator_fast_iterator<Header> &i) override
	{
		if (i.end == 0)
		{
			i.end = (sizeof(*this) + AllocSize-1) / AllocSize;
		}
		vaddr_t start = (vaddr_t)this;
		std::array<size_t, allocator_fast_iterator<Header>::buffer_size> buf;
		i.buffer_length = ChunkHeader::allocations(buf, i.end);
		i.buffer_idx = 0;
		for (int idx=0 ; idx<i.buffer_length ; idx++)
		{
			auto &a = i.buffer.at(idx);
			i.end = buf.at(idx)+1;
			a.first = allocation_for_address(start + (buf.at(idx)*AllocSize), a.second);
		}
	}
	/**
	 * Free the object.
	 */
	bool free(void *ptr) override
	{
		size_t offset = reinterpret_cast<char*>(ptr) - reinterpret_cast<char*>(this);
		ASSERT(offset < chunk_size);
		ChunkHeader::free_allocation(offset);
		return false;
	};
	/**
	 * Constructor.  Reserves space for all of the metadata.
	 */
	FixedAllocator() : ChunkHeader(sizeof(*this))
	{
		ASSERT(!full());
	}
	public:
	/**
	 * Create an instance of this object.  This will reserve space for a chunk,
	 * and then initialise the object at the start of this space.
	 */
	static self_type *create()
	{
		static_assert(chunk_size > sizeof(self_type),
		              "Metadata is bigger than chunk!");
		char *p = PageAllocator<char>().allocate(chunk_size);
		return ::new (p) self_type();
	}
};

/**
 * Small allocator.  Handles allocations that are either much smaller than a
 * page or a few pages.  These are arranged in folios, and when an entire folio
 * is freed the underlying pages can be returned to the OS.
 */
template<size_t AllocSize, typename Header>
using SmallAllocator = FixedAllocator<AllocSize,
                                      SmallAllocationHeader<AllocSize, chunk_size, Header>,
                                      Header>;

/**
 * Large allocator.  Handles objects that are from 32KB to half of the size of
 * a chunk.  Objects are allocated as a range of pages and are returned to the
 * OS as soon as they're no longer needed.
 */
template<size_t AllocSize, typename Header>
using LargeAllocator = FixedAllocator<AllocSize,
                                      LargeAllocationHeader<AllocSize, chunk_size, Header>,
                                      Header>;

template<typename Header>
struct Buckets;

/**
 * Huge allocator.  Allocates objects as a multiple of page size.  The huge
 * allocator is responsible for objects that are more than half the size of a
 * chunk.  These are allocated directly by mapping new pages from the OS.
 */
template<typename Header>
struct HugeAllocator final : public Allocator<Header>
{
	/**
	 * Huge allocators must register themselves with the page metadata array.
	 */
	using PageMetadataArray = PageMetadata<chunk_size_bits, address_space_size_bits, page_size, Header>;
	/**
	 * The pointer that this allocator is responsible for.
	 * 
	 * Each huge allocator is responsible for only a single multi-page allocation.
	 */
	std::atomic<void*> allocation;
	/**
	 * The size of this allocation.
	 */
	size_t size = 0;
	/**
	 * The metadata array that's responsible for mapping from allocations to
	 * allocators.  Huge allocators are responsible for updating this mapping
	 * on each allocation.
	 */
	PageMetadataArray &metadata_array;
	/**
	 * The owner for this allocator.
	 */
	Buckets<Header> &owner;
	/**
	 * Either the header, or a zero-sized allocation, depending on whether we
	 * have a header type.
	 */
	typename std::conditional<std::is_void<Header>::value, char[0], Header>::type header;
	/**
	 * Allocate a huge object.  Rounds up to a multiple of page size.
	 */
	void *alloc(size_t sz) override
	{
		// FIXME: We should add some entropy to the start address
		sz = roundUp<page_size>(sz);
		void *a = reinterpret_cast<void*>(PageAllocator<char>().allocate(sz));
		void *expected = nullptr;
		if (allocation.compare_exchange_strong(expected, a))
		{
			vaddr_t addr = (vaddr_t)a;
			for (vaddr_t i=0 ; i<sz ; i+=chunk_size)
			{
				metadata_array.set_allocator_for_address(this, addr + i);
			}
			size = sz;
			return a;
		}
		PageAllocator<char>().deallocate(reinterpret_cast<char*>(a), sz);
		return nullptr;
	}
	/**
	 * Returns the object size.
	 */
	size_t object_size(void *) override
	{
		return size;
	}
	/**
	 * Free an object in this allocator.  Returns true if the allocator has
	 * just transitioned from a full state to a non-full state, at which point
	 * it can be added to a list of allocators from which to allocate new
	 * objects of this size.
	 */
	bool free(void *ptr) override
	{
		cheri::capability<void> cap(allocation);
		if (cap.contains(cheri::base(ptr)))
		{
			void *alloc = allocation.load();
			// This can be nullptr if two threads race to free the same allocation.
			// This should never happen in a GC environment.
			if (!alloc)
			{
				return false;
			}
			// After this point, this allocator should not be found by any iterators.
			if (allocation.compare_exchange_strong(alloc, nullptr))
			{
				vaddr_t addr = (vaddr_t)alloc;
				for (vaddr_t i=0 ; i<size ; i+=chunk_size)
				{
					metadata_array.set_allocator_for_address(nullptr, addr + i);
				}
				// After this point, the allocator can't be found by mapping
				// from an allocation to an allocator.  In an environment with
				// manual memory management, this is a race, but is okay
				// because it can only be triggered by a use-after-free, which
				// is undefined.
				// In an environment with garbage collection, only the GC
				// should call this method and should do so only after
				// eliminating all of the pointers from which this object can
				// be looked up.  It is therefore safe to delete the object
				// after unmapping the memory.
				PageAllocator<char>().deallocate(reinterpret_cast<char*>(alloc), size);
				return delete_self();
			}
		}
		return false;
	}
	/**
	 * Return whether the allocator is full (i.e. unable to allocate anything
	 * else).
	 */
	bool full() override
	{
		return allocation != nullptr;
	}
	/**
	 * Returns a pointer to the allocation for the address and, via the second
	 * argument, a pointer to the header for the object.
	 *
	 * Note that fixed-sized allocators may not give the bounds of the object,
	 * but rather the bounds of a fixed-size allocation.
	 */
	void *allocation_for_address(vaddr_t addr, Header *&h) override
	{
		cheri::capability<void> cap(allocation);
		if (cap.contains(addr))
		{
			h = &header;
			return allocation;
		}
		return nullptr;
	}
	/**
	 * Fill the provided fast iteration state.  This allocator is responsible
	 * for a single allocation.
	 */
	void fill_fast_iterator(allocator_fast_iterator<Header> &i) override
	{
		i.buffer_idx = 0;
		if ((i.end == 0) && (allocation != nullptr))
		{
			i.buffer[0] = { allocation.load(), &header };
			i.buffer_length = 1;
			i.end = 1;
		}
	}
	/**
	 * Delete this object.  This is called when the object managed by this
	 * allocator is freed, after removing the allocator from all metadata that
	 * refers to it and then deleting it.
	 */
	bool delete_self();
	public:
	/**
	 * Constructor.  Takes the metadata array as an argument.
	 */
	HugeAllocator(PageMetadataArray &p, Buckets<Header> &b) : metadata_array(p), owner(b) {}
};

/**
 * Factory template class for creating small allocators.  This is designed
 * assuming that the compiler does good optimisation for large switch
 * statements.  This template instantiates itself recursively to construct a
 * `SmallAllocator` template instantiated with the correct size.
 *
 * Note: This must be a class template and not a function template, because C++
 * doesn't allow partial function template specialisations and we need to have
 * a base case of 0 for the second template parameter (the bucket), for any
 * given `Header` value.
 */
template<typename Header, size_t Bucket = largest_medium_bucket()>
struct small_allocator_factory
{
	/**
	 * Create an allocator in the specified `bucket`.  The value of `bucket`
	 * must be lower than the `Bucket` template value.
	 */
	__attribute__((always_inline))
	static Allocator<Header>* create(int bucket)
	{
		if (bucket == Bucket)
		{
			const size_t size = BucketSize<Bucket>::value;
			//ASSERT(bucket_for_size(size) == bucket);
			return SmallAllocator<size, Header>::create();
		}
		return small_allocator_factory<Header, Bucket-1>::create(bucket);
	}
};

/**
 * Base case to terminate recursive `small_allocator_factory` instantiations.
 */
template<typename Header>
struct small_allocator_factory<Header, 0>
{
	/**
	 * Base case for `create` function.  Either creates an allocator with
	 * bucket 0, or returns a null pointer.
	 */
	static Allocator<Header>* create(int bucket)
	{
		if (bucket == 0)
		{
			return SmallAllocator<BucketSize<0>::value, Header>::create();
		}
		ASSERT(0);
		return nullptr;
	}
};

template<typename Header, size_t Bucket = largest_large_bucket()>
struct large_allocator_factory
{
	/**
	 * Create an allocator in the specified `bucket`.  The value of `bucket`
	 * must be lower than the `Bucket` template value.
	 */
	__attribute__((always_inline))
	static Allocator<Header>* create(int bucket)
	{
		if (bucket == Bucket)
		{
			const size_t size = BucketSize<Bucket>::value;
			//ASSERT(bucket_for_size(size) == bucket);
			return LargeAllocator<size, Header>::create();
		}
		return large_allocator_factory<Header, Bucket-1>::create(bucket);
	}
};

template<typename Header>
struct large_allocator_factory<Header, 0>
{
	/**
	 * Base case for `create` function.  Either creates an allocator with
	 * bucket 0, or returns a null pointer.
	 */
	static Allocator<Header>* create(int bucket)
	{
		if (bucket == 0)
		{
			return LargeAllocator<BucketSize<0>::value, Header>::create();
		}
		ASSERT(0);
		return nullptr;
	}
};

/**
 * Manager for allocators.  Constructs new allocators on demand.
 */
template<typename Header>
struct Buckets : public PageAllocated<Buckets<Header>>
{
	/**
	 * Convenience name for the metadata array template.
	 */
	using PageMetadataArray = PageMetadata<chunk_size_bits, address_space_size_bits, page_size, Header>;
	/**
	 * Array of allocators for fixed-size buckets.  The allocators form a
	 * linked list within each bucket.
	 */
	std::array<std::atomic<Allocator<Header>*>, fixed_buckets> fixed_buckets;
	/**
	 * Pointer to the index that stores the map from address to allocator.
	 */
	PageMetadataArray &p;
	/**
	 * Allocator type used to allocate huge allocators.  This allocator doesn't
	 * need to store per-object headers, even if the huge allocators that it
	 * allocates do.
	 */
	using HugeAllocatorAllocator = SmallAllocator<sizeof(HugeAllocator<Header>), void>;
	/**
	 * Allocator used to allocate huge allocators.  Huge allocation metadata is
	 * stored out of line from the rest of the allocation, and is quite small.
	 */
	std::atomic<HugeAllocatorAllocator*> huge_allocator_allocator;
	/**
	 * Construct a huge allocator.
	 */
	Allocator<Header> *huge_allocator()
	{
		if (huge_allocator_allocator == nullptr)
		{
			auto *aa = HugeAllocatorAllocator::create();
			HugeAllocatorAllocator *expected = nullptr;
			// If we lost the race to create this allocator
			if (!huge_allocator_allocator.compare_exchange_strong(expected, aa))
			{
				delete aa;
				ASSERT(expected != nullptr);
			}
		}
		auto *aa = huge_allocator_allocator.load();
		// FIXME: If we allocate a *lot* of huge allocations, then we never
		// reuse space from allocators after the head of this list.  It would
		// be better to maintain two lists, protected by a lock: whenever we
		// create or destroy a huge allocator, we're calling m[un]map, so an
		// extra lock and unlock on this path is unlikely to be significant.
		void *buffer = static_cast<Allocator<void>*>(aa)->alloc(sizeof(HugeAllocator<Header>));
		// The buffer will be null if the allocator became full (possibly as a
		// result of allocations in another thread).
		if (buffer == nullptr)
		{
			auto *new_allocator_allocator = HugeAllocatorAllocator::create();
			new_allocator_allocator->next = huge_allocator_allocator;
			// If we lost a race to allocate a new allocator allocator, delete
			// our new one.  Either way, try again - now that this is visible,
			// it's possible (though highly unlikely) that we'll be preempted
			// by another thread that will use up all of the space in this
			// allocator.
			if (!huge_allocator_allocator.compare_exchange_strong(aa, new_allocator_allocator))
			{
				delete new_allocator_allocator;
			}
			return huge_allocator();
		}
		auto *a = new (buffer) HugeAllocator<Header>(p, *this);
		ASSERT(a);
		return a;
	}
	public:
	/**
	 * Constructor. 
	 */
	Buckets(PageMetadataArray &metadata) : p(metadata) {}
	/**
	 * Returns an allocator for a specific bucket.  If there is no existing
	 * bucket, then one is created.
	 */
	Allocator<Header> *allocator_for_bucket(size_t bucket)
	{
		if (unlikely(bucket == -1))
		{
			return huge_allocator();
		}
		// No lock held.  The returned object is not locked, so callers may
		// need to try multiple times to get an allocator that has empty space.
		Allocator<Header> *a = fixed_buckets[bucket].load(std::memory_order_relaxed);
		// FIXME: Handle creating huge allocators for things that want to just be mmap'd.
		if (a == nullptr)
		{
			if (bucket <= largest_medium_bucket())
			{
				a = small_allocator_factory<Header>::create(bucket);
			}
			else if (bucket <= largest_large_bucket())
			{
				a = large_allocator_factory<Header>::create(bucket);
			}
			else
			{
				ASSERT(0);
			}
			ASSERT(a);
			ASSERT(a->bucket() == bucket);
			ASSERT(!a->full());
			p.set_allocator_for_address(a, (vaddr_t)a);
			Allocator<Header> *old = nullptr;
			while (!fixed_buckets[bucket].compare_exchange_weak(old, a, std::memory_order_relaxed))
			{
				a->next = old;
			}
		}
		// If this allocator is full
		if (a->full())
		{
			// FIXME: This is racy.  An allocator can transition from full to
			// non-full in parallel with this.  
			Allocator<Header> *next = a->next.exchange(nullptr);
			fixed_buckets[bucket].compare_exchange_strong(a, next, std::memory_order_relaxed);
			return allocator_for_bucket(bucket);
		}
		return a;
	}
	/**
	 * Delete a huge allocator.
	 */
	bool delete_huge_allocator(HugeAllocator<Header> *a)
	{
		for (Allocator<void> *allocator = huge_allocator_allocator.load() ;
		     allocator ;
		     allocator = allocator->next.load())
		{
			cheri::capability<Allocator<void>> alloc_cap(allocator);
			if (alloc_cap.contains(a))
			{
				assert(a->allocation == nullptr);
				allocator->free(a);
				return true;
			}
		}
		return false;
	}
};

template<typename Header>
bool HugeAllocator<Header>::delete_self()
{
	return owner.delete_huge_allocator(this);
}


/**
 * External interface for this allocator.  This manages a set of fixed-size
 * allocators.
 */
template<typename Header>
class slab_allocator : public PageAllocated<slab_allocator<Header>>
{
	/**
	 * Convenience name for the metadata array.
	 */
	using PageMetadataArray = PageMetadata<chunk_size_bits, address_space_size_bits, page_size, Header>;
	/**
	 * Large array for mapping from addresses to allocators.
	 */
	PageMetadataArray *p = PageMetadataArray::create();
	/**
	 * Fixed-size allocator manager.
	 */
	Buckets<Header> global_buckets = { *p };
	class huge_allocator_iterator
	{
		using alloc = typename allocator_fast_iterator<Header>::alloc;
		/**
		 * flag indicating that we have reached the end of the huge iterators.
		 */
		bool end = false;
		/**
		 * The allocator used for huge allocators.
		 */
		Allocator<void> *a;
		/**
		 * Array storing the valid allocators that we've found.
		 */
		std::array<alloc, allocator_fast_iterator<void>::buffer_size> allocators;
		/**
		 * Iteration state in the current allocator.
		 */
		allocator_fast_iterator<void> iter;
		/**
		 * Current index in the array that we're iterating over.
		 */
		int idx = 0;
		/**
		 * For iterators before the end, the index from the first allocator
		 * that we found.
		 */
		int global_idx = 0;
		/**
		 * Number of allocators that we've found in the `allocators` array.
		 */
		int allocator_count = 0;
		/**
		 * Helper method to fill the `allocators` array.
		 */
		void fill_iterator()
		{
			if (a == nullptr)
			{
				end = true;
				return;
			}
			if (end)
			{
				return;
			}
			a->fill_fast_iterator(iter);
			if (iter.buffer_length == 0)
			{
				a = a->next;
				if (a == nullptr)
				{
					end = true;
				}
				fill_iterator();
				return;
			}
			idx = 0;
			allocator_count = 0;
			for (int i=0 ; i<iter.buffer_length ; i++)
			{
				auto *ha = reinterpret_cast<HugeAllocator<Header>*>(iter.buffer[i].first);
				if (ha->allocation != nullptr)
				{
					allocators[allocator_count++] = { ha->allocation, &ha->header };
				}
			}
			if (allocator_count == 0)
			{
				fill_iterator();
			}
		}
		public:
		/**
		 * Constructor, takes the first allocator in the chain that allocates
		 * over huge allocators as an argument.
		 */
		huge_allocator_iterator(Allocator<void> *allocator) : a(allocator)
		{
			fill_iterator();
		}
		/**
		 * Constructor for creating an iterator pointing to the end.
		 */
		huge_allocator_iterator() : a(nullptr), end(true) {}
		/**
		 * Increment operator.  Finds the next valid huge allocator.
		 */
		huge_allocator_iterator &operator++()
		{
			idx++;
			global_idx++;
			if (idx >= allocator_count)
			{
				fill_iterator();
			}
			return *this;
		}
		/**
		 * Dereference operator.  Returns the current huge allocator.
		 */
		alloc &operator*()
		{
			return allocators[idx];
		}
		/**
		 * Non-equality test, for terminating range-based for loop.
		 */
		bool operator!=(const huge_allocator_iterator &other)
		{
			// If both are at the end, then these are equal (not not equal)
			if (end && other.end)
			{
				return false;
			}
			// If only one is at the end, then these are not equal
			if (end || other.end)
			{
				return true;
			}
			return global_idx != other.global_idx;
		}
	};
	/**
	 * Forward iterator for iterating all allocations.
	 */
	class fixed_allocator_iterator
	{
		/**
		 * The current allocator that we're iterating over.  Allocators are
		 * arranged in a linked list for each size.
		 */
		Allocator<Header> *a = nullptr;
		/**
		 * The allocator used for huge allocators.
		 */
		Allocator<void> *huge_allocators = nullptr;
		/**
		 * The container that lets us find the heads of all of the linked
		 * lists.
		 */
		Buckets<Header> &buckets;
		/**
		 * The iteration state for the current allocator.
		 */
		allocator_fast_iterator<Header> iter;
		/**
		 * Import the fast iterator's definition of an allocation.
		 */
		using alloc = typename allocator_fast_iterator<Header>::alloc;
		/**
		 * Has this iterator reached the end?
		 */
		bool end = false;
		/**
		 * Returns the next allocator after the specified bucket, or nullptr if
		 * none exists.
		 */
		Allocator<Header> *allocator_from_bucket(int idx)
		{
			while (idx < fixed_buckets)
			{
				Allocator<Header> *allocator = buckets.fixed_buckets[idx++];
				if (allocator)
				{
					return allocator;
				}
			}
			return nullptr;
		}
		/**
		 * Helper, fills the fast iterator state.
		 */
		void fill_iterator()
		{
			if (unlikely(a == nullptr))
			{
				ASSERT(iter.end == 0);
				ASSERT(iter.buffer_length == 0);
				a = allocator_from_bucket(0);
				// No allocations yet?
				if (unlikely(a == nullptr))
				{
					end = true;
					return;
				}
			}
			// If we have some data in the buffer, but we haven't completely
			// filled it, then we've filled this allocator.
			if ((iter.end > 0) && (iter.buffer_length < iter.buffer_size))
			{
				iter.end = 0;
				iter.buffer_length = 0;
				if (a->next)
				{
					a = a->next;
				}
				else
				{
					int bucket = a->bucket();
					if (bucket < 1)
					{
						end = true;
						return;
					}
					a = allocator_from_bucket(bucket + 1);
				}
				if (unlikely(a == nullptr))
				{
					end = true;
					return;
				}
			}
			ASSERT(a);
			a->fill_fast_iterator(iter);
			if (iter.buffer_length == 0)
			{
				fill_iterator();
			}
		}
		public:
		/**
		 * Constructor.
		 */
		fixed_allocator_iterator(Buckets<Header> &b, bool e=false) : buckets(b), end(e) {}
		alloc &operator*()
		{
			if (unlikely(iter.buffer_length == 0))
			{
				fill_iterator();
			}
			return iter.buffer.at(iter.buffer_idx);
		}
		/**
		 * Increment the iterator.
		 */
		fixed_allocator_iterator &operator++()
		{
			iter.buffer_idx++;
			if (unlikely(iter.buffer_idx >= iter.buffer_length))
			{
				fill_iterator();
			}
			return *this;
		}
		/**
		 * Non-equality test, for terminating range-based for loop.
		 */
		bool operator!=(const fixed_allocator_iterator &other)
		{
			// If both are at the end, then these are equal (not not equal)
			if (end && other.end)
			{
				return false;
			}
			// If only one is at the end, then these are not equal
			if (end || other.end)
			{
				return true;
			}
			return (&buckets != &other.buckets) || (iter != other.iter);
		}
	};
	public:
	using object_header = Header;
	/**
	 * Allocate `size` bytes.
	 */
	void *alloc(size_t size)
	{
		ASSERT(p);
		if (unlikely(size == 0))
		{
			return nullptr;
		}
		int bucket = bucket_for_size(size);
		while (true)
		{
			auto *a = global_buckets.allocator_for_bucket(bucket);
			void *allocation = a->alloc(size);
			if (allocation)
			{
				return allocation;
			}
		}
	}
	/**
	 * Free the specified pointer.
	 */
	void free(void *ptr)
	{
		ASSERT(p);
		Allocator<Header> *a = p->allocator_for_address((vaddr_t)ptr);
		if (!a)
		{
			fprintf(stderr, "Failed to find allocator for %#p\n", ptr);
		}
		ASSERT(a);
		// FIXME: This needs to zero memory.
		a->free(ptr);
	}
	/**
	 * Returns the underlying allocation and the header for a given pointer.
	 */
	void *object_for_allocation(void *ptr, Header *&header)
	{
		ASSERT(this);
		ASSERT(p);
		vaddr_t addr = (vaddr_t)ptr;
		Allocator<Header> *a = p->allocator_for_address(addr);
		if (!a)
		{
			header = nullptr;
			return nullptr;
		}
		return a->allocation_for_address(addr, header);
	}
	using iterator = SplicedForwardIterator<fixed_allocator_iterator, huge_allocator_iterator>;
	/**
	 * Returns a start iterator for all allocations.
	 */
	iterator begin()
	{
		return iterator(std::move(fixed_allocator_iterator(global_buckets)),
		                std::move(fixed_allocator_iterator(global_buckets, true)),
		                std::move(huge_allocator_iterator(global_buckets.huge_allocator_allocator)));
	}
	/**
	 * Returns an end iterator for all allocations.
	 */
	iterator end()
	{
		return iterator(std::move(fixed_allocator_iterator(global_buckets, true)),
		                std::move(fixed_allocator_iterator(global_buckets, true)),
		                std::move(huge_allocator_iterator()));
	}
};


}
