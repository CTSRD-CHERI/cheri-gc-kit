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

// Forward declare mark_and_compact so that it can be a friend of the header.
template<class RootSet, class Heap>
class mark_and_compact;

/**
 * Object header for this collector.  Declared outside the class so that its
 * type doesn't depend on the template arguments.
 */
class alignas(void*) mark_and_compact_object_header
{
	template<class RootSet, class Heap>
	friend class mark_and_compact;
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
	public:
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
		displacement = 0;
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
class mark_and_compact : mark<RootSet, Heap, mark_and_compact_object_header>
{
	/**
	 * Import the superclass name.
	 */
	using Super = mark<RootSet, Heap, mark_and_compact_object_header>;
	/**
	 * Import the mark set from the superclass.
	 */
	using Super::m;
	/**
	 * Import the heap from the superclass.
	 */
	using Super::h;
	/**
	 * Import the visited counter from the superclass.
	 */
	using Super::visited;
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
	using object_header = mark_and_compact_object_header;
	static_assert(std::is_same<typename Heap::object_header, object_header>::value,
			"Heap must insert correct object header");
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
	mark_and_compact(Heap &heap) : Super(heap)
	{
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
		Super::mark_roots();
		Super::trace();
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
