# TomatOS

Ein 32-Bit-x86-Hobbykernel (Multiboot 1) — eigener präemptiver Scheduler,
VGA-Textkonsole, PS/2-Tastatur mit deutschem Layout, PIT-Timer, CMOS-Uhr
und eine kleine Shell. Ursprünglich 2006–2011 entstanden, 2026 auf eine
aktuelle Linux-Toolchain portiert.

## Voraussetzungen

Eine Cross-Toolchain wird **nicht** gebraucht — der System-GCC erzeugt mit
`-m32` den nötigen 32-Bit-Code.

```sh
sudo pacman -S --needed nasm qemu-system-x86 grub libisoburn mtools
```

`nasm` und `qemu-system-x86` reichen zum Bauen und Testen. `grub` und
`libisoburn` werden nur für `make iso` gebraucht, `mtools` nur für
`make floppy`.

## Kompilieren

```sh
make
```

Ergebnis ist `build/kernel.elf`. Alle Zwischenprodukte landen in `build/`,
`make clean` räumt auf.

## In QEMU starten

```sh
make run
```

QEMU lädt den Multiboot-ELF direkt per `-kernel`, ganz ohne Bootloader —
das ist der schnellste Weg zum Ausprobieren. Beenden mit `Ctrl-C` im
Terminal oder `Strg-Alt-Q` im QEMU-Fenster.

An der Shell stehen `help`, `taskmgr`, `start`, `mem`, `reboot` und `exit` zur
Verfügung. `taskmgr` ohne Argument zeigt seine eigene Syntax.

### Tastaturlayout

Der Kernel bringt eine deutsche Keymap mit, deshalb startet `make run` QEMU
mit `-k de`. Ohne das übersetzt QEMU unter Wayland über das Keysym und legt
die US-Belegung zugrunde — das Minus käme dann als `ß` an. Für ein anderes
Layout:

```sh
make run QEMU_KEYMAP=en-us
```

## Alle Targets

| Befehl | Wirkung |
|---|---|
| `make` | Kernel nach `build/kernel.elf` bauen |
| `make run` | Direkt in QEMU starten (schnellster Testzyklus) |
| `make iso` | BIOS-bootfähiges `build/tomatos.iso` via GRUB 2 |
| `make run-iso` | Dieses ISO in QEMU booten |
| `make floppy` | Kernel in eine Kopie des GRUB-Legacy-Floppy-Images legen |
| `make run-floppy` | Dieses Floppy-Image in QEMU booten |
| `make debug` | QEMU angehalten starten, GDB-Stub auf Port 1234 |
| `make usb DEV=/dev/sdX` | ISO auf einen USB-Stick schreiben (fragt vorher nach) |
| `make clean` | `build/` löschen |
| `make help` | Diese Liste |

## Auf echter Hardware

`make iso` erzeugt ein Image, das per **Legacy-BIOS/CSM** bootet. Auf einen
Stick kommt es mit `make usb DEV=/dev/sdX` — das Ziel wird vorher angezeigt
und muss mit `yes` bestätigt werden. Vorsicht bei der Gerätewahl, `lsblk`
hilft beim Identifizieren.

Auf reinen UEFI-Geräten ohne CSM bleibt der Bildschirm schwarz: der Kernel
schreibt direkt in den VGA-Textpuffer bei `0xB8000`, den es dort nicht mehr
gibt. Dafür bräuchte es einen Framebuffer-Ausgabepfad.

## Debuggen

```sh
make debug          # Terminal 1 — QEMU wartet angehalten
gdb build/kernel.elf # Terminal 2
(gdb) target remote :1234
(gdb) break kernel
(gdb) continue
```

Da der Kernel als ELF gelinkt wird, stehen GDB alle Symbole zur Verfügung.

## Aufbau

```
src/            Kernelquellen
src/include/    eigene Header (src/include/sys/ ist ungenutzter DJGPP-Rest)
linker.ld       Linkerskript, lädt bei 1 MiB
bin/            das originale GRUB-Legacy-Floppy-Image (Vorlage, wird nie verändert)
legacy/         die alte Windows-Toolchain von 2011 (DJGPP, VFD) und ihre Batchdateien
```

Die Quellen sind UTF-8 mit LF. Gebaut wird mit `-std=gnu89`; unter neueren
Standards werden die impliziten Deklarationen des Originalcodes zu Fehlern.
