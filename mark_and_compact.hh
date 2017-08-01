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

#pragma once
#include <type_traits>
#include <vector>
#include <setjmp.h>
#include "cheri.hh"
#include "page.hh"
#include "counter.hh"

namespace 
{

/**
 * Object header for this collector.  Declared outside the class so that its
 * type doesn't depend on the template arguments.
 */
struct alignas(void*) mark_and_compact_object_header 
{
	/**
	 * The displacement for the object.  After the object has been relocated,
	 * the new version will be `displacement` bytes before the old.
	 */
	uint64_t displacement;
	/**
	 * The GC state for this object.
	 */
	enum
	{
		/// Object has not been seen by the GC yet
		unmarked = 0,
		/// Object has been marked as live, but it has not yet been scanned.
		marked,
		/// Object has been visited.
		visited
	} color;
	/**
	 * Does the object contain any pointers?
	 */
	bool contains_pointers;
	/**
	 * Helper for debugging: dump the header in a human-readable format.
	 */
	void dump()
	{
		fprintf(stderr, "Displacement: %lx, color: %s, contains pointers: %s\n",
				displacement,
				(color == unmarked) ? "unmarked" :
					((color == marked) ? "marked" :
						((color == visited) ? "visited" :
						 	"unknown")),
				contains_pointers ? "true" : "false");
	}
};

// The header is expected to be small enough to fit in a pointer.
static_assert(sizeof(mark_and_compact_object_header) <= sizeof(void*),
		"Header is larger than expected!");

/**
 * Mark and compact garbage collector, based on the LISP2 design.
 *
 * Takes an object responsible for tracking the roots and a heap implementation
 * as template parameters.
 */
template<class RootSet, class Heap>
class mark_and_compact
{
	/**
	 * The root set object.
	 */
	RootSet m;
	/**
	 * Reference to the heap.  The heap is expected to last as long as the
	 * program.
	 */
	Heap &h;
	/**
	 * The number of objects that 
	 */
	Counter<> visited;
	/**
	 * Capability comparator that uses the base of the capabilities for comparison.
	 */
	template<typename T>
	struct cap_less
	{
		constexpr bool operator()(const T *&lhs, const T *&rhs) const 
		{
			return cheri::base(lhs) < cheri::base(rhs);
		}
	};
	/**
	 * Import the `cheri::capability` class.
	 */
	template<typename T> using capability = cheri::capability<T>;
	/**
	 * Import the header with a more convenient name.
	 */
	using object_header = mark_and_compact_object_header;
	static_assert(std::is_same<typename Heap::object_header, object_header>::value,
			"Heap must insert correct object header");
	/**
	 * The mark list (i.e. list of objects seen but not yet inspected by the
	 * collector).  This is page allocated and should be invisible to the
	 * collector.
	 */
	std::vector<void*, PageAllocator<void*>> mark_list;
	/**
	 * Mark the object referred to by the specified pointer.
	 */
	void mark(void *p)
	{
		object_header *header;
		void *obj = h.object_for_allocation(p, header);
		// If this object isn't one that the GC allocated, ignore it.
		// All non-GC memory is either a root (in which case we've seen it
		// already), or assumed not to point to GC'd objects.
		if (!obj)
		{
			return;
		}
		// Objects should only be added to the mark stack if they're really
		// objects and have not yet been seen, but if one is then skip it.
		//
		// FIXME: We should be able to assert that color is marked, find out
		// why we can't.
		if (header->color == object_header::visited)
		{
			return;
		}
		// Count the number of visited objects, for sanity checking later.
		++visited;
		// Initialise the header.
		header->color = object_header::visited;
		header->displacement = 0;
		header->contains_pointers = false;
		// Scan the contents of the object.
		capability<void*> cap(static_cast<void**>(obj));
		for (void *ptr : cap)
		{
			// Skip pointer-sized things that are not pointers.
			capability<void> ptr_as_cap(ptr);
			if (!ptr_as_cap)
			{
				continue;
			}
			// If we see a pointer, record the fact.
			object_header *pointee_header;
			header->contains_pointers = true;
			ptr = h.object_for_allocation(ptr, pointee_header);
			if (!ptr)
			{
				continue;
			}
			// If an object has not yet been seen, add it to the mark list.
			if (pointee_header->color == object_header::unmarked)
			{
				pointee_header->color = object_header::marked;
				// Note: BDW observe that having separate mark lists for nearby
				// allocations improves cache / TLB usage.
				mark_list.push_back(ptr);
			}
		}
	}
	/**
	 * Trace: Inspect all of the objects that are known live and recursively
	 * find all that are reachable from them.
	 */
	void trace()
	{
		while (!mark_list.empty())
		{
			void *p = mark_list.back();
			mark_list.pop_back();
			mark(p);
		}
	}
	/**
	 * Look at all of the roots and add any reachable objects to the stack.
	 */
	void mark_roots()
	{
		m.collect_roots_from_ranges();
		// FIXME: We should record the roots of objects that we're going to
		// move here, rather than scanning for them again.
		for (auto &r : m)
		{
			object_header *header;
			if (nullptr == h.object_for_allocation(r.second, header))
			{
				continue;
			}
			if (header->color == object_header::unmarked)
			{
				// FIXME: We should be recording this as a reachable root
				// so that we don't have to scan all of root memory twice.
				mark(r.second);
			}
		};
	}
	/**
	 * Calculate the displacements for all of the objects that we're going to move.
	 */
	void calculate_displacements()
	{
		size_t last_end = 0;
		// FIXME: Some of this logic should be in the heap, which knows about the location.
		// Ideally, we should ask the heap to give us the displacement between
		// two known-live objects, which it can also use to mark any
		// intervening objects as dead.
		for (auto alloc : h)
		{
			capability<object_header> header(alloc.first);
			capability<void> object(alloc.second);
			if (last_end == 0)
			{
				last_end = header.base();
			}
			if (header->color == object_header::unmarked)
			{
				continue;
			}
			ASSERT(header->color == object_header::visited);
			size_t base = header.base();
			header->displacement = 0;
			if (base > last_end)
			{
				header->displacement = last_end - base;
			}
			last_end = object.base() + object.length();
		}
	}
	/**
	 * Once the headers of all live objects contain their displacements,
	 * revisit all live pointers and update them to point to the new objects.
	 */
	void update_pointers()
	{
		for (auto &r : m)
		{
			object_header *header;
			if (nullptr == h.object_for_allocation(r.second, header))
			{
				continue;
			}
			ASSERT(header->color == object_header::visited);
			if (header->displacement != 0)
			{
				*r.first = h.move_reference(r.second, header->displacement);
			}
		}
		int live = 0;
		int dead = 0;
		int objects = 0;
		for (auto alloc : h)
		{
			if (alloc.first->color != object_header::visited)
			{
				dead++;
				continue;
			}
			live++;
			if (!alloc.first->contains_pointers)
			{
				continue;
			}
			capability<void*> cap(static_cast<void**>(alloc.second));
			for (void *&ptr : cap)
			{
				capability<void> ptr_as_cap;
				if (!ptr_as_cap)
				{
					continue;
				}
				object_header *pointee_header;
				void *obj = h.object_for_allocation(ptr, pointee_header);
				if (!obj || (pointee_header->displacement == 0))
				{
					continue;
				}
				ptr = h.move_reference(obj, pointee_header->displacement);
			}
		}
		fprintf(stderr, "Found %d live objects, %d dead ones\n", live, dead);
		ASSERT(visited == live);
	}
	/**
	 * Move all of the objects that we've calculated displacements for.
	 */
	void move_objects()
	{
		void *last_object = nullptr;
		for (auto alloc : h)
		{
			if (alloc.first->color != object_header::visited)
			{
				ASSERT(alloc.first->color == object_header::unmarked);
				continue;
			}
			object_header *header = alloc.first;
			// FIXME: Incremental collection could leave these in the marked state
			header->color = object_header::unmarked;
			if (header->displacement != 0)
			{
				fprintf(stderr, "Moving object: %#p\n", alloc.second);
				last_object = h.move_object(alloc.second, header->displacement);
			}
		};
		// If we've moved objects, notify the heap of the last object that
		// we've moved so that it can reuse any space after that object.
		if (last_object)
		{
			h.set_last_object(last_object);
		}
	}
	public:
	/**
	 * Constructor.  
	 */
	mark_and_compact(Heap &heap) : h(heap)
	{
		m.register_global_roots();
	}
	/**
	 * Run the collector.
	 */
	void collect()
	{
		visited = 0;
		jmp_buf jb;
		// Spill caller-save registers from any calling frames to the stack.
		// This lets later code update them as if they were simply in-memory
		// capabilities.
		if (_setjmp(jb) != 0)
		{
			return;
		}
		m.temporary_roots.clear();
		m.stop_the_world();
		// FIXME: Other threads, sandboxes
		m.add_thread(static_cast<void**>(__builtin_cheri_stack_get()));
		mark_roots();
		trace();
		ASSERT(mark_list.empty());
		calculate_displacements();
		update_pointers();
		move_objects();
		m.start_the_world();
		// FIXME: We should probably zero caller-save capability registers
		// before returning.
		_longjmp(jb, 1);
	}
};


} // Anonymous namespace
