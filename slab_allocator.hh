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
#include <cassert>

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
		a <<= ASBits;
		a >>= ASBits;
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
		assert(pa);
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
		assert(allocator_for_address(addr) == nullptr);
		array.at(index_for_vaddr(addr)) = allocator;
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
};


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
	 * Template for storing either an array of headers, or nothing if the
	 * header type is void.  This allows the small allocator template to be
	 * instantiated with or without per-object headers, without suffering any
	 * space overhead.  In an optimised build, there should also be no run-time
	 * overhead (though there will be more work for the optimisers to delete
	 * dead code).
	 */
	template<typename HeaderTy, class Enable = void>
	struct HeaderList {};
	/**
	 * Template specialisation with a void header type.  This provides a
	 * stub implementation that has a size of zero and a trivial method.
	 */
	template<typename HeaderTy>
	struct HeaderList<HeaderTy, typename std::enable_if<std::is_void<HeaderTy>::value>::type>
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
	template<typename HeaderTy>
	struct HeaderList<HeaderTy, typename std::enable_if<!std::is_void<HeaderTy>::value>::type>
	{
		/**
		 * The array storing the headers.
		 */
		std::array<Header, folios_per_chunk*allocs_per_folio> array;
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
	static_assert(sizeof(HeaderList<void>) == 0, "Compiler has odd ABI!\n");
	/**
	 * List of headers.
	 */
	HeaderList<Header> headers;
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
				assert(l.free[alloc_in_folio]);
				l.free.clear(alloc_in_folio);
				assert(!l.free[alloc_in_folio]);
				// TODO: By placing this back at the head of the list, we
				// ensure that it will be reallocated quickly.  To reduce the
				// danger of use-after-free, we probably want the opposite
				// policy (note that this will also have to be done with
				// caching)
				insert_list_entry(folio_idx);
				free_allocs_total--;
				if (l.free_count == alloc_in_folio)
				{
					madvise(reinterpret_cast<char*>(this)+offset, alloc_in_folio, MADV_FREE);
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
				assert(free_head != 0);
				if (free_head == 0)
				{
					return;
				}
				assert(free_lists[free_head] != folio::not_present);
				folio_index = free_lists[free_head];
				folio &l = folios[folio_index];
				assert(l.free_count != 0);
				remove_list_entry(folio_index);
				l.free_count--;
				insert_list_entry(folio_index);
				offset = l.free.first_zero();
				if (offset >= allocs_per_folio)
				{
					fprintf(stderr, "Free list head is %d (%d), found full folio on free list\n", (int)free_head, (int)allocs_per_folio);
				}
				assert(offset < allocs_per_folio);
				free_head--;
				assert(!l.free[offset]);
				l.free.set(offset);
				assert(l.free[offset]);
				free_allocs_total--;
			}));
		if (folio_index == folio::not_present)
		{
			return -1;
		}
		assert(folio_index * folio_size + (offset * AllocSize) > sizeof(*this));
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
};

/**
 * Small allocator.  This implements the methods defined in the abstract
 * allocator, but delegates most of the implementation to the
 * SmallAllocationHeader.
 */
template<size_t AllocSize, typename Header>
class SmallAllocator final : public Allocator<Header>,
                             public SmallAllocationHeader<AllocSize, chunk_size, Header>,
                             public PageAllocated<SmallAllocator<AllocSize, Header>>
{
	using ChunkHeader = SmallAllocationHeader<AllocSize, chunk_size, Header>;
	using self_type = SmallAllocator<AllocSize, Header>;
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
		fprintf(stderr, "Allocating from %#p\n", this);
		assert(sz <= AllocSize);
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
	 * Free the object.
	 */
	bool free(void *ptr) override
	{
		size_t offset = reinterpret_cast<char*>(ptr) - reinterpret_cast<char*>(this);
		assert(offset < chunk_size);
		ChunkHeader::free_allocation(offset);
		return false;
	};
	/**
	 * Constructor.  Reserves space for all of the metadata.
	 */
	SmallAllocator() : ChunkHeader(sizeof(*this))
	{
		assert(!full());
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
template<typename Header, size_t Bucket = fixed_buckets>
struct small_allocator_factory
{
	/**
	 * Create an allocator in the specified `bucket`.  The value of `bucket`
	 * must be lower than the `Bucket` template value.
	 */
	static Allocator<Header>* create(int bucket)
	{
		if (bucket == Bucket)
		{
			const size_t size = BucketSize<Bucket>::value;
			//assert(bucket_for_size(size) == bucket);
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
		assert(0);
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
	PageMetadataArray *p;
	/**
	 * Constructor. 
	 */
	Buckets(PageMetadataArray *metadata) : p(metadata) {}
	/**
	 * Returns an allocator for a specific bucket.  If there is no existing
	 * bucket, then one is created.
	 */
	Allocator<Header> *allocator_for_bucket(size_t bucket)
	{
		// No lock held.  The returned object is not locked, so callers may
		// need to try multiple times to get an allocator that has empty space.
		Allocator<Header> *a = fixed_buckets[bucket].load(std::memory_order_relaxed);
		// FIXME: Handle creating huge allocators for things that want to just be mmap'd.
		if (a == nullptr)
		{
			a = small_allocator_factory<Header>::create(bucket);
			assert(a->bucket() == bucket);
			assert(!a->full());
			p->set_allocator_for_address(a, (vaddr_t)a);
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
};


/**
 * External interface for this allocator.  This manages a set of fixed-size
 * allocators.
 */
template<typename Header>
class slab_allocator
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
	Buckets<Header> global_buckets = { p };
	public:
	/**
	 * Allocate `size` bytes.
	 */
	void *alloc(size_t size)
	{
		assert(p);
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
		assert(p);
		Allocator<Header> *a = p->allocator_for_address((vaddr_t)ptr);
		assert(a);
		a->free(ptr);
	}
	/**
	 * Returns the underlying allocation and the header for a given pointer.
	 */
	void *allocation_for_address(void *ptr, Header *&header)
	{
		assert(this);
		assert(p);
		vaddr_t addr = (vaddr_t)ptr;
		Allocator<Header> *a = p->allocator_for_address(addr);
		assert(a);
		return a->allocation_for_address(addr, header);
	}
};


}
