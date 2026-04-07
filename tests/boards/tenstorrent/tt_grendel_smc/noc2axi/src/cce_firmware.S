.global _start

_start:
	mv a0, zero
	mv a1, zero
	li a0, 0x0300004C
	li a1, 0xdeadbeef
	sw a1, 0x0(a0)
	# Write 0xcafebabe to CCE SRAM at 0x1000
	li a0, 0x401000
	li a1, 0xcafebabe
	sw a1, 0x0(a0)
_spin:
	j _spin
