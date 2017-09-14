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

#include <type_traits>

namespace {

/**
 * Helper function to determine if a number is prime, given a possible divisor.
 * This is evaluated at compile time to determine bucket sizes and is intended
 * to be called only from the single-argument version.
 */
constexpr bool is_prime(unsigned i, unsigned prev)
{
	if (prev < 2)
	{
		return true;
	}
	return (i % prev == 0) ? false : is_prime(i, prev-1);
}

/**
 * Function to determine if a number is prime.  Returns true if `i` is prime,
 * false otherwise.
 */
constexpr bool is_prime(unsigned i)
{
	return (i < 4) ? true : is_prime(i, i-1);
}

static_assert(is_prime(17), "is_prime() is broken!");
static_assert(is_prime(19), "is_prime() is broken!");
static_assert(!is_prime(20), "is_prime() is broken!");
static_assert(is_prime(23), "is_prime() is broken!");

/**
 * Generator function that produces the next number that is either a prime
 * number of a power of two.  These are used to define bucket sizes.
 */
constexpr unsigned next_prime_or_power_of_two(unsigned candidate)
{
	if (1<<log2(candidate) == candidate)
	{
		return candidate;
	}
	if (is_prime(candidate))
	{
		return candidate;
	}
	return next_prime_or_power_of_two(candidate+1);
}

/**
 * Template wrapping `next_prime_or_power_of_two` to force compile-time
 * evaluation.
 */
template<unsigned Counter>
struct MediumBucketCandidate
{
	static const unsigned value = next_prime_or_power_of_two(MediumBucketCandidate<Counter-1>::value + 1);
};

/**
 * Specialisation for the base case.
 */
template<>
struct MediumBucketCandidate<1>
{
	static const unsigned value = next_prime_or_power_of_two(1);
};

/**
 * Template providing sizes of medium bucket sizes.  This assumes that the
 * first 20 buckets are calculated by some other mechanism.
 */
template<unsigned N>
struct MediumBucketSize 
{
	static_assert(N > 20, "The first 20 buckets are small");
	static const unsigned value = MediumBucketCandidate<N-10>::value * cache_line_size;
};

/**
 * Helper function that defines the size of a small bucket.
 */
constexpr unsigned SmallBucketSize(size_t i)
{
	return (i > 20) ? 0 : (i < 5 ? ((i+1) * 8) : ((1<<(((i+12) >> 2))) * (((i+12) & 0b11) + 4)));
}

/**
 * Template that defines all of the bucket sizes.
 */
template<int Bucket>
struct BucketSize
{
	static const int value = std::conditional<Bucket<21,
				 std::integral_constant<unsigned, SmallBucketSize(Bucket)>,
				 MediumBucketSize<Bucket>>::type::value;
};

#if 0
#undef fprintf
template<int N>
void print_primes()
{
	print_primes<N-1>();
	int x = BucketSize<N>::value;
	fprintf(stderr, "Medium %d = %d (%d)\n", N, x, x / cache_line_size);
}
template<>
void print_primes<0>()
{
}
#endif

static_assert(BucketSize<21>::value == 1088,
              "Medium bucket numbering starts in the wrong place");

/**
 * Template that maps from a size to a bucket.  Takes the size of the largest
 * bucket as an argument.  Recursively calls itself with a smaller bucket size
 * and relies on the 
 */
template<int Bucket>
constexpr int bucket_for_size(size_t size)
{
	if ((size <= BucketSize<Bucket>::value) && (size > BucketSize<Bucket-1>::value))
	{
		return Bucket;
	}
	if (size <= BucketSize<Bucket/4>::value)
	{
		return bucket_for_size<Bucket/4>(size);
	}
	if (size <= BucketSize<Bucket/2>::value)
	{
		return bucket_for_size<Bucket/2>(size);
	}
	return bucket_for_size<Bucket-1>(size);
}
/**
 * Base case specialisation.
 */
template<>
constexpr int bucket_for_size<0>(size_t size)
{
	return (size <= BucketSize<0>::value) ? 0 : -1;
}

/**
 * Function that maps from size to bucket.  Uses `bucket_for_size<>` for medium
 * buckets.
 */
__attribute__((always_inline))
constexpr int bucket_for_size(size_t sz)
{
	if (sz <= 64)
	{
		size_t div8 = sz >> 3;
		if (div8 << 3 == sz)
		{
			div8--;
		}
		return div8;
	}
	if (sz <= 320)
	{
		int z = __builtin_clzll(sz);
		size_t r = sz + (1ULL << (61-z)) -1;
		int y = __builtin_clzll(r);
		int bin = (4 * (60-y)+((r>>(61-y))&3)) - 8;
		assert(bin == bucket_for_size<25>(sz));
		assert(SmallBucketSize(bin) >= sz);
		assert(SmallBucketSize(bin-1) < sz);
		return bin;
	}
	return bucket_for_size<fixed_buckets>(sz);
}

}
