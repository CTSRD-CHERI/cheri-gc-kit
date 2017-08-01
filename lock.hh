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
#include <functional>
#include <atomic>
#include <thread>
#include "page.hh"

/**
 * Variable declared in FreeBSD libc that indicates whether the program is
 * multithreaded.  Set monotonically to non-zero when the first thread is
 * created.
 */
extern "C" int __isthreaded;

namespace {

/**
 * Call `f` with lock `m` held.  This wrapper is intended to be used with a
 * lambda.
 */
template<typename Mutex, typename T, typename... Args>
void run_locked(Mutex &m, T &&f, Args&&... args)
{
	std::lock_guard<Mutex> l(m);
	f(std::forward<Args>(args)...);
}

/**
 * Try to call `f` with `m` locked.  Returns immediately if `m` cannot be
 * locked without blocking.
 */
template<typename Mutex, typename F, typename... Args>
bool try_run_locked(Mutex &m, F &&f, Args&&... args)
{
	if (m.try_lock())
	{
		std::lock_guard<Mutex> l(m, std::adopt_lock);
		f(std::forward<Args>(args)...);
		return true;
	}
	return false;
}

extern "C" {
/**
 * Private FreeBSD pthreads function that allows a mutex to be created with a
 * custom allocator.  This is used for locks in the `malloc` implementation,
 * which cannot call `malloc` without hitting infinite recursion.
 */
int _pthread_mutex_init_calloc_cb(pthread_mutex_t *mutex,
                                  void *(calloc_cb)(size_t, size_t));
/**
 * Internal name for `pthread_mutex_lock`.  This ensures that we call the
 * pthread version and not some user code.
 */
int _pthread_mutex_lock(pthread_mutex_t *mutex);
/**
 * Internal name for `pthread_mutex_trylock`.  This ensures that we call the
 * pthread version and not some user code.
 */
int _pthread_mutex_trylock(pthread_mutex_t *mutex);
/**
 * Internal name for `pthread_mutex_unlock`.  This ensures that we call the
 * pthread version and not some user code.
 */
int _pthread_mutex_unlock(pthread_mutex_t *mutex);
}

/**
 * Mutex implementation that is compatible with the standard mutex but does not
 * rely on malloc working to be allocated.
 */
class Mutex
{
	/**
	 * The underlying mutex object.
	 */
	pthread_mutex_t mutex;
	public:
	/**
	 * Construct a mutex, given a calloc function.
	 */
	Mutex(void *(calloc_fn)(size_t, size_t))
	{
		_pthread_mutex_init_calloc_cb(&mutex, calloc_fn);
	}
	/**
	 * Lock the mutex.
	 *
	 * WARNING: The program must not transition from single-threaded to
	 * multithreaded while the lock is held.
	 */
	void lock()
	{
		if (__isthreaded)
		{
			_pthread_mutex_lock(&mutex);
		}
	}
	/**
	 * Try to lock the mutex.  Returns `true` if the mutex is locked, `false`
	 * otherwise.
	 *
	 * WARNING: The program must not transition from single-threaded to
	 * multithreaded while the lock is held.
	 */
	bool try_lock()
	{
		if (__isthreaded)
		{
			return _pthread_mutex_trylock(&mutex) == 0;
		}
		return true;
	}
	/**
	 * Unlock the mutex.  It is undefined to call this if the mutex is not
	 * already locked.
	 */
	void unlock()
	{
		if (__isthreaded)
		{
			_pthread_mutex_unlock(&mutex);
		}
	}
};


/**
 * A spinlock that's designed for uncontended use.  Only provides a `try_lock`
 * operation, not a `lock`.  This is intended to fail fast when it encounters
 * contention and allow other strategies to be used.
 *
 * This is intended to be used to protect very small critical segments,
 * reducing the chance that we'll be preempted in the middle.
 *
 * If available, this uses hardware lock elision.  This is expected to
 * primarily help the case where an interrupt is delivered in the middle of the
 * critical section, as it will discard the process in this thread and allow
 * the other thread to proceed.
 */
template<typename T=int>
class alignas(64) UncontendedSpinlock
{
#ifdef __ATOMIC_HLE_ACQUIRE
	static const std::memory_order acquire = std::memory_order_acquire | __ATOMIC_HLE_ACQUIRE;
	static const std::memory_order release = std::memory_order_release | __ATOMIC_HLE_RELEASE;
	void fail()
	{
		_mm_pause();
	}
#else
	static const std::memory_order acquire = std::memory_order_acquire;
	static const std::memory_order release = std::memory_order_release;
	void fail() {}
#endif
	/**
	 * Variable that holds the lock.
	 */
	std::atomic<T> l = {0};
	public:
	/**
	 * Try to lock the mutex.
	 */
	bool try_lock()
	{
		int v = l.exchange(static_cast<T>(1), acquire);
		if (v == 0)
		{
			return true;
		}
		fail();
		return false;
	}
	/**
	 * Lock the mutex.  Note: calling this is usually an error, because this
	 * mutex is expected to be used only when contention is rare.
	 */
	void lock()
	{
		while (!try_lock()) {}
	}
	/**
	 * Unlock the mutex.  It is undefined to call this when the mutex is not
	 * held.
	 */
	void unlock()
	{
		l.store(static_cast<T>(0), release);
	}
};

/**
 * Wrapper for per-CPU caches of `T`.  
 */
template<typename T, int Size, int CacheSize=cache_line_size>
class PerCPUCache
{
	/**
	 * Calculates the padding required to ensure that each `T` starts in a
	 * different cache line.  This is done to avoid cache contention.
	 *
	 * FIXME: We should probably ensure that we allocate an exact multiple of
	 * cache lines.
	 */
	static constexpr int padding()
	{
		return sizeof(T) > CacheSize ? 0 : CacheSize - sizeof(T);
	}
	/**
	 * Returns the current CPU.
	 *
	 * FIXME: Unimplemented
	 */
	static int get_cpu()
	{
		return -1;
	}
	/**
	 * Structure that adds padding to `T` to round it up to cache line size.
	 */
	template<typename Type, int Padding>
	struct Padded
	{
		/**
		 * The important value in this structure.
		 */
		Type value;
		/**
		 * Alignment padding.
		 */
		char padding[Padding];
	};
	/**
	 * Specialisation for no padding.  Not required if the compiler supports
	 * the GNU extension for zero-length arrays.
	 */
	template<typename Type>
	struct Padded<Type, 0>
	{
		Type value;
	};
	/**
	 * The array of padded structures.
	 */
	std::array<Padded<T, padding()>, Size> values;
};

/**
 * A wrapper providing an object and an associated lock.  The object is
 * inaccessible without the lock being held.
 */
template<typename T, typename MutexClass=Mutex>
class ProtectedGlobal : public PageAllocated<ProtectedGlobal<T,MutexClass>>
{
	/**
	 * The protected object.
	 */
	T val;
	/**
	 * The lock that guards the object.
	 */
	MutexClass lock;
	public:
	ProtectedGlobal() = delete;
	/**
	 * Construct the object and lock.
	 */
	template<typename... Args>
	ProtectedGlobal(void *(calloc_fn)(size_t, size_t), Args&&... args)
		: val(std::forward<Args>(args)...), lock(calloc_fn)
	{ }
	/**
	 * The only way of accessing the object: runs the passed callable with the
	 * object as an argument and with the lock held.
	 */
	template<typename Fn>
	void run_locked(Fn &&fn)
	{
		::run_locked(lock, fn, val);
	}
};

}
