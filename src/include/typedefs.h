/* Mehrfachinklusion verhindern: mm.h zieht typedefs.h direkt und ueber
*  multiboot.h ein zweites Mal herein. */
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
typedef unsigned long int uint32_t;

#endif
