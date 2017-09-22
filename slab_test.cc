#include "slab_allocator.hh"

struct header { int x; };
slab_allocator<header> a;
slab_allocator<void> b;

int main(void)
{
	void *x = a.alloc(42);
	header *h;
	void *y = a.object_for_allocation(x, h);
	assert(cheri::base(y) == cheri::base(x));
	assert(cheri::length(h) == sizeof(*h));
	for (auto &alloc : a)
	{
		assert(cheri::base(alloc.first) == cheri::base(x));
		assert(cheri::length(alloc.second) == sizeof(*h));
		assert(alloc.second == h);
	}
	void *p;
	x = b.alloc(42);
	y = b.object_for_allocation(x, p);
	assert(cheri::base(y) == cheri::base(x));
	assert(p == nullptr);
	for (auto &alloc : b)
	{
		assert(cheri::base(alloc.first) == cheri::base(x));
		assert(cheri::length(alloc.second) == sizeof(*h));
		assert(alloc.second == nullptr);
	}
}
