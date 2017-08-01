        .text
	.abicalls
	.section        .mdebug.abi64,"",@progbits
	.text
	.globl  clear_regs              # -- Begin function clear_regs
	.p2align        3
	.type   clear_regs,@function
	.ent    clear_regs
clear_regs:                             # @clear_regs
	.set    noreorder
	.set    nomacro
	.set    noat
# BB#0:
	# No stack usage, simply zero all callee-save registers and return.
	cfromptr $c1, $c11, $zero
	cfromptr $c2, $c11, $zero
	cfromptr $c3, $c11, $zero
	cfromptr $c4, $c11, $zero
	cfromptr $c5, $c11, $zero
	cfromptr $c6, $c11, $zero
	cfromptr $c7, $c11, $zero
	cfromptr $c8, $c11, $zero
	cfromptr $c9, $c11, $zero
	cfromptr $c10, $c11, $zero
	cfromptr $c12, $c11, $zero
	cfromptr $c13, $c11, $zero
	cfromptr $c14, $c11, $zero
	cfromptr $c15, $c11, $zero
	cjr     $c17
	cfromptr $c16, $c11, $zero
	.set    at
	.set    macro
	.set    reorder
	.end    clear_regs
.Lfunc_end0:
	.size   clear_regs, .Lfunc_end0-clear_regs

