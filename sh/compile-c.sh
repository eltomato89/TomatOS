#!/bin/sh

echo Compiling C Files

echo kernel.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/kernel.o ../src/kernel.c
echo scrn.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/scrn.o ../src/scrn.c
echo gdt.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/gdt.o ../src/gdt.c
echo idt.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/idt.o ../src/idt.c
echo isrs.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/isrs.o ../src/isrs.c
echo irq.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/irq.o ../src/irq.c
echo hardware.c
gcc -o -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/hardware.o ../src/hardware.c
echo timer.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/timer.o ../src/timer.c
echo kb.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/kb.o ../src/kb.c
echo main.c
gcc -O -masm=intel -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/main.o ../src/main.c
echo str.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/str.o ../src/str.c
echo math.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/math.o ../src/math.c
echo tasks.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/tasks.o ../src/tasks.c
echo asm.c
gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I../src/include -c -o ../bin/asm.o ../src/asm.c