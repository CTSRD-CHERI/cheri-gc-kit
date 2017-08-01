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
#include "page.hh"
#include "BitSet.hh"
#include "cheri.hh"
#include "lock.hh"
#include "bump_the_pointer_heap.hh"
#include <cstddef>
#include <type_traits>

namespace {

template<size_t HeapSize, class Header=void>
class bump_the_pointer_or_large_heap 
{
	/**
	 * Bump-the-pointer heap used for objects smaller than a page.
	 */
	bump_the_pointer_heap<HeapSize, Header> small_heap;
	/**
	 * Import the `cheri::capability` template.
	 */
	template<typename T>
	using capability = cheri::capability<T>;
	/**
	 * Convenience name for the type of the current instantiation of this
	 * template.
	 */
	using ThisType = class bump_the_pointer_or_large_heap<HeapSize, Header>;
	/**
	 * Large allocations are created in a separate page.  Their header is
	 * stored in the metadata section.
	 */
	using LargeAlloc = std::pair<Header, capability<void>>;
	/**
	 * Type for the container for large allocators.
	 */
	using LargeAllocsVector = std::vector<LargeAlloc, PageAllocator<LargeAlloc>>;
	/**
	 * List of all large allocations.
	 * 
	 * FIXME: We currently have to do a linear scan of these to find the object
	 * corresponding to a pointer.  We could improve this in several ways, and
	 * should if we see more than a small number of large allocs.
	 */
	LargeAllocsVector large_allocs;
	/**
	 * Spinlock protecting large allocations.  We expect that large allocations
	 * will be sufficiently infrequent that it will be rare for them to happen
	 * concurrently.
	 */
	UncontendedSpinlock<long> large_alloc_lock;
	/**
	 * Adaptor that transforms an iterator into our large objects vector to
	 * have the same element type as the small object iterator.
	 */
	struct wrap_iterator : LargeAllocsVector::iterator
	{
		/**
		 * Type of the superclass.
		 */
		using parent = typename LargeAllocsVector::iterator;
		/**
		 * Construct this iterator from the iterator that it wraps.
		 */
		wrap_iterator(parent &&i) : LargeAllocsVector::iterator(std::move(i)) {}
		/**
		 * Dereference operator.  The underlying storage stores the headers
		 * inline and returns a reference to them.  We must transform this into
		 * a pointer.
		 */
		std::pair<Header*,void*> operator*()
		{
			auto v = parent::operator*();
			return std::make_pair(&v.first, v.second.get());
		}
	};
	/**
	 * The type of the iterator that we return.
	 */
	using iterator = SplicedForwardIterator<typename decltype(small_heap)::iterator, wrap_iterator>;
	public:
	/**
	 * Accessor for the header type.
	 */
	typedef Header object_header;
	/**
	 * Iterator to all objects and their headers in this heap.
	 */
	iterator begin()
	{
		return make_spliced_forward_iterator(small_heap.begin(), small_heap.end(), wrap_iterator(large_allocs.begin()));
	}
	/**
	 * End iterator.
	 */
	iterator end()
	{
		return make_spliced_forward_iterator(small_heap.end(), small_heap.end(), wrap_iterator(large_allocs.end()));
	}
	// FIXME: These three won't actually work if large objects are allocated
	/**
	 * Update a reference with the given displacement.
	 */
	void *move_reference(void *ptr, ptrdiff_t disp)
	{
		return small_heap.move_reference(ptr, disp);
	}
	/**
	 * Move the specified object by a given displacement.
	 *
	 * This assumes that the object is in the small object region.
	 */
	void *move_object(void *start, ptrdiff_t disp)
	{
		return small_heap.move_object(start, disp);
	}
	/**
	 * Sets the object at the end of the relocatable section.
	 */
	void set_last_object(capability<void> obj)
	{
		return small_heap.set_last_object(obj);
	}
	/**
	 * Returns a pointer to the complete object for a given allocation.
	 */
	void *object_for_allocation(void *ptr, Header *&h)
	{
		void *obj = small_heap.object_for_allocation(ptr, h);
		if (obj)
		{
			return obj;
		}
		for (auto &o : large_allocs)
		{
			capability<void> cap(o.second);
			if (cap.contains(ptr))
			{
				h = &o.first;
				return o.second;
			}
		}
		return nullptr;
	}
	/**
	 * Sets the callback used to invoke the GC.
	 */
	template<typename T>
	void set_gc(T &g)
	{
		small_heap.set_gc(g);
	}
	/**
	 * Notify the allocator that the GC is going to start running.
	 */
	void start_gc()
	{
		small_heap.start_gc();
		large_alloc_lock.lock();
	}
	/**
	 * Notify the allocator that GC has finished.
	 */
	void end_gc()
	{
		large_alloc_lock.unlock();
		small_heap.end_gc();
	}
	/**
	 * Create an instance of this object.
	 */
	static ThisType *create()
	{
		auto heap = PageAllocator<ThisType>().allocate(1);
		heap->small_heap.allocate_heap();
		return heap;
	}
	/**
	 * Returns true if the range given as an argument might contain pointers.
	 */
	bool may_contain_pointers(void *)
	{
		// FIXME: We should mprotect our heap with no-store-capability and give
		// a coarse-grained reply to this (though then we'd have to store the
		// object headers somewhere else)
		return true;
	}
	/**
	 * Allocate an object of the specified size.
	 */
	void *alloc(size_t size)
	{
		ASSERT(this);
		if (size < page_size)
		{
			return small_heap.alloc(size);
		}
		// FIXME: We never trigger GC from large object allocations - we
		// probably should count these towards the total heap size.
		PageAllocator<char> alloc;
		void *a = alloc.allocate(size);
		run_locked(large_alloc_lock, [&] { large_allocs.emplace_back(Header(), a); });
		return a;
	}
	/**
	 * Start the garbage collector running.
	 */
	void collect()
	{
		small_heap.collect();
	}
};

} // Anonymous namespace.
