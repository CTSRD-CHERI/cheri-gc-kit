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

#include <atomic>

/**
 * Optionally-atomic counter.  This compiles away to nothing when not in debug
 * mode.
 */
template<bool IsAtomic=false>
struct Counter
{
#ifdef NDEBUG
	/**
	 * Value to ensure that the size of this structure is 0 in no-debug mode.
	 */
	int x[0];
	/**
	 * Empty assignment operator.  Does nothing in non-debug mode.
	 */
	Counter &operator=(uint64_t v)
	{
		return *this;
	}
	/**
	 * Empty increment operator.  Does nothing in non-debug mode.
	 */
	Counter &operator++()
	{
		return *this;
	}
	/**
	 * Get the counter value.  Unspecified when not in debug mode.
	 */
	uint64_t value()
	{
		return 0;
	}
	/**
	 * Implicit conversion operator.  Unspecified when not in debug mode.
	 */
	operator uint64_t()
	{
		return 0;
	}
	/**
	 * Equality test, unspecified in debug mode.
	 */
	template<typename T>
	bool operator==(T v)
	{
		return false;
	}
#else
	/**
	 * assignment operator.
	 */
	Counter &operator=(uint64_t v)
	{
		val = v;
		return *this;
	}
	/**
	 * Increment operator.
	 */
	Counter &operator++()
	{
		++val;
		return *this;
	}
	/**
	 * Get the counter value.
	 */
	uint64_t value()
	{
		return val;
	}
	/**
	 * Get the counter value.
	 */
	operator uint64_t()
	{
		return val;
	}
	/**
	 * Compare the counter value to a number.
	 */
	template<typename T>
	bool operator==(T v)
	{
		return v == val;
	}
	private:
	/**
	 * Counter value.
	 */
	typename std::conditional<IsAtomic, std::atomic<uint64_t>, uint64_t>::type val = 0;
#endif
};
