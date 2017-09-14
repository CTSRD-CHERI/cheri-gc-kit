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

// Make sure that we don't depend on libc++ being linked.
#define _LIBCPP_EXTERN_TEMPLATE(...)
#include <cstdlib>
#include <cstdio>

#include "bump_the_pointer_or_large.hh"
#include "roots.hh"
#include "mark_and_compact.hh"

/**
 * Testing implementation of a GC.  You should be able to change the
 * definitions of `heap_type` and `gc_type` to run the same test with a
 * different GC implementation.
 */
namespace
{

/**
 * For testing, we'll use an 8MiB heap.
 */
using heap_type = bump_the_pointer_or_large_heap<8_MiB, mark_and_compact_object_header>;
/**
 * Mark and compact heap.
 */
using gc_type = mark_and_compact<Roots, std::remove_pointer<heap_type>::type>;
/**
 * Allocate the heap and return it.  This function should only be called once.
 */
heap_type *get_heap_once()
{
	// Allocate the heap.
	auto *h = heap_type::create();
	// Allocate the GC.
	PageAllocator<gc_type> a;
	auto *gc = new (a.allocate(sizeof(gc_type))) gc_type(*h);
	// Set up the GC callback in the heap.
	auto run_gc = [=]()
		{
			gc->collect();
		};
	h->set_gc(run_gc);
	return h;
}
/**
 * Get the heap.  This includes a simplified equivalent of the thread-safe
 * static initialiser to ensure that we only ever have one version.
 */
heap_type *get_heap()
{
	// The heap.
	static heap_type *h;
	// A flag protecting the initialisation.  This is 0 initially, 1 while one
	// thread is initialising `h`, and `2` afterwards.
	static std::atomic<int> init_flag; 
	// Fast path: Most of the time, this should be the only reached code path.
	if (h != nullptr)
	{
		return h;
	}
	int expected = 0;
	int desired = 1;
	// If the we can transition the flag from 0 -> 1, no other thread is (yet)
	// trying to initialise `h`, so we go ahead.
	if (init_flag.compare_exchange_strong(expected, desired))
	{
		ASSERT(expected == 0);
		h = get_heap_once();
		ASSERT(h != nullptr);
		expected = 1;
		desired = 2;
		// Now we should be able to move the flag from 1 -> 2.  No other thread
		// should attempt this transition.
		bool done = init_flag.compare_exchange_strong(expected, desired);
		ASSERT(done);
		return h;
	}
	// If another thread beat us to start initialising, spin until they've finished.
	while (init_flag != 2) { }
	ASSERT(h != nullptr);
	return h;
}
}

/**
 * Public interface to allocate garbage-collected memory.
 */
extern "C"
void *GC_malloc(size_t size)
{
	return get_heap()->alloc(size);
}

/**
 * Public interface to force early garbage collection.
 */
extern "C"
void GC_collect()
{
	get_heap()->collect();
}

/**
 * Simple list structure.  Stores a 
 */
struct list
{
	/**
	 * Next pointer.
	 */
	list *next = nullptr;
	/**
	 * Value stored in this list element.
	 */
	int val;
	/**
	 * Constructor, takes the value to store in this list element.
	 */
	list(int i) : val(i) {}
	/**
	 * Operator new implementation that returns GC'd memory.
	 */
	void *operator new(size_t sz)
	{
		return GC_malloc(sz);
	}
};

int main()
{
	// Allocate a linked list.
	list *head = new list(0);
	for (int i=1 ; i<100 ; i++)
	{
		list *l = new list(i);
		l->next = head;
		head = l;
	}
	// Run the GC, should not find any garbage.
	GC_collect();
	fprintf(stderr, "Head: %#p\n", head);
	// Clear the next element of the head.  Should now have 99 dead objects.
	fprintf(stderr, "Truncating list!\n");
	head->next = nullptr;
	// Clear any temporary registers, so that we don't accidentally have
	// pointers in them.
	clear_regs();
	std::atomic_thread_fence(std::memory_order_seq_cst);
	// Run the GC again, 99 objects should now be deallocated.
	fprintf(stderr, "Run collector again\n");
	fprintf(stderr, "Head val: %d\n", head->val);
	GC_collect();
	// Head value should be the same, but head object should be moved.
	fprintf(stderr, "Head: %#p\n", head);
	fprintf(stderr, "Head val: %d\n", head->val);
}
