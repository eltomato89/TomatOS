@echo Cleaning up
@cd bin
@DEL kernel.bin
@RENAME z.bin kernel.bin

@echo Cleaning up object files...
@del *.o
@cd ..