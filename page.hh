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

#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <array>
#include <cassert>
#include "config.hh"
#include "cheri.hh"

namespace {

using cheri::vaddr_t;

template<typename T>
struct PageAllocator
{
	typedef T value_type;
	T* allocate_aligned(std::size_t n, int align=-1)
	{
		size_t len = n*sizeof(T);
		const int align_mask = align == -1 ? MAP_ALIGNED_SUPER : MAP_ALIGNED(align);
		void *alloc = mmap(nullptr, len, PROT_READ | PROT_WRITE,
		                   MAP_ANON | MAP_PRIVATE | align_mask, -1, 0);
		if (alloc == MAP_FAILED)
		{
			return nullptr;
		}
		return static_cast<T*>(alloc);
	}
	T* allocate(std::size_t n)
	{
		return allocate_aligned(n, log2<chunk_size>());
	}
	void deallocate(T* p, std::size_t n)
	{
		size_t len = n*sizeof(T);
		munmap(static_cast<void*>(p), len);
	}
	void return_pages(T* p, std::size_t n)
	{
		size_t len = n*sizeof(T);
		madvise(static_cast<void*>(p), len, MADV_FREE);
	}
};

template<typename T>
struct PageAllocated
{
	using allocator = PageAllocator<T>;
	void* operator new(size_t len)
	{
		allocator a;
		return static_cast<void*>(a.allocate(len / sizeof(T)));
	}
	void operator delete(void* ptr, size_t len)
	{
		allocator a;
		a.deallocate(ptr, len / sizeof(T));
	}
};

}
