#include <system.h>
#include <stdio.h>

long register_read(char* reg)
{
	long r;
	if(strcmp(reg, "eax"))
	{
		asm ( "mov %%eax, %0;" : "=r"(r)); // eax into r

	} else if(strcmp(reg, "ebx"))
	{
		asm ( "mov %%ebx, %0;" : "=r"(r));
	} else if(strcmp(reg, "ecx"))
	{
		asm ( "mov %%ecx, %0;" : "=r"(r));
	} else if(strcmp(reg, "edx"))
	{
		asm ( "mov %%edx, %0;" : "=r"(r));
	} else if(strcmp(reg, "esi"))
	{
		asm ( "mov %%esi, %0;" : "=r"(r));
	} else if(strcmp(reg, "edi"))
	{
		asm ( "mov %%edi, %0;" : "=r"(r));
	} else {
		printf("register_read() error");
		r= -1;
	}
	
	return r;
}

void register_write(char* reg, long value)
{
	if(strcmp(reg, "eax"))
	{
		asm ( "mov %0, %%eax; " : : "r"(value)); // a into eax

	} else if(strcmp(reg, "ebx"))
	{
		asm ( "mov %0, %%ebx; " : : "r"(value));
	} else if(strcmp(reg, "ecx"))
	{
		asm ( "mov %0, %%ecx; " : : "r"(value));
	} else if(strcmp(reg, "edx"))
	{
		asm ( "mov %0, %%edx; " : : "r"(value));
	} else if(strcmp(reg, "esi"))
	{
		asm ( "mov %0, %%esi; " : : "r"(value));
	} else if(strcmp(reg, "edi"))
	{
		asm ( "mov %0, %%edi; " : : "r"(value));
	} else {
		printf("register_write() error");
		
	}
}

void asm_test()
{
	/*
	long a=8, b=9;
	asm ( "mov %0, %%eax; " : : "r"(a)); // a into eax
	asm ( "mov %%eax, %0;" : "=r"(b)); // eax into r
	*/
	
	int x;
	asm ("mov %%eax, %0;" : "=r"(x));
	printf("eax: %i\n", x);
}
