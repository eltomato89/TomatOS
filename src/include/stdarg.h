/* TomatOS - variable argument lists
*  Desc: What printf() and its relatives stand on.
*
*  This replaces the DJGPP header the project shipped with in 2011, which
*  reached into src/include/sys/ for its va_list typedef and carried three
*  decades of compatibility machinery for compilers that are not this one.
*  The whole sys/ directory existed for that single include and is gone with
*  it.
*
*  Nothing here is hand written arithmetic over a pointer to the last named
*  argument, which is what the old header did: on a target where an argument
*  does not simply follow its predecessor on the stack, that is wrong, and
*  more immediately it makes the compiler's own knowledge of the frame
*  unavailable. GCC provides the builtins for exactly this reason and knows
*  where its arguments went; freestanding or not, they are always there.
*/
#ifndef __TOMATOS_STDARG_H
#define __TOMATOS_STDARG_H

typedef __builtin_va_list va_list;

#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_end(ap)          __builtin_va_end(ap)
#define va_copy(dst, src)   __builtin_va_copy(dst, src)

#endif
