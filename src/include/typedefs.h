/* Prevent multiple inclusion: mm.h pulls typedefs.h in directly and a second
*  time via multiboot.h. */
#ifndef __TYPEDEFS_H
#define __TYPEDEFS_H

typedef int size_t;
typedef char                sbyte;
typedef unsigned char       ubyte;

typedef char                int8;
typedef unsigned char       card8;
typedef short               int16;
typedef unsigned short      card16;
typedef int                 int32;
typedef unsigned int        card32;
typedef long long           int64;
typedef unsigned long long  card64;
//typedef unsigned char byte;
typedef unsigned char uchar;
typedef unsigned short ushort;

typedef enum {false,true} bool;

typedef void (*fp)(void);
typedef unsigned char uint8_t;
/* Same definition as in stdint.h, which not every header pulls in. Without
*  it here, multiboot.h and elf.h only compile when stdint.h happens to have
*  been included first -- an ordering dependency nothing enforces. */
typedef unsigned short uint16_t;
typedef unsigned long int uint32_t;
typedef signed char int8_t;
typedef signed short int16_t;
typedef signed long int int32_t;

#endif
