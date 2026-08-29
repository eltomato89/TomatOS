/* TomatOS - Loading programs
*  Desc: Turns a bootloader module or a file on disk into a running ring 3
*        task.
*
*  A program reaches the loader from one of two places. GRUB can load it
*  alongside the kernel, one per "module" line in grub.cfg, and hand over its
*  physical address in the info structure - that is what exec_init() records
*  at boot and what exec_spawn() runs. Or it simply lies on the mounted
*  volume, in which case exec_spawn_path() reads it off the disk. Both end in
*  the same loader; only the source of the bytes differs.
*
*  Loading means: parse the ELF32 headers, allocate a frame per page each
*  PT_LOAD segment needs, copy the file contents in, zero the part that is
*  .bss (p_memsz beyond p_filesz), and map it into the task's own address
*  space with PAGE_USER. Nothing is shared with the kernel or with another
*  program except the kernel quarter every space carries anyway.
*
*  On top of that the task gets its argument vector, built on its own user
*  stack rather than in kernel memory, in the layout a plain
*
*      void _start(int argc, char **argv)
*
*  finds at [esp+4] and [esp+8]. Every program is entered that way, whether
*  it was given arguments or not: without any, argc is 1 and argv[0] is the
*  program's name.
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

/* Loads the ELF file at path from the mounted volume as a new ring 3 task
*  and returns its pid, or a negative value on failure, with the reason in
*  exec_last_error().
*
*  Nothing about the file is trusted: it is a file on a disk, not a module the
*  bootloader vouched for, so it may be a text file, a truncated download or a
*  64-bit binary. Everything is checked before a single frame is allocated,
*  and a rejected file leaves neither a task nor an address space behind.
*
*  args is the rest of the command line as one string, or 0 when there is
*  none. It is split into words here - blanks and tabs separate, runs of them
*  count once, there is no quoting - and handed to the program as argv[1] and
*  up. argv[0] is always the last component of path. Arguments that would not
*  leave the program enough stack to run in are refused rather than
*  truncated, and the spawn fails.
*
*  Like exec_spawn(), the task comes back SUSPENDED. It runs when the caller
*  passes its pid to taskmgr_task_start(), which is what leaves room to look
*  at the task, or to take it back down, before it has executed anything. */
extern int exec_spawn_path(const char *path, const char *args, int prio);

/* Why the last exec_spawn() or exec_spawn_path() failed, for the shell to
*  print. */
extern const char *exec_last_error(void);

#endif
