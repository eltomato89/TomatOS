@echo Compiling ASM
@G:\tools\nasm\nasm -f aout -o ./bin/a.o ./src/start.asm
@gcc -c ./src/test.S -o ./bin/z.o