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

#include <link.h>
#include <pthread_np.h>

#include <utility>
#include <vector>
#include <unistd.h>
#include "page.hh"
#include "utils.hh"
#include "cheri.hh"

namespace
{


/**
 * Class for managing roots.  This class is intended to be reusable across
 * multiple collector designs and encapsulates the functionality required to
 * keep track of where non-GC'd memory references GC'd objects.
 */
class Roots
{
	/**
	 * Import the `cheri::capability` template.
	 */
	template<typename T> using capability = cheri::capability<T>;
	/**
	 * A root is a pair of an pointer to a pointer and the pointer that it
	 * contains.
	 */
	using gc_root = std::pair<void**, void*>;
	/**
	 * The number of objects that we'll reserve space for in the vectors.
	 */
	static const size_t reservation_size = 64_KiB / sizeof(gc_root);
	/**
	 * Type used for vectors of roots.  These are allocated using the simple
	 * page allocator.
	 */
	using root_vector = std::vector<gc_root, PageAllocator<gc_root>>;
	/**
	 * A root range is simply a block of memory that may contain roots.  We
	 * treat it as a capability to an array of `void*` pointers, so that we can
	 * easily iterate over each pointer in the range.
	 */
	using root_range = capability<void*>;
	/**
	 * The type of a vector of root ranges.   As with `root_vector`, this is
	 * allocated using a simple page-at-a-time allocator.
	 */
	using root_range_vector = std::vector<root_range, PageAllocator<root_range>>; 
	/**
	 * The vector of persistent memory locations.  This stores the ranges where
	 * globals may be mapped and any other locations that are not expected to
	 * change over the lifetime of the program.
	 */
	root_range_vector permanent_root_ranges;
	/**
	 * The vector of temporary root ranges.  This includes stacks and similar
	 * mappings that are added and removed once per GC run.
	 */
	root_range_vector temporary_root_ranges;
	/**
	 * Helper that scans a root range and adds any valid pointers within it to
	 * the range.
	 *
	 * FIXME: We should provide a mechanism for the heap to skip pointers that
	 * don't point to GC'd memory.
	 */
	void add_range_to_roots(root_vector &roots, root_range range)
	{
		// Iterate over each pointer-sized value in the range.
		for (void *&r : range)
		{
			if (auto c = capability<void>(r))
			{
				// For now, skip the DDC / PCC values.  
				if (c.base() == 0)
				{
					continue;
				}
				roots.emplace_back(&r, r);
			}
		}
	}
	public:
	/**
	 * All of the permanent roots.  These typically only exist in constant globals.
	 */
	root_vector permanent_roots;
	/**
	 * All of the roots that are expected to be transient to this run.  This
	 * includes the current snapshot of the values in permanent root ranges.
	 */
	root_vector temporary_roots;
	/**
	 * Add the stack for a thread to the temporary roots range.
	 */
	void add_thread(void **thr)
	{
		capability<void*> c(thr);
		temporary_root_ranges.push_back(c);
	}
	/**
	 * Construct a root set.
	 */
	Roots()
	{
		permanent_root_ranges.reserve(reservation_size);
		temporary_root_ranges.reserve(reservation_size);
		permanent_roots.reserve(reservation_size);
		temporary_roots.reserve(reservation_size);
	}
	/**
	 * Reset all of the temporary ranges.  Run at the end of garbage collection.
	 */
	void clear_temporary_roots()
	{
		// Clear the vector and allow the OS to reclaim physical pages.
		temporary_roots.clear();
		temporary_roots.get_allocator().return_pages(temporary_roots.data(), temporary_roots.capacity());
	}
	/**
	 * Iterator joins together the iterators for the temporary and permanent
	 * roots.
	 */
	using iterator = SplicedForwardIterator<root_vector::iterator, root_vector::iterator>;
	/**
	 * Return forward iterator for scanning all roots.
	 */
	iterator begin()
	{
		return make_spliced_forward_iterator(temporary_roots.begin(),
		                                     temporary_roots.end(),
		                                     permanent_roots.begin());
	}
	/**
	 * End iterator.
	 */
	iterator end()
	{
		return make_spliced_forward_iterator(temporary_roots.end(),
		                                     temporary_roots.end(),
		                                     permanent_roots.end());
	}
	/**
	 * Scan all of the root ranges and record the roots that we find.
	 *
	 * FIXME: This should ideally be done in parallel, if the collector has a
	 * thread pool.
	 */
	void collect_roots_from_ranges()
	{
		// Iterate over each range
		auto iterate = [&](root_range_vector &v, root_vector &r)
			{
				for (auto roots : v)
				{
					add_range_to_roots(r, roots);
				}
			};
		iterate(temporary_root_ranges, temporary_roots);
		iterate(permanent_root_ranges, permanent_roots);
	}
	/**
	 * Query the current environment and add ranges to the relevant range set.
	 */
	void register_global_roots()
	{
		int found = dl_iterate_phdr(
			[](struct dl_phdr_info *pinfo, size_t, void *data) -> int
			{
					auto *r = static_cast<Roots*>(data);
					for (decltype(pinfo->dlpi_phnum) i=0 ; i<pinfo->dlpi_phnum ; ++i)
					{
						const auto *phdr = &pinfo->dlpi_phdr[i];
						// Skip over anything that isn't loaded.
						if (phdr->p_type != PT_LOAD)
						{
							continue;
						}
						// FIXME: Need a better dl_iterate_phdr to not need this
						auto segment_addr = root_range::default_data_capability();
						segment_addr += phdr->p_vaddr / sizeof(void*);
						segment_addr.set_bounds(phdr->p_memsz / sizeof(void*));
						// If the region is writeable, then assume that
						// capabilities might end up anywhere.  Otherwise,
						// assume that we only care about ones that are there
						// now.
						if ((phdr->p_flags & PF_W) == PF_W)
						{
							r->permanent_root_ranges.push_back(segment_addr);
						}
						else
						{
							r->add_range_to_roots(r->permanent_roots, segment_addr);
						}
					}
					return 1;
			}, this);
	}
	/**
	 * For stop-the-world collectors, stop all threads.  This exists here to
	 * isolate collectors from pthreads details.
	 *
	 * FIXME: This should find all of the stopped threads and their stacks.
	 */
	void stop_the_world()
	{
		pthread_suspend_all_np();
	}
	/**
	 * For stop-the-world collectors, restart all threads.  This exists here to
	 * isolate collectors from pthreads details.
	 */
	void start_the_world()
	{
		pthread_resume_all_np();
	}
};

}
