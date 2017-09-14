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
#include <type_traits>

namespace cheri
{

#define __CHERI_INLINE inline __attribute__((__visibility__("hidden"), __always_inline__))
/**
 * Type for a virtual address.
 */
using vaddr_t = size_t;

/**
 * Returns true if the pointer is valid.
 */
template<typename T>
__CHERI_INLINE
bool is_valid(T *ptr)
{
	return __builtin_cheri_tag_get(static_cast<void*>(ptr));
}

/**
 * Returns the length of the capability, in bytes.
 */
template<typename T>
__CHERI_INLINE
vaddr_t length(T *ptr)
{
	return __builtin_cheri_length_get(static_cast<void*>(ptr));
}

/**
 * Returns the base of the capability.
 *
 * Note: In an environment with a copying garbage collector, this value is
 * not guaranteed to be stable!
 */
template<typename T>
__CHERI_INLINE
vaddr_t base(T *ptr)
{
	return __builtin_cheri_base_get(static_cast<void*>(ptr));
}

/**
 * Sets the offset in a capability.
 */
template<typename T>
__CHERI_INLINE
T* set_offset(T *ptr, vaddr_t offset)
{
	return static_cast<T*>(__builtin_cheri_offset_set(static_cast<void*>(ptr), offset));
}
/**
 * The `cheri::capability` class encapsulates a CHERI capability, providing
 * methods for accessing the properties of the capability.
 */
template<typename T>
class capability
{
	/**
	 * Helper that gives the size of the pointee, or 1 for void pointers.
	 */
	static const size_t object_size = sizeof(typename std::conditional<std::is_void<T>::value, char, T>::type);
	/**
	 * Pointer value.
	 */
	T *__capability ptr;
	/**
	 * Helper function.  All of the CHERI builtins expect a void*, so cast the
	 * stored value to this type.
	 */
	__CHERI_INLINE void *__capability get_ptr() const
	{
		return static_cast<void *__capability>(ptr);
	}
	/**
	 * Helper function.  All of the CHERI builtins that modify a capability
	 * return a void*, so cast the result to the value type.
	 */
	__CHERI_INLINE void set(void *__capability v)
	{
		ptr = static_cast<T *__capability>(v);
	}
	public:
	/**
	 * Valid permissions on capabilities.
	 */
	enum permission
	{
		permit_global = __CHERI_CAP_PERMISSION_GLOBAL__,
		permit_execute = __CHERI_CAP_PERMISSION_PERMIT_EXECUTE__,
		permit_load_capability = __CHERI_CAP_PERMISSION_PERMIT_LOAD_CAPABILITY__,
		permit_load = __CHERI_CAP_PERMISSION_PERMIT_LOAD__,
		permit_seal = __CHERI_CAP_PERMISSION_PERMIT_SEAL__,
		permit_store_capability = __CHERI_CAP_PERMISSION_PERMIT_STORE_CAPABILITY__,
		permit_store_local = __CHERI_CAP_PERMISSION_PERMIT_STORE_LOCAL__,
		permit_store = __CHERI_CAP_PERMISSION_PERMIT_STORE__
	};
	/**
	 * Capability object types.  
	 */
	using otype = long long;
	/**
	 * Constant for an invalid object type.
	 */
	static const otype invalid_otype = -1;
	/**
	 * The maximum valid object type.
	 */
	static const otype otype_max = sizeof(void*) == 32 ? ((1<<24) - 1) : ((1<<12) - 1);
	/**
	 * Returns the default data capability, as a capability to the specified
	 * type.
	 */
	__CHERI_INLINE
	static capability<T> default_data_capability()
	{
		return static_cast<T*>(__builtin_cheri_global_data_get());
	}
	/**
	 * Returns the program counter capability, as a capability to the specified
	 * type.
	 */
	__CHERI_INLINE
	static capability<T> program_counter_capability()
	{
		return static_cast<T*>(__builtin_cheri_program_counter_get());
	}
	/**
	 * Construct a capability object from a pointer.
	 */
	__CHERI_INLINE
	capability(T *p) : ptr(p) {}
	/**
	 * Default constructor.
	 */
	__CHERI_INLINE
	capability() : ptr(nullptr) {}
	/**
	 * Returns the size of the memory range, in multiples of the object size.
	 */
	__CHERI_INLINE
	size_t size() const
	{
		return __builtin_cheri_length_get(get_ptr()) / object_size;
	}
	/**
	 * Returns the size of the memory range, in bytes.
	 */
	__CHERI_INLINE
	size_t length() const
	{
		return __builtin_cheri_length_get(get_ptr());
	}
	/**
	 * Returns the base virtual address.
	 *
	 * Note: In an environment with a copying garbage collector, this value is
	 * not guaranteed to be stable!
	 */
	__CHERI_INLINE
	vaddr_t base() const
	{
		return __builtin_cheri_base_get(get_ptr());
	}
	/**
	 * Returns the offset of the pointer from the base.
	 */
	__CHERI_INLINE
	size_t offset() const
	{
		return __builtin_cheri_offset_get(get_ptr());
	}
	/**
	 * Returns a bitmask of the permissions in this object.
	 */
	__CHERI_INLINE
	long permissions() const
	{
		return __builtin_cheri_perms_get(get_ptr());
	}
	/**
	 * Returns the object type of this object, or `invalid_otype` if the capability 
	 * is not sealed..
	 */
	__CHERI_INLINE
	otype type() const
	{
		return __builtin_cheri_type_get(get_ptr());
	}
	/**
	 * Returns true if the capability has the specified permission, false otherwise.
	 */
	bool has_permission(permission p)
	{
		return (permissions() & p) == p;
	}
	/**
	 * Returns true if this capability is sealed, false otherwise.
	 */
	bool is_sealed()
	{
		return __builtin_cheri_sealed_get(get_ptr());
	}
	/**
	 * Unseals the capability, given a sealing capability.
	 */
	template<typename K>
	__CHERI_INLINE
	bool unseal(capability<K> t)
	{
		// FIXME: If we have a sane implementation of __builtin_cheri_unseal
		// then we can avoid the need for all of this.
		if (!(*this && t && is_sealed() && !t.is_sealed() && 
		      !(t.offset() >= t.size()) && t.has_permission(permit_seal) &&
		      (t.base() + t.offset() != type())))
		{
			return false;
		}
		set(__builtin_cheri_unseal(get_ptr(), t.get_ptr()));
		return true;
	}
	/**
	 * Seals the capability, given a sealing capability.
	 */
	template<typename K>
	__CHERI_INLINE
	bool seal(capability<K> t)
	{
		// FIXME: If we have a sane implementation of __builtin_cheri_seal
		// then we can avoid the need for all of this.
		if (!(*this && t && is_sealed() && !t.is_sealed() && 
		      !(t.offset() >= t.size()) && t.has_permission(permit_seal) &&
		      (t.base() + t.offset() != type())))
		{
			return false;
		}
		set(__builtin_cheri_seal(get_ptr(), t.get_ptr()));
		return true;
	}
	/**
	 * Sets the bounds to be `l` times the size of the pointee type.
	 */
	__CHERI_INLINE
	void set_bounds(size_t l)
	{
		set(__builtin_cheri_bounds_set(get_ptr(), l * object_size));
	}
	/**
	 * Sets the offset to `l` (in bytes).
	 */
	__CHERI_INLINE
	void set_offset(size_t l)
	{
		set(__builtin_cheri_offset_set(get_ptr(), l));
	}
	/**
	 * Removes all permissions that are not specified in the mask `p`.
	 */
	__CHERI_INLINE
	void mask_permissions(long long p)
	{
		set(__builtin_cheri_perms_and(get_ptr(), p));
	}
	/**
	 * Removes a single specified permission.
	 */
	__CHERI_INLINE
	void remove_permission(permission p)
	{
		mask_permissions(~static_cast<long long>(p));
	}
	/**
	 * Conversion to bool.  True for valid capabilities, false for invalid
	 * ones.
	 */
	__CHERI_INLINE
	operator bool()
	{
		return __builtin_cheri_tag_get(get_ptr());
	}
	/**
	 * Negation operator.  The inverse of bool conversion.
	 */
	__CHERI_INLINE
	bool operator !()
	{
		return !__builtin_cheri_tag_get(get_ptr());
	}
	/**
	 * Conversion to bare pointer operator.
	 */
	__CHERI_INLINE
	operator T *__capability()
	{
		// FIXME: This won't work in the hybrid ABI.
		return ptr;
	}
	/**
	 * Addition operator, has the same semantics as pointer addition.
	 */
	__CHERI_INLINE
	capability<T> &operator +=(ptrdiff_t o)
	{
		ptr += o;
		return *this;
	}
	/**
	 * Subtraction operator, has the same semantics as pointer subtraction.
	 */
	__CHERI_INLINE
	capability<T> &operator -=(ptrdiff_t o)
	{
		ptr -= o;
		return *this;
	}
	/**
	 * Member access operator, has the same semantics as pointer member access.
	 */
	__CHERI_INLINE
	T *operator->() const
	{
		return ptr;
	}
	/**
	 * Dereference operator, has the same semantics as pointer dereference.
	 */
	__CHERI_INLINE
	typename std::add_lvalue_reference<T>::type operator*() const
	{
		return *ptr;
	}
	/**
	 * Begin: Provides pointer to the start of the capability that can be used
	 * for iteration.
	 */
	__CHERI_INLINE
	T * __capability begin()
	{
		return ptr;
	}
	/**
	 * End: Provides pointer to the end of the capability that can be used
	 * for iteration.
	 */
	__CHERI_INLINE
	T* __capability end()
	{
		return cheri::set_offset(ptr, length());
	}
	/**
	 * Returns the underlying pointer.
	 */
	__CHERI_INLINE
	T* __capability get()
	{
		return ptr;
	}
	/**
	 * Returns true if the virtual address `a` is within the bounds of this
	 * capability.
	 *
	 * Note: In an environment with a copying garbage collector, this value is
	 * not guaranteed to be stable!
	 */
	__CHERI_INLINE
	bool contains(vaddr_t a)
	{
		return (a >= base()) && (a < (base() + size()));
	}
	/**
	 * Returns true if the pointer `a` is within the bounds of this
	 * capability.
	 */
	template<typename P>
	__CHERI_INLINE
	bool contains(P *ptr)
	{
		return contains(capability<P>(ptr));
	}
	/**
	 * Returns true if the capability `cap` is within the bounds of this
	 * capability.
	 */
	template<typename P>
	__CHERI_INLINE
	bool contains(capability<P> cap)
	{
		// FIXME: Use test subset instruction
		// FIXME: This is not stable in the presence of copying GC, but it
		// could be if we used CLT / CGT.
		return (base() <= cap.base()) &&
		       ((base() + size() >= (cap.base() + cap.size()))) &&
		       ((permissions() & cap.permissions()) == cap.permissions());
	}
};

#undef __CHERI_INLINE
}
