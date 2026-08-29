#ifndef __TL_STDINT_H
#define	__TL_STDINT_H

#ifdef __cplusplus
extern "C"
{
#endif

typedef unsigned char		uint8_t;
typedef unsigned short		uint16_t;
typedef unsigned long		uint32_t;

/* The signed set. Added when the system call ABI needed a field that can be
*  negative and still has a width the ABI can promise -- "int" would do on
*  i386, but a fixed width is the point of these names. */
typedef signed char		int8_t;
typedef signed short		int16_t;
typedef signed long		int32_t;

#ifdef __cplusplus
}
#endif

#endif
