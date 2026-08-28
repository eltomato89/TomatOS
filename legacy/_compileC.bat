@echo Compiling C

@echo kernel.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/b.o ./src/kernel.c
@echo scrn.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/c.o ./src/scrn.c
@echo gdt.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/d.o ./src/gdt.c
@echo idt.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/e.o ./src/idt.c
@echo isrs.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/f.o ./src/isrs.c
@echo irq.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/g.o ./src/irq.c
@echo hardware.c
@gcc -o -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/h.o ./src/hardware.c
@echo timer.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/i.o ./src/timer.c
@echo kb.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/j.o ./src/kb.c
@echo main.c
@gcc -O -masm=intel -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/k.o ./src/main.c
@echo str.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/l.o ./src/str.c
@echo math.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/m.o ./src/math.c
@echo tasks.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/n.o ./src/tasks.c
@echo asm.c
@gcc -O -fomit-frame-pointer -finline-functions -nostdinc -fno-builtin -I./src/include -c -o ./bin/o.o ./src/asm.c