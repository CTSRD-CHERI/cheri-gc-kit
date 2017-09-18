#include "slab_allocator.hh"

struct header { int x; };
slab_allocator<header> a;
slab_allocator<void> b;

int main(void)
{
	void *x = a.alloc(42);
	header *h;
	void *y = a.allocation_for_address(x, h);
	fprintf(stderr, "x: %#p\ny: %#p\nh: %#p\n", x, y, h);
	x = b.alloc(42);
	void *p;
	y = b.allocation_for_address(x, p);
	fprintf(stderr, "x: %#p\ny: %#p\np: %#p\n", x, y, p);
}
