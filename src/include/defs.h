#ifndef __TL__KRNL_H
#define	__TL__KRNL_H

#ifdef __cplusplus
extern "C"
{
#endif

/* registers pushed on the stack by exceptions or interrupts that
switch from user privilege to kernel privilege
	*** WARNING ***
The layout of this struct must agree with the order in which
registers are pushed and popped in the assembly-language
interrupt handler code. */
typedef struct
{
	unsigned edi, esi, ebp, esp, ebx, edx, ecx, eax; /* PUSHA/POP */
	unsigned ds, es, fs, gs;
	unsigned which_int, err_code;
	unsigned eip, cs, eflags, user_esp, user_ss; /* INT nn/IRET */
	unsigned v_es, v_ds, v_fs, v_gs; /* V86 mode only */
} uregs_t;

#ifdef __cplusplus
}
#endif

#endif
