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
class PageMetadata : public PageAllocated<PageMetadata<ChunkBits, ASBits, PageSize, Header>>
{
	std::array<Allocator<Header>*, 1ULL<<(ASBits - ChunkBits)> array;
	using self_type = PageMetadata<ChunkBits, ASBits, PageSize, Header>;
	size_t index_for_vaddr(vaddr_t a)
	{
		// Trim off any high bits that might accidentally be set.
		a <<= ASBits;
		a >>= ASBits;
		a >>= log2<chunk_size>();
		return a;
	}
	public:
	static self_type *create()
	{
		static_assert(!std::is_polymorphic<self_type>::value,
			"Page metadata class must not have a vtable!\n");
		return (PageAllocated<self_type>::alloc(sizeof(self_type)));
	}
	Allocator<Header> *allocator_for_address(vaddr_t addr)
	{
		return array[index_for_vaddr(addr)];
	}
	void set_allocator_for_address(Allocator<Header> *allocator, vaddr_t addr)
	{
		array[index_for_vaddr(addr)] = allocator;
	}
};

template<typename Header>
struct Allocator
{
	std::atomic<Allocator<Header>*> next;
	/**
	 * Allocate an object of the specified size.  For small allocations, this is 
	 */
	virtual void *alloc(size_t) { return nullptr; }
	/**
	 * Returns the size of allocations from this pool.
	 */
	virtual size_t object_size(void *) { return 0; }
	/**
	 * Free an object in this allocator.  Returns true if the allocator has
	 * just transitioned from a full state to a non-full state, at which point
	 * it should be re-added to the list.
	 */
	virtual bool free(void *) { return false; }
	/**
	 * Return whether the allocator is full (i.e. unable to allocate anything
	 * else).
	 */
	virtual bool full() { return true; }
	virtual int bucket() const { return -1; }
	virtual void *allocation_for_address(vaddr_t, Header *&) { return nullptr; }
};

static constexpr size_t gcd(size_t a, size_t b)
{
	return b == 0 ? a : gcd(b, a % b);
}


template<size_t AllocSize, size_t ChunkSize, typename Header>
class SmallAllocationHeader
{
	public:
	/**
	 * To avoid having to track allocations that span a page boundary, we use a
	 * folio that is the least common multiple of the page size and alloc size.
	 */
	static const size_t folio_size = page_size * AllocSize / gcd(page_size, AllocSize);
	static const size_t allocs_per_folio = folio_size / AllocSize;
	static const int folios_per_chunk = ChunkSize / folio_size;
	UncontendedSpinlock<long> lock;
	//static_assert(allocs_per_folio > 1, "Folios the same size as allocs don't make sense");
	//static_assert(ChunkSize / folio_size > 1, "Chunks that only hold one folio don't make sense!");
	struct list_entry
	{
		static const uint16_t not_present = 0xffff;
		uint16_t prev;
		uint16_t next;
		uint16_t free_count;
		BitSet<allocs_per_folio> free;
	};
	/**
	 * Linked list entries.
	 */
	std::array<list_entry, folios_per_chunk> list_entries;
	template<typename HeaderTy, class Enable = void>
	struct HeaderList
	{
	};
	template<typename HeaderTy>
	struct HeaderList<HeaderTy, typename std::enable_if<std::is_void<HeaderTy>::value>::type>
	{
		Header *header_at_index(size_t idx)
		{
			return nullptr;
		}
	};
	template<typename HeaderTy>
	struct HeaderList<HeaderTy, typename std::enable_if<!std::is_void<HeaderTy>::value>::type>
	{
		std::array<Header, folios_per_chunk*allocs_per_folio> array;
		Header *header_at_index(size_t idx)
		{
			return array.at(idx);
		}
	};
	HeaderList<Header> headers;
	Header *header_at_index(size_t idx)
	{
		return headers.header_at_index(idx);
	}
	public:
	/**
	 * A conservative approximation of the bucket that has the most free space.
	 * The bucket with the most free space will always be after this, but it
	 * may not be exactly here.
	 */
	uint16_t free_head = 1;
	uint16_t free_allocs_total;
	// Check that the number of list entries is small enough that we can store
	// all of the allocations.
	static_assert(folios_per_chunk < 0xfffe, "Not index value too small");
	/**
	 * An array of indexes into the `list_entries` array.  Each entry in this represents one 
	 */
	std::array<uint16_t, allocs_per_folio+1> free_lists;
	SmallAllocationHeader(size_t size)
	{
		for (auto i=0 ; i<allocs_per_folio ; i++)
		{
			free_lists[i] = list_entry::not_present;
		}
		const int folios_for_header = ((size + (folio_size-1)) / folio_size) + 5;
		// FIXME: We probably shouldn't reserve the entire folio.
		for (uint16_t i=0 ; i<folios_for_header ; i++)
		{
			list_entry &l = list_entries[i];
			l.free_count = 0;
			l.prev = i-1;
			l.next = i+1;
		}
		for (uint16_t i=folios_for_header ; i<folios_per_chunk ; i++)
		{
			list_entry &l = list_entries[i];
			l.prev = i-1;
			l.next = i+1;
			l.free_count = allocs_per_folio;
		}
		free_allocs_total = (folios_per_chunk-folios_for_header) * allocs_per_folio;
		// The last freelist (for pages that are completely empty)
		free_lists[allocs_per_folio] = folios_for_header;
		free_head = allocs_per_folio;
		list_entries[folios_for_header].prev = list_entry::not_present;
		list_entries[folios_per_chunk-1].next = list_entry::not_present;
		// The first freelist (for pages that are completely full)
		free_lists[0] = 0;
		list_entries[folios_for_header-1].next = list_entry::not_present;
		list_entries[0].prev = list_entry::not_present;
		//fprintf(stderr, "Header for %d byte allocations is %d bytes\n", (int)AllocSize, (int)size);
		//fprintf(stderr, "%d folios of %d bytes (%d allocs per folio)\n", (int)folios_per_chunk, (int)folio_size, (int)allocs_per_folio);
		//fprintf(stderr, "Each list entry is %d bytes\n", (int)sizeof(list_entry));
		//fprintf(stderr, "Overhead: %.2lf%%\n", (double)sizeof(*this)/ChunkSize*100);
	}
	bool free_allocation(size_t offset)
	{
		// FIXME: We should abort if offset % AllocSize is non-zero
		int idx = offset / AllocSize;
		int folio_idx = offset / folio_size;
		int alloc_in_folio = idx % allocs_per_folio;
		list_entry &l = list_entries[folio_idx];
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
	size_t reserve_allocation()
	{
		uint16_t folio = list_entry::not_present;
		size_t offset = 0;
		do {} while (!try_run_locked(lock, [&]()
			{
				// FIXME: Figure out why the free head isn't being put in the
				// correct place.
				free_head = 1;
				while (free_lists[free_head] == list_entry::not_present)
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
				assert(free_lists[free_head] != list_entry::not_present);
				folio = free_lists[free_head];
				list_entry &l = list_entries[folio];
				assert(l.free_count != 0);
				remove_list_entry(folio);
				l.free_count--;
				insert_list_entry(folio);
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
		if (folio == list_entry::not_present)
		{
			return -1;
		}
		assert(folio * folio_size + (offset * AllocSize) > sizeof(*this));
		return folio * folio_size + (offset * AllocSize);
	}
	__attribute__((always_inline))
	void remove_list_entry(uint16_t folio_idx)
	{
		list_entry &l = list_entries[folio_idx];
		if (l.prev == list_entry::not_present)
		{
			free_lists[l.free_count] = l.next;
		}
		else
		{
			list_entries[l.prev].next = l.next;
		}
		if (l.next != list_entry::not_present)
		{
			list_entries[l.next].prev = l.prev;
		}
	}
	__attribute__((always_inline))
	void insert_list_entry(uint16_t folio_idx)
	{
		list_entry &l = list_entries[folio_idx];
		l.prev = list_entry::not_present;
		l.next = free_lists[l.free_count];
		if (l.next != list_entry::not_present)
		{
			list_entries[free_lists[l.free_count]].prev = folio_idx;
		}
		free_lists[l.free_count] = folio_idx;
	}
};

template<size_t AllocSize, typename Header>
class SmallAllocator : public Allocator<Header>,
                       public SmallAllocationHeader<AllocSize, chunk_size, Header>,
                       public PageAllocated<SmallAllocator<AllocSize, Header>>
{
	using ChunkHeader = SmallAllocationHeader<AllocSize, chunk_size, Header>;
	using self_type = SmallAllocator<AllocSize, Header>;
	int bucket() const override
	{
		return bucket_for_size(AllocSize);
	}
	bool full() override
	{
		return ChunkHeader::free_allocs_total == 0;
	}
	void *alloc(size_t sz) override
	{
		assert(sz <= AllocSize);
		size_t offset = ChunkHeader::reserve_allocation();
		if (offset == -1)
		{
			return nullptr;
		}
		return reinterpret_cast<char*>(this) + offset;
	};
	size_t object_size(void *) override
	{
		return AllocSize;
	};
	bool free(void *ptr) override
	{
		size_t offset = reinterpret_cast<char*>(ptr) - reinterpret_cast<char*>(this);
		assert(offset < chunk_size);
		ChunkHeader::free_allocation(offset);
		return false;
	};
	SmallAllocator() : ChunkHeader(sizeof(*this))
	{
		assert(!full());
	}
	public:
	static SmallAllocator<AllocSize, Header> *create()
	{
		static_assert(chunk_size > sizeof(SmallAllocator<AllocSize, Header>),
		              "Metadata is bigger than chunk!");
		char *p = PageAllocated<self_type>::alloc(chunk_size, log2<chunk_size>());
		auto *a = new (p) SmallAllocator<AllocSize, Header>();
		set_allocator_for_address(a, reinterpret_cast<void*>(a));
		return a;
	}
};

template<typename Header, size_t Bucket = fixed_buckets>
struct small_allocator_factory
{
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

template<typename Header>
struct small_allocator_factory<Header, 0>
{
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

template<typename Header>
struct Buckets : public PageAllocated<Buckets<Header>>
{
	using PageMetadataArray = PageMetadata<chunk_size_bits, address_space_size_bits, page_size, Header>;
	std::array<std::atomic<Allocator<Header>*>, fixed_buckets> fixed_buckets;
	PageMetadataArray *p;
	Buckets(PageMetadataArray *metadata) : p(metadata) {}
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


template<typename Header>
class slab_allocator
{
	using PageMetadataArray = PageMetadata<chunk_size_bits, address_space_size_bits, page_size, Header>;
	PageMetadataArray *p = PageMetadataArray::create();
	Buckets<Header> global_buckets = { p };
	public:
	void *alloc(size_t size)
	{
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
	void free(void *ptr)
	{
		Allocator<Header> *a = p->allocator_for_address((vaddr_t)ptr);
		assert(a);
		a->free(ptr);
	}

};

}
