@echo linking ..
@cd bin
@ld -T link.ld -o z.bin a.o b.o c.o d.o e.o f.o g.o h.o i.o j.o k.o l.o m.o n.o o.o z.o
@cd ..