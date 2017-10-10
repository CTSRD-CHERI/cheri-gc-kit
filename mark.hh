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
 * Simple template class that returns true from its call method.  This is the
 * default filter for marking - always mark every object.
 */
template<class Header>
struct always_true
{
	/**
	 * Returns true.  This implementation is intended to be optimised away.
	 */
	bool operator()(const Header &, const void*)
	{
		return true;
	}
};

/**
 * Mark and compact garbage collector, based on the LISP2 design.
 *
 * Takes an object responsible for tracking the roots and a heap implementation
 * as template parameters, as well as a header and a filter class.  The
 * allocator is expected to associate an instance of the header class with each
 * object.  The header object must respond to queries related to mark state.
 *
 * The filter class allows some objects to be ignored.  For example, if the GC
 * can guarantee that an object has not been used to store pointers, then it
 * can skip scanning.
 */
template<class RootSet, class Heap, class Header, class Filter=always_true<Header>>
class mark
{
	protected:
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
	 * Import the `cheri::capability` class.
	 */
	template<typename T> using capability = cheri::capability<T>;
	/**
	 * The mark list (i.e. list of objects seen but not yet inspected by the
	 * collector).  This is page allocated and should be invisible to the
	 * collector.
	 */
	std::vector<void*, PageAllocator<void*>> mark_list;
	/**
	 * Constructor.
	 */
	mark(Heap &heap) : h(heap)
	{
		m.register_global_roots();
	}
	/**
	 * Mark the object referred to by the specified pointer.
	 */
	void mark_pointer(void *p)
	{
		Header *header;
		void *obj = h.object_for_allocation(p, header);
		// If this object isn't one that the GC allocated, ignore it.
		// All non-GC memory is either a root (in which case we've seen it
		// already), or assumed not to point to GC'd objects.
		if (!obj)
		{
			return;
		}
		Filter f;
		// If the GC policy tells us to ignore this object, then skip it.
		if (!f(*header, obj))
		{
			return;
		}
		// Objects should only be added to the mark stack if they're really
		// objects and have not yet been seen, but if one is then skip it.
		//
		// FIXME: We should be able to assert that color is marked, find out
		// why we can't.
		if (header->is_visited())
		{
			return;
		}
		// Count the number of visited objects, for sanity checking later.
		++visited;
		// Initialise the header.
		header->reset();
		header->set_visited();
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
			Header *pointee_header;
			header->set_contains_pointers();
			ptr = h.object_for_allocation(ptr, pointee_header);
			if (!ptr)
			{
				continue;
			}
			// If an object has not yet been seen, add it to the mark list.
			if (pointee_header->is_unmarked())
			{
				pointee_header->set_marked();
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
			mark_pointer(p);
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
			Header *header;
			if (nullptr == h.object_for_allocation(r.second, header))
			{
				continue;
			}
			if (header->is_unmarked())
			{
				// FIXME: We should be recording this as a reachable root
				// so that we don't have to scan all of root memory twice.
				mark_pointer(r.second);
			}
		};
	}
};


} // Anonymous namespace
