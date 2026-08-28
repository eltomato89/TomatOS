/* TomatOS - Loading programs
*  Desc: Turns a bootloader module into a running ring 3 task.
*
*  There is no filesystem yet, so programs arrive as multiboot modules: GRUB
*  loads them alongside the kernel, one per "module" line in grub.cfg, and
*  hands over their physical addresses in the info structure. The kernel
*  records them at boot and can later load one into a fresh address space.
*
*  Loading means: parse the ELF32 headers, allocate a frame per page each
*  PT_LOAD segment needs, copy the file contents in, zero the part that is
*  .bss (p_memsz beyond p_filesz), and map it into the task's own address
*  space with PAGE_USER. Nothing is shared with the kernel or with another
*  program except the kernel quarter every space carries anyway.
*/
#ifndef __EXEC_H
#define __EXEC_H

#include "typedefs.h"
#include "multiboot.h"

#define EXEC_MAX_MODULES 8

/* Records the modules the bootloader passed. Call once, from kernel(), with
*  the already converted (virtual) info pointer. Safe to call with no
*  modules present -- exec_module_count() is then simply 0. */
extern void exec_init(multiboot_info *mbi);

extern int exec_module_count(void);

/* Name of a module, taken from its command line, or "?" if it has none.
*  Valid for 0 <= index < exec_module_count(). */
extern const char *exec_module_name(int index);

/* Size of a module in bytes. */
extern uint32_t exec_module_size(int index);

/* Index of the module with that name, or -1. */
extern int exec_module_find(const char *name);

/* Loads a module as a new ring 3 task and returns its pid, or a negative
*  value on failure. The task is created suspended; the caller starts it
*  with taskmgr_task_start(), which leaves room to inspect it first.
*
*  Everything the program needs is placed in its own address space before it
*  ever runs -- which is the whole reason this exists as a separate step
*  from taskmgr_add_user_task(). */
extern int exec_spawn(int index, int prio);

/* Why the last exec_spawn() failed, for the shell to print. */
extern const char *exec_last_error(void);

#endif
