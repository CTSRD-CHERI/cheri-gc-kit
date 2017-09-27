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
#include "utils.hh"
#include "config.hh"

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
 * Returns the size of the largest small bucket.  This depends on the size of
 * `void*`, because small allocations are all `void*`-aligned.
 */
constexpr unsigned largest_small_bucket()
{
	switch (sizeof(void*))
	{
		case 8: return 20;
		case 16: return 18;
		case 32: return 12;
		default:
			assert(0);
			return -1;
	}
}

/**
 * Returns the size of the largest medium bucket.
 */
constexpr unsigned largest_medium_bucket()
{
	// This is value is the size of the largest medium bucket that is less than
	// 32KiB.  This doesn't change depending on pointer size, so we're hard
	// coding it rather than computing it.
	return 107;
}


/**
 * Return the medium bucket number that is used to implement bucket `n`.  The
 * number of small buckets depends on the size of the pointer, but the first
 * bucket is always the one that handles 1088-byte allocations.
 */
constexpr unsigned medium_bucket_for_bucket(unsigned n)
{
	return n + 10 - largest_small_bucket();
}


/**
 * Template providing sizes of medium bucket sizes.  This assumes that the
 * first 20 buckets are calculated by some other mechanism.
 */
template<unsigned N>
struct MediumBucketSize 
{
	static_assert(N > largest_small_bucket(), "The first buckets are small");
	static const unsigned value = MediumBucketCandidate<medium_bucket_for_bucket(N)>::value * cache_line_size;
};


/**
 * Helper function that defines the size of a small bucket.
 */
constexpr unsigned SmallBucketSize(size_t i)
{
	// Small buckets should not be for large bucket indexes
	if (i > largest_small_bucket())
	{
		return -1;
	}
	// The smallest buckets are multiples of the pointer size
	if (i < 5)
	{
		return ((i+1) * sizeof(void*));
	}
	// The remaining small buckets are computed by the sequence of bits with a
	// 1, two arbitrary digits, and then all zeroes.  At least enough low bits
	// must be zero to guarantee pointer alignment.
	return ((1<<(((i+12) >> 2))) * ((((i+12) & 0b11) + 4) << (log2<sizeof(void*)>() - 3)));
}

constexpr size_t large_bucket_size(int bucket)
{
	if (bucket < largest_medium_bucket())
	{
		return 0;
	}
	bucket -= largest_medium_bucket();
	return (bucket * page_size) + 32_KiB;
}

/**
 * Template that defines all of the bucket sizes.
 */
template<int Bucket>
struct BucketSize
{
	static const int value = std::conditional<(Bucket<=largest_small_bucket()),
				 std::integral_constant<unsigned, SmallBucketSize(Bucket)>,
				 typename std::conditional<(Bucket<=largest_medium_bucket()),
				   MediumBucketSize<Bucket>,
				   std::integral_constant<unsigned, large_bucket_size(Bucket)>>::type>::type::value;
};

static_assert(BucketSize<largest_medium_bucket()>::value < 32_KiB,
		"Largest medium bucket is too big");
static_assert(BucketSize<largest_medium_bucket()+1>::value >= 32_KiB,
		"Largest medium bucket is too small");

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

static_assert(BucketSize<largest_small_bucket()+1>::value == 1088,
              "Medium bucket numbering starts in the wrong place");

/**
 * Template that maps from a size to a bucket.  Takes the size of the largest
 * bucket as an argument.  Recursively calls itself with a smaller bucket size
 * and relies on the 
 */
template<int Bucket>
constexpr int small_bucket_for_size(size_t size)
{
	if ((size <= BucketSize<Bucket>::value) && (size > BucketSize<Bucket-1>::value))
	{
		return Bucket;
	}
	if (size <= BucketSize<Bucket/4>::value)
	{
		return small_bucket_for_size<Bucket/4>(size);
	}
	if (size <= BucketSize<Bucket/2>::value)
	{
		return small_bucket_for_size<Bucket/2>(size);
	}
	return small_bucket_for_size<Bucket-1>(size);
}
/**
 * Base case specialisation.
 */
template<>
constexpr int small_bucket_for_size<0>(size_t size)
{
	return (size <= BucketSize<0>::value) ? 0 : -1;
}

/**
 * Returns the large bucket that corresponds to a specific size.  Large buckets
 * are allocated in a multiple of the page size, starting at 32KiB.
 */
constexpr int large_bucket_for_size(size_t sz)
{
	ASSERT(sz >= 32_KiB);
	ASSERT(sz <= chunk_size / 4);
	// Round up the requested size to the nearest multiple of the page size,
	// then subtract a constant such that large bucket 0 is 32KiB.
	return ((sz + page_size-1) / page_size) - (32_KiB / page_size);
}
/**
 * Returns the number of the largest large bucket.  Above this size, huge
 * allocators manage memory provided directly by the operating system's page
 * allocator.
 */
constexpr int largest_large_bucket()
{
	return large_bucket_for_size(chunk_size / 4);
}

static_assert(large_bucket_for_size(32_KiB) == 0,
		"Large buckets start in the wrong place!");
static_assert(large_bucket_for_size(32_KiB + page_size + 1) == 2,
		"Large don't round correctly!");

/**
 * Function that maps from size to bucket.  Uses `bucket_for_size<>` for medium
 * buckets.
 */
__attribute__((always_inline))
constexpr int bucket_for_size(size_t sz)
{
	// FIXME: The optimisations for SuperMalloc are not quite right here, so
	// they're gone for now, but this is a 
	if (sz < 32_KiB)
	{
		return small_bucket_for_size<largest_medium_bucket()>(sz);
	}
	if (sz < (chunk_size / 4))
	{
		return large_bucket_for_size(sz) + largest_medium_bucket();
	}
	// Not a fixed-sized bucket at all
	return -1;
}

/**
 * The number of fixed-size buckets to use.
 */
static const int fixed_buckets = largest_medium_bucket() + largest_large_bucket();


}
