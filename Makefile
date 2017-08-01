SDK?=~/sdk
INSTALL_DIR?=.

all: gctest.dump

gctest.dump: ${INSTALL_DIR}/gctest
	${SDK}/bin/llvm-objdump -source -disassemble /exports/users/dc552/cheriroot/tmp/gctest > gctest.dump

${INSTALL_DIR}/gctest: test.o clean_regs.s
	${SDK}/bin/clang test.o -lpthread -o ${INSTALL_DIR}/gctest -static -mabi=purecap clean_regs.s

test.o: test.cc BitSet.hh bump_the_pointer_heap.hh bump_the_pointer_or_large.hh cheri.hh config.hh counter.hh lock.hh mark_and_compact.hh nonstd_function.hh page.hh roots.hh utils.hh
	${SDK}/bin/clang++ -O1 -g -funwind-tables -Werror -std=gnu++14 test.cc -mabi=purecap -fno-exceptions -fno-rtti  -msoft-float -c 
