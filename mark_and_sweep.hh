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
#include "mark.hh"

namespace
{

// Forward declare mark_and_sweep so that it can be a friend of the header.
template<class RootSet, class Heap>
class mark_and_sweep;

/**
 * Object header for this collector.  Declared outside the class so that its
 * type doesn't depend on the template arguments.
 *
 * This is intended to be stored in a separate location to the rest of the
 * allocation and so is designed to be tightly packed.
 */
class mark_and_sweep_object_header
{
	template<class RootSet, class Heap>
	friend class mark_and_sweep;
	/**
	 * The GC state for this object.
	 */
	enum : unsigned char
	{
		/// Object has not been seen by the GC yet
		unmarked = 0,
		/// Object has been marked as live, but it has not yet been scanned.
		marked,
		/// Object has been visited.
		visited
	} color:2;
	/**
	 * Does the object contain any pointers?
	 */
	bool contains_pointers:1;
	public:
	/**
	 * Has this object been free'd?
	 */
	bool is_free:1;
	/**
	 * Helper for debugging: dump the header in a human-readable format.
	 */
	void dump()
	{
		fprintf(stderr, "Color: %s, contains pointers: %s\n",
				(color == unmarked) ? "unmarked" :
					((color == marked) ? "marked" :
						((color == visited) ? "visited" :
						 	"unknown")),
				contains_pointers ? "true" : "false");
	}
	/**
	 * Set the colour to marked.
	 *
	 * This method avoids the need for the marker to know the implementation
	 * details of the header.
	 */
	void set_marked()
	{
		color = marked;
	}
	/**
	 * Set the colour to visited.
	 *
	 * This method avoids the need for the marker to know the implementation
	 * details of the header.
	 */
	void set_visited()
	{
		color = visited;
	}
	/**
	 * Set the contains-pointers flag.
	 *
	 * This method avoids the need for the marker to know the implementation
	 * details of the header.
	 */
	void set_contains_pointers()
	{
		contains_pointers = true;
	}
	/**
	 * Reset the state.
	 *
	 * This method avoids the need for the marker to know the implementation
	 * details of the header.
	 */
	void reset()
	{
		color = unmarked;
		contains_pointers = false;
	}
	/**
	 * Return whether the colour is visited.
	 *
	 * This method avoids the need for the marker to know the implementation
	 * details of the header.
	 */
	bool is_visited()
	{
		return color == visited;
	}
	/**
	 * Return whether the colour is marked.
	 *
	 * This method avoids the need for the marker to know the implementation
	 * details of the header.
	 */
	bool is_marked()
	{
		return color == marked;
	}
	/**
	 * Return whether the colour is unmarked.
	 *
	 * This method avoids the need for the marker to know the implementation
	 * details of the header.
	 */
	bool is_unmarked()
	{
		return color == unmarked;
	}
};

struct skip_free
{
	bool operator()(mark_and_sweep_object_header &h, void *obj)
	{
		if (!h.is_free)
		{
			return true;
		}
		return false;
	}
};

// The header is expected to be a single byte.
static_assert(sizeof(mark_and_sweep_object_header) == 1,
		"Header is larger than expected!");

/**
 * Mark and compact garbage collector, based on the LISP2 design.
 *
 * Takes an object responsible for tracking the roots and a heap implementation
 * as template parameters.
 */
template<class RootSet, class Heap>
class mark_and_sweep : mark<RootSet, Heap, mark_and_sweep_object_header, skip_free>
{
	/**
	 * Import the superclass name.
	 */
	using Super = mark<RootSet, Heap, mark_and_sweep_object_header, skip_free>;
	/**
	 * Import the mark set from the superclass.
	 */
	using Super::m;
	/**
	 * Import the heap from the superclass.
	 */
	using Super::h;
	/**
	 * Import the mark list from the superclass.
	 */
	using Super::mark_list;
	/**
	 * Import the `cheri::capability` class.
	 */
	template<typename T> using capability = cheri::capability<T>;
	/**
	 * Import the header with a more convenient name.
	 */
	using object_header = mark_and_sweep_object_header;
	static_assert(std::is_same<typename Heap::object_header, object_header>::value,
			"Heap must insert correct object header");
	void free_unmarked()
	{
		for (auto alloc : h)
		{
			ASSERT(!alloc.second->is_marked() || alloc.second->is_free);
			if (alloc.second->is_free)
			{
				memset(cheri::set_offset(alloc.first, 0), 0, cheri::length(alloc.first));
				++free_reachable;
			}
			if (alloc.second->is_unmarked())
			{
				h.free(alloc.first);
			}
			else
			{
				alloc.second->reset();
			}
		}
	}
	public:
	/**
	 * Import the visited counter from the superclass and make it public.
	 */
	using Super::visited;
	/**
	 * Counter for the number of free objects that are still reachable.
	 */
	Counter<> free_reachable;
	/**
	 * Constructor.
	 */
	mark_and_sweep(Heap &heap) : Super(heap)
	{
	}
	/**
	 * Run the collector.
	 */
	void collect()
	{
		visited = 0;
		free_reachable = 0;
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
		Super::mark_roots();
		Super::trace();
		free_unmarked();
		ASSERT(mark_list.empty());
		m.start_the_world();
		// FIXME: We should probably zero caller-save capability registers
		// before returning.
		_longjmp(jb, 1);
	}
	void free(void *obj)
	{
		mark_and_sweep_object_header *header = nullptr;
		h.object_for_allocation(obj, header);
		if (header)
		{
			header->is_free = true;
		}
	}
};


} // Anonymous namespace
