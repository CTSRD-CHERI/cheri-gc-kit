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
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits.h>
#include "cheri.hh"

namespace {
// Pure virtual functions are implemented as a call to this.  Ensure that we
// provide a copy when linking, but as a private symbol so that nothing
// external will see our version.
extern "C" void __cxa_pure_virtual() { abort(); }
}
// unwind.h doesn't expose `_Unwind_Backtrace` unless `_GNU_SOURCE` is defined.
#define _GNU_SOURCE
#include <unwind.h>
#undef _GNU_SOURCE
/**
 * Assertion failure function.  This improves C's assert by providing
 * backtraces on failure.
 */
[[noreturn]]
void assert_fail(const char *func, const char *file, int line, const char *err)
{
	fprintf(stderr, "Assertion failed: %s, function %s, file %s:%d\n", err, func, file, line);
	int depth = 0;
	fprintf(stderr, "-- BACKTRACE --\n");
	_Unwind_Backtrace([](struct _Unwind_Context *context, void *arg)
		{
			int &depth = *(int*)arg;
			fprintf(stderr, "[%2d] %#p\n", depth++, reinterpret_cast<void*>(_Unwind_GetIP(context)));
			return _URC_NO_REASON;
		}, (void*)&depth);
	fprintf(stderr, "-- END BACKTRACE --\n");
	abort();
}
#ifndef NDEBUG
#define ASSERT(x) \
	do { \
		if (!(x))\
		{\
			assert_fail(__func__, __FILE__, __LINE__, #x);\
		}\
	} while(0)
#else
#define ASSERT(x) do { } while(0)
#endif

namespace {

/**
 * Compile-time evaluation of the base-2 logarithm of a number.
 */
constexpr int log2(size_t sz)
{
	return sz == 0 ? -1 : sizeof(size_t) * CHAR_BIT - __builtin_clzll(sz) - 1;
}

/**
 * Template wrapper around the `log2` function.  This forces compile-time evaluation.
 */
template<size_t sz>
constexpr int log2()
{
	return log2(sz);
}

static_assert(log2<1>() == 0, "log2 is broken");
static_assert(log2<2>() == 1, "log2 is broken");
static_assert(log2<4>() == 2, "log2 is broken");
static_assert(log2<9223372036854775808ULL>() == 63, "log2 is broken");

/**
 * Round up the argument to be a multiple of the template parameter.
 */
template<long long multiple>
long long roundUp(long long val)
{
	int isPositive = (val >= 0);
	return ((val + isPositive * (multiple - 1)) / multiple) * multiple;
}

/**
 * User defined literal for binary kilobytes.
 */
constexpr unsigned long long int operator"" _KiB(unsigned long long int k)
{
	return k * 1024;
}

/**
 * User defined literal for binary megabytes.
 */
constexpr unsigned long long int operator"" _MiB(unsigned long long int m)
{
	return m * 1024 * 1024;
}

/**
 * User defined literal for binary gigabytes.
 */
constexpr unsigned long long int operator"" _GiB(unsigned long long int g)
{
	return g * 1024 * 1024 * 1024;
}

/**
 * Construct a pointer for a moved object, preserving the offset and
 * permissions.
 */
void *move_capability(void* base, void *src, ptrdiff_t displacement)
{
	cheri::capability<void> b(base);
	cheri::capability<void> s(src);
	size_t addr = s.base() + displacement;
	b.set_offset(addr - b.base());
	b.set_bounds(s.length());
	b.mask_permissions(s.permissions());
	b.set_offset(s.offset());
	return b;
}

/**
 * Class for splicing two iterators of the same return type together.
 * Iterates over all objects exposed by the first and then starts iterating
 * over the second.  Does not perform bounds checking on the second iterator
 * and so should always be used with a matching end iterator (for example in
 * range-based for loops).
 */
template<typename It1, typename It2>
class SplicedForwardIterator
{
	/**
	 * Shorthand for the type of this template instantiation.
	 */
	using ThisType = SplicedForwardIterator<It1, It2>;
	/**
	 * The current iterator to the first collection.
	 */
	It1 i1;
	/**
	 * The iterator to the end of the first collection.  Once `i1 != e1`
	 * becomes false, this object will move to using `i2`.
	 */
	It1 e1;
	/**
	 * The iterator into the second collection.
	 */
	It2 i2;
	/**
	 * Helper method determines whether we are still inspecting the first
	 * collection.
	 */
	bool is_first()
	{
		return i1 != e1;
	}
	public:
	/**
	 * Construct a spliced iterator.  Given two collections, `a` and `b`, begin
	 * and end iterators that allow iterating over both should be constructed
	 * with `SplicedForwardIterator(a.begin(), a.end(), b.begin())` and
	 * `SplicedForwardIterator(a.end(), a.end(), b.end())`, respectively.
	 */
	SplicedForwardIterator(It1 &&start1, It1 &&end1, It2 &&start2)
		: i1(start1), e1(end1), i2(start2) {}
	/**
	 * Increments the iterator by iterating one of the two underlying
	 * iterators, depending on which we're currently using.
	 */
	SplicedForwardIterator<It1, It2> &operator++()
	{
		if (is_first())
		{
			++i1;
		}
		else
		{
			++i2;
		}
		return *this;
	}
	/**
	 * Dereference the current iterator.
	 */
	decltype(*i1) operator*()
	{
		if (is_first())
		{
			return *i1;
		}
		return *i2;
	}
	/**
	 * Compare two iterators for inequality.  See the constructor documentation
	 * for how to ensure that this behaves correctly.
	 */
	bool operator!=(const ThisType &other)
	{
		return (i1 != other.i1) || (i2 != other.i2);
	}
};

/**
 * Helperfunction for constructing spliced iterators.  This allows the type to
 * be deduced, rather than specified explicitly.
 */
template<typename It1, typename It2>
class SplicedForwardIterator<It1, It2> make_spliced_forward_iterator(It1 &&start1, It1 &&end1, It2 &&start2)
{
	return SplicedForwardIterator<It1, It2>(std::move(start1), std::move(end1), std::move(start2));
}

/**
 * External function that clears all callee-save capability registers.
 */
extern "C" void clear_regs();

} // Anonymous namespace
