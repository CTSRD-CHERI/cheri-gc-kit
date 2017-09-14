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

namespace 
{
/**
 * The number of valid non-zero bits that we expect to see in a virtual
 * address.
 */
constexpr const int address_space_size_bits = 48;
/**
 * Size of a cache line.  This doesn't have to be accurate (it will vary
 * between microarchitectures), it is used to define the padding size that we intend to use.
 */
constexpr const int cache_line_size = 64;
/**
 * The size of a chunk.  The chunk size should be a multiple of the platform's
 * superpage size.
 */
const int chunk_size = 2_MiB;
/**
 * The number of bytes in a page.  Note that 'page' here means the smallest
 * granularity at which we expect page table management operations to work, not
 * the optimal superpage size for TLB usage..
 */
const size_t page_size = 4_KiB;
/**
 * The base two logarithm of the size of a chunk.
 */
const int chunk_size_bits = log2<chunk_size>();
/**
 * The maximum number of cores that we support.
 */
const int max_cores = 128;
/**
 * The number of fixed-size buckets to use.
 */
static const int fixed_buckets = 100;
}
