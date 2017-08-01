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
#include "nonstd_function.hh"
#include "page.hh"
#include "BitSet.hh"
#include "cheri.hh"
#include <cstddef>
#include <type_traits>


/**
 * A simple fixed-size bump-the-pointer heap.  Allows an optional object header.
 */
template<size_t HeapSize, class Header=void>
class bump_the_pointer_heap
{
	/**
	 * The size of the header.  We calculate this once so that we don't have to
	 * special-case `void` as a header type everywhere.
	 */
	static const size_t header_size = std::is_void<Header>::value ? 0 : sizeof(Header);
	/**
	 * Import the `cheri::capability` template.
	 */
	template<typename T> using capability = cheri::capability<T>;
	/**
	 * Convenience type for the current template instantiation.
	 */
	using ThisType = class bump_the_pointer_heap<HeapSize, Header>;
	/**
	 * Allocate objects as multiples of the maximum alignment requirement.
	 *
	 * Note: This works around a bug in CheriBSD where `max_align_t` is
	 * insufficiently aligned.
	 */
	static const auto alloc_granularity = std::max(alignof(std::max_align_t),alignof(void*));
	static_assert(sizeof(Header) % alloc_granularity == 0,
	              "Header size must be a multiple of allocation granularity");
	/**
	 * This heap type supports arbitrary-sized allocations and so must keep
	 * track of the starts.  It must also be able to map from a pointer to the
	 * allocated object.  We do this by adopting Munraj Vadera's idea of simply
	 * keeping a FAT-style bitmap, with one bit per block (typically
	 * `sizeof(void*)` bytes).  We can cheaply scan back from the start of a
	 * pointer to find the enclosing object.  For objects smaller than 1KiB (on
	 * CHERI128, 2KB on CHERI256), this will typically result in a single
	 * memory access and a short computation sequence.  For very large
	 * allocations, this may be slow if the user has subset the pointer so that
	 * the base of the capability is significantly (several KB) above the start
	 * of the object, but this allocator is a poor fit for very large
	 * allocations.
	 *
	 * Finding the next object also requires a linear scan but this, again,
	 * typically requires a single 64-bit memory access for objects smaller
	 * than 1KiB.
	 */
	BitSet<HeapSize / alloc_granularity, true> start_bits;
	static_assert(alloc_granularity >= alignof(void*), "max_align_t is insufficiently aligned!");
	/**
	 * Pointer to the heap.
	 */
	capability<char> heap;
	/**
	 * The offset in the heap of the first unallocated space.  The `allocate`
	 * method simply atomically increments this pointer to get a new
	 * allocation.
	 */
	std::atomic<size_t> start;
	/**
	 * Counter for detecting current access.  If the value is even, then the GC
	 * is not running.  If the value is odd, the GC is running.  If the value
	 * changes in between the start and the end of a transaction, the
	 * transaction must retry.
	 */
	std::atomic<long long> version;
	/**
	 * Callback for invoking the GC.  This is called when allocation fails.
	 */
	Function *gc = nullptr;
	/**
	 * The space used to store the object that `gc` points to.
	 */
	char callback_buffer[128];
	public:
	/**
	 * Iterator for objects created b this allocator.
	 */
	class iterator
	{
		/**
		 * The allocator is allowed to modify the state of this iterator.
		 */
		friend class bump_the_pointer_heap<HeapSize, Header>;
		/**
		 * The start of the current object, in `alloc_granularity` units from
		 * the start of the heap..
		 */
		size_t start;
		/**
		 * The start of the next object, in `alloc_granularity` units from the
		 * start of the heap..
		 */
		size_t next;
		/**
		 * The start of the object after the end of all allocated objects, in
		 * `alloc_granularity` units from the start of the heap..
		 */
		size_t end;
		/**
		 * The allocator that this iterator is iterating over.
		 */
		ThisType &heap;
		/**
		 * Initialise a new iterator pointing at the start.
		 */
		iterator(ThisType &heap) : start(0), heap(heap)
		{
			next = heap.start_bits.one_after(0);
			end = heap.start / alloc_granularity;
		}
		public:
		/**
		 * Return a pair of a pointer to the current object header and a
		 * pointer to the current object.
		 */
		std::pair<Header*, void*> operator*()
		{
			ASSERT(start < next);
			size_t start_byte = start * alloc_granularity;
			size_t next_byte = next * alloc_granularity;
			capability<Header> header(reinterpret_cast<Header*>(heap.heap.get()));
			ASSERT(start_byte < heap.heap.length());
			// FIXME: handle void header types
			header.set_offset(start_byte);
			header.set_bounds(1);
			capability<void> obj(heap.heap);
			obj.set_offset(start_byte + header_size);
			obj.set_bounds(next_byte - (start_byte + header_size));
			return { header.get(), obj.get() };
		}
		/**
		 * Increment the current iterator.
		 */
		iterator &operator++()
		{
			start = next;
			next = heap.start_bits.one_after(next);
			// If this there are no more ones, find the end.
			// FIXME: Scanning the bitfield can be expensive if we're doing an
			// early GC, we should probably record the bounds of the last
			// allocation so that we know when to stop scanning...
			if (next == HeapSize / alloc_granularity)
			{
				next = end;
			}
			return *this;
		}
		/**
		 * Inequality test.
		 */
		bool operator!=(const iterator &o) const
		{
			return start != o.start;
		}
	};
	/**
	 * The iterator is tightly coupled to this class and both are allowed to
	 * inspect each others' internal state.
	 */
	friend class iterator;
	/**
	 * Start iterator for all objects allocated by this object.
	 */
	iterator begin()
	{
		return iterator(*this);
	}
	/**
	 * End iterator for all objects allocated by this object.
	 */
	iterator end()
	{
		iterator i(*this);
		i.start = start / alloc_granularity;
		return i;
	}
	/**
	 * Update a pointer to an object in this heap to point to a new location.
	 */
	void *move_reference(void *ptr, ptrdiff_t disp)
	{
		capability<void> cap(ptr);
		ASSERT(heap.base() <= cap.base());
		ASSERT((heap.base() + heap.length()) >= (cap.base() + cap.length()));
		return move_capability(static_cast<void*>(heap.get()), ptr, disp);
	}
	/**
	 * Move an object in this heap.
	 */
	void *move_object(void *obj_start, ptrdiff_t disp)
	{
		Header *h;
		capability<void> obj = object_for_allocation(obj_start, h);
		capability<void> cap(obj_start);
		ASSERT(disp <= 0);
		ASSERT(cap.base() + disp > heap.base());
		size_t offset = cap.base() - heap.base();
		if (!std::is_void<Header>::value)
		{
			offset = cheri::base(h) - heap.base();
			void *desthead = move_reference(h, disp);
			memmove(desthead, h, header_size);
		}
		start_bits.clear(offset / alloc_granularity);
		start_bits.set((offset + disp) / alloc_granularity);
		void *dest = move_reference(obj_start, disp);
		return memmove(dest, obj, obj.length());
	}
	/**
	 * Notify the allocator that all objects after this are no longer needed.
	 */
	void set_last_object(capability<void> obj)
	{
		// FIXME: This should zero the memory.
		start = obj.base() + obj.length();
	}
	/**
	 * Returns the object that contains the start of `ptr`, or `nullptr` if the
	 * object is not in this range.
	 */
	void *object_for_allocation(void *ptr, Header *&h)
	{
		capability<void> cap(ptr);
		ptrdiff_t offset = cap.base() - heap.base();
		if ((offset < 0) || (offset >= heap.length()))
		{
			return nullptr;
		}
		offset /= alloc_granularity;
		while ((offset > 0) && !start_bits[offset])
		{
			offset--;
		}
		offset *= alloc_granularity;
		capability<void> header(heap);
		header.set_offset(offset);
		header.set_bounds(header_size);
		offset += header_size;
		// Assume in the common case that the length hasn't been shrunk
		size_t end = (cap.base() + cap.length()) / alloc_granularity;
		end = start_bits.one_after(end-1);
		end *= alloc_granularity;
		end = std::min(end, start.load());
		capability<void> obj(heap);
		obj.set_offset(offset);
		obj.set_bounds(end - offset);
		h = static_cast<Header*>(header.get());
		return obj;
	}
	/**
	 * Allocate the space used for this allocator.
	 */
	void allocate_heap()
	{
		assert(heap == nullptr);
		capability<char> h(PageAllocator<char>().allocate(HeapSize));
		start = 0;
		assert(h != nullptr);
		heap = h;
		fprintf(stderr, "Allocated small object heap: %#p\n", h.get());
	}
	/**
	 * Set the callback for invoking the garbage collector.
	 */
	template<typename T>
	void set_gc(T &fn)
	{
		static_assert(sizeof(T) <= sizeof(callback_buffer),
		              "Callback buffer too small for callback");
		gc = (new (callback_buffer) ConcreteFunction<T>(fn));
	}
	/**
	 * Expose the object header type.
	 */
	typedef Header object_header;
	/**
	 * Notify the allocator that the GC has started to run.
	 */
	void start_gc()
	{
		version++;
		assert(version % 2 == 1);
	}
	/**
	 * Notify the allocator that the GC has finished running.
	 */
	void end_gc()
	{
		version++;
		assert(version % 2 == 0);
	}
	/**
	 * Create an instance of this object.
	 */
	static ThisType *create()
	{
		return PageAllocator<ThisType>().allocate(1);
	}
	/**
	 * Return whether an object in a given range may contain pointers.
	 */
	bool may_contain_pointers(void *)
	{
		// FIXME: We should mprotect our heap with no-store-capability and give
		// a coarse-grained reply to this (though then we'd have to store the
		// object headers somewhere else)
		return true;
	}
	/**
	 * Allocate an object of the given size.
	 */
	void *alloc(size_t size)
	{
		ASSERT(this);
		size += header_size;
		long long v;
		capability<void> a;
		do
		{
			// If the GC has started then we're about to get a signal.  Spin until we do.
			while ((v = version) % 2 == 1) {}
			size = roundUp<alloc_granularity>(size);
			// FIXME: Round the size so that CHERI 128 bounds will be exact
			size_t offset = start.fetch_add(size);
			if (offset > heap.length())
			{
				(*gc)();
				continue;
			}
			start_bits.set((offset) / alloc_granularity);
			offset += header_size;
			a = static_cast<char*>(heap.get());
			a.set_offset(offset);
			a.set_bounds(size - header_size);
		} while (v != version);
		return a;
	}
	/**
	 * Invoke the garbage collector.
	 */
	void collect()
	{
		(*gc)();
	}
	/**
	 * Destroy the object.  Should never be called.
	 */
	~bump_the_pointer_heap()
	{
		delete (callback_buffer) (gc);
	}
};
