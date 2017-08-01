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
#include <array>

/**
 * Class representing a fixed-size array of bits.  The atomic flag, if set,
 * ensures that set and clear operations are atomic, but assumes that any of
 * the O(S) operations are not performed concurrently.
 */
template<size_t S, bool IsAtomic=false>
class BitSet
{
	/**
	 * The type used for storage.  Bits are stored in the host-native endian
	 * within an array of elements of this size.
	 */
	using nonatomic_bitfield_word = uint64_t;
	/**
	 * The number of bits that can be stored per word.
	 */
	static const int bits_per_word = sizeof(nonatomic_bitfield_word) * 8;
	/**
	 * The number of words needed to represent this array: `S` divided by the word size and rounded up.
	 */
	static const size_t words = (S+(bits_per_word-1)) / bits_per_word;
	/**
	 * The type used for the word.  This will be an atomic type if the atomic
	 * flag is set.
	 */
	using bitfield_word = typename std::conditional<IsAtomic, std::atomic<uint64_t>, uint64_t>::type;
	/**
	 * The storage for the bitfield.
	 */
	std::array<bitfield_word, words> bits;
	/**
	 * Compare and exchange on an atomic type.  Used so that instantiations
	 * will call nonatomic or atomic overloads depending on the type of
	 * `bitfield_word`.
	 */
	bool cmpexch(std::atomic<uint64_t> &w, uint64_t &expected, uint64_t desired)
	{
		return w.compare_exchange_strong(expected, desired);
	}
	/**
	 * Compare and exchange on a non-atomic type.  Used so that instantiations
	 * will call nonatomic or atomic overloads depending on the type of
	 * `bitfield_word`.
	 */
	bool cmpexch(uint64_t &w, uint64_t &expected, uint64_t desired)
	{
		// After inlining, `w == expected` should always be optimised to true.
		if (w == expected)
		{
			w = desired;
			return true;
		}
		return false;
	}
	public:
	/**
	 * Constructor.  Intialises bits to zero.
	 */
	BitSet()
	{
		for (auto &w : bits)
		{
			w = 0;
		}
	}
	/**
	 * Accessor.  Returns the bit at the specified index.
	 */
	bool operator[](size_t i) const
	{
		ASSERT(i < S);
		size_t word = i / bits_per_word;
		size_t bit = i % bits_per_word;
		return bits[word] & (1ULL << ((bits_per_word-1) - bit));
	}
	/**
	 * Set the bit at the specified index to 1.
	 */
	void set(size_t i)
	{
		ASSERT(i < S);
		size_t word = i / bits_per_word;
		size_t bit = i % bits_per_word;
		bitfield_word &w = bits[word];
		bitfield_word desired;
		nonatomic_bitfield_word expected = w.load();
		do {
			desired = expected | (1ULL << ((bits_per_word-1) - bit));
		} while (!cmpexch(w, expected, desired));
	}
	/**
	 * Set the bit at the specified index to 0.
	 */
	void clear(size_t i)
	{
		ASSERT(i < S);
		size_t word = i / bits_per_word;
		size_t bit = i % bits_per_word;
		bitfield_word &w = bits[word];
		nonatomic_bitfield_word desired;
		nonatomic_bitfield_word expected = w;
		do {
			desired = expected & ~(1ULL << ((bits_per_word-1)-bit));
		} while (!cmpexch(w, expected, desired));
	}
	/**
	 * Returns the index of the first zero in the set.
	 *
	 * WARNING: This is not atomic.
	 */
	size_t first_zero()
	{
		size_t idx = 0;
		for (size_t i=0 ; i<words ; i++, idx+=64)
		{
			uint64_t word = bits[i];
			if (word == 0)
			{
				return idx;
			}
			if (word != 0xffffffffffffffffULL)
			{
				if (__builtin_clzll(word) > 0)
				{
					return idx;
				}
				return idx + __builtin_clzll(~word);
			}
		}
		return S;
	}
	/**
	 * Returns the index of the first bit that is set after the specified
	 * index, or `S` if no bit is set after the specified index..
	 *
	 * WARNING: This is not atomic.
	 */
	size_t one_after(size_t idx)
	{
		idx++;
		size_t bit_idx = (idx % 64);
		for (size_t i=idx/64 ; i<words ; i++, idx+=64)
		{
			uint64_t word = bits[i];
			if (word == 0)
			{
				bit_idx = 0;
				continue;
			}
			// Skip any words that have no non-zero bits set.
			// Zero all bits before the index that we're looking for.
			uint64_t mask = (bit_idx == 0) ? -1ULL : ~((-1ULL) << (64 - bit_idx));
			word &= mask;
			if (word == 0)
			{
				bit_idx = 0;
				continue;
			}
			return (i*64) + __builtin_clzll(word);
		}
		return S;
	}
};
