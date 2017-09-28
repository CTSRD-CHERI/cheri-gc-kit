SDK?=~/sdk
INSTALL_DIR?=.

CXXFLAGS=-O1 -g -funwind-tables -Werror -std=gnu++14 -mabi=purecap -fno-exceptions -fno-rtti  -msoft-float -fconstexpr-depth=4096 -ferror-limit=3

all: gctest.dump slab_test.dump mark_and_sweep_test.dump

gctest.dump: ${INSTALL_DIR}/gctest
	${SDK}/bin/llvm-objdump -source -disassemble ${INSTALL_DIR}/gctest > gctest.dump

mark_and_sweep_test.dump: ${INSTALL_DIR}/mark_and_sweep_test
	${SDK}/bin/llvm-objdump -source -disassemble ${INSTALL_DIR}/mark_and_sweep_test > mark_and_sweep_test.dump

slab_test.dump: ${INSTALL_DIR}/slab_test
	${SDK}/bin/llvm-objdump -source -disassemble ${INSTALL_DIR}/slab_test > slab_test.dump

${INSTALL_DIR}/gctest: test.o clean_regs.s
	${SDK}/bin/clang test.o -lpthread -o ${INSTALL_DIR}/gctest -static -mabi=purecap clean_regs.s -lc

${INSTALL_DIR}/mark_and_sweep_test: mark_and_sweep_test.o clean_regs.s
	${SDK}/bin/clang mark_and_sweep_test.o -lpthread -o ${INSTALL_DIR}/mark_and_sweep_test -static -mabi=purecap clean_regs.s -lc

${INSTALL_DIR}/slab_test: slab_test.cc slab_allocator.hh config.hh page.hh cheri.hh bucket_size.hh utils.hh
	time ${SDK}/bin/clang ${CXXFLAGS} slab_test.cc  -lpthread -o ${INSTALL_DIR}/slab_test -static -mabi=purecap -lc

test.o: test.cc BitSet.hh bump_the_pointer_heap.hh bump_the_pointer_or_large.hh cheri.hh config.hh counter.hh lock.hh mark_and_compact.hh nonstd_function.hh page.hh roots.hh utils.hh mark.hh
	${SDK}/bin/clang++ -c ${CXXFLAGS} test.cc

mark_and_sweep_test.o: test.cc BitSet.hh bump_the_pointer_heap.hh bump_the_pointer_or_large.hh cheri.hh config.hh counter.hh lock.hh mark_and_compact.hh nonstd_function.hh page.hh roots.hh utils.hh mark.hh bucket_size.hh mark_and_sweep.hh slab_allocator.hh
	${SDK}/bin/clang++ -c ${CXXFLAGS} mark_and_sweep_test.cc


clean:
	rm -f test.o gctest.dump
