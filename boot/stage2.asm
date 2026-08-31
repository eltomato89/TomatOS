; TomatOS - stage 2 of the boot chain
;
; Stage 1 (the boot sector) has loaded STAGE2_SECTORS sectors starting at
; STAGE2_LBA to STAGE2_ORG and jumped here in real mode with the BIOS boot
; drive number in dl. From here on there is no size limit worth worrying
; about, so this is where all the real work happens.
;
; The job is to hand the kernel exactly what GRUB would hand it: eax =
; 0x2BADB002, ebx = a multiboot info structure, 32-bit protected mode, flat
; 4 GiB segments, interrupts off. The kernel does not care who booted it.
;
; ---------------------------------------------------------------------------
; Order of the steps, and why it is this order
; ---------------------------------------------------------------------------
;
;   0. Shift state    One byte, read before anything else can disturb it and
;                     long before it is needed. It is the user's only way to
;                     say "do not set a graphics mode" on a machine where the
;                     graphics mode is why they cannot see anything, so it has
;                     to be sampled while the key they held through POST is
;                     still down. See the block at s2_start.
;
;   1. A20            Everything else that touches memory above 1 MiB depends
;                     on it. If A20 is off, a write to 0x00100000 lands at
;                     0x00000000 instead -- on the IVT, on this code, on the
;                     stack. Doing anything before this is building on sand.
;
;   2. E820 map       A BIOS call, so it has to happen while we are still in
;                     real mode, and it is the one piece of information the
;                     kernel cannot recover for itself.
;
;   3. mem_lower /    Two more BIOS calls. Cheap, and they give the kernel a
;      mem_upper      fallback if E820 was not available.
;
;   4. Kernel load    Needs A20 (step 1) and unreal mode. Deliberately BEFORE
;      and modules    the graphics mode: disk reads are the longest and most
;                     failure-prone part of the boot, and we want to be able
;                     to see the progress messages while it happens. Once a
;                     VBE mode is set, int 10h teletype output goes nowhere.
;                     The modules follow the kernel rather than preceding it
;                     because a machine that cannot read its own disk should
;                     say so before it has spent time on the user programs.
;
;   5. Graphics mode  Last of the BIOS calls, for exactly that reason. It is
;                     also the only step that is allowed to fail without
;                     stopping the boot.
;
;   6. Protected mode Pure register and memory work, no BIOS left to need.
;
; Everything that prints therefore happens before step 5, so a hang is always
; locatable from the last line on the screen.
;
; ---------------------------------------------------------------------------
; NOTE ON THE SIZE BUDGET
; ---------------------------------------------------------------------------
; Stage 1 loads STAGE2_SECTORS sectors to STAGE2_ORG, so this file's assembled
; content must fit in 8 KiB -- and vbe.inc is %include'd into it, so the two
; share that budget. DISK_BUFFER sits directly above the 8 KiB, which means
; overrunning the budget would have stage 2 read disk sectors over its own
; tail. Both limits are asserted at the bottom of this file so the build
; breaks loudly instead of the boot corrupting itself at run time.

%include "layout.inc"

[BITS 16]
[ORG STAGE2_ORG]

; Selectors into the GDT built at the bottom of this file.
SEL_CODE32      equ 0x08
SEL_DATA32      equ 0x10

; Stack the kernel is entered on. The kernel switches to its own stack
; immediately, but multiboot does not promise a stack at all, so we leave a
; sane one well away from everything we built (0x00080000..0x00090000 is free
; low memory, above the disk buffer and below the EBDA).
PM_STACK        equ 0x00090000

; Text mode framebuffer, used to describe the screen when VBE is unavailable.
TEXT_FB_ADDR    equ 0x000B8000
TEXT_FB_PITCH   equ 160         ; 80 cells * 2 bytes
TEXT_FB_WIDTH   equ 80
TEXT_FB_HEIGHT  equ 25
TEXT_FB_BPP     equ 16          ; two bytes per character cell

; The BIOS keyboard shift-state byte, in the BIOS data area at 0040:0017.
; Bit 0 right shift, bit 1 left shift, bit 2 ctrl, bit 3 alt; the higher bits
; are the caps/num/scroll latches, which are NOT what we want -- a latch is a
; toggle somebody may have left on months ago, and this has to mean "held down
; right now".
;
; BIOS_KBD_FLAGS itself is in layout.inc with the other fixed low-memory
; addresses. Only the mask is here, because only this file decides which keys
; mean "text mode".
KBD_SHIFT_MASK  equ 0x03        ; either shift key, see s2_start

; ---------------------------------------------------------------------------
; Entry point and the patchable header
; ---------------------------------------------------------------------------
; Stage 1 jumps to offset 0, so byte 0 has to be executable code. Immediately
; behind it sits a small header the Makefile patches after it knows how big
; the kernel image actually is -- see s2_kernel_sectors.

s2_entry:
        jmp     s2_start
        times   4-($-$$) db 0           ; header starts at file offset 4

; Magic so the Makefile (or a human with hexdump) can assert it is patching
; the right bytes of the right file.
s2_magic:
        db      'TOMATOS2'              ; file offset 4 .. 11

; Size of the kernel image in 512-byte sectors, little endian, at file offset
; 12 of stage2.bin. The Makefile writes the real value here; the built-in
; default is the worst case so that an unpatched image still boots (it just
; reads more than it needs). Clamped at run time to KERNEL_MAX_SECTORS.
s2_kernel_sectors:
        dd      KERNEL_MAX_SECTORS      ; file offset 12 .. 15

; How many multiboot modules follow, and where each of them lives on the disk.
; The Makefile patches both once it knows which programs were built and where
; it wrote them; layout.inc states the offsets so that both sides can code to
; the same numbers without either having to read the other.
;
; The default is a count of zero, which is the same courtesy s2_kernel_sectors
; extends to an unpatched image: stage 2 then hands the kernel no modules at
; all and boots exactly as it did before any of this existed. Zero is also the
; honest answer for a disk built without programs, so the "unpatched" and the
; "nothing to load" cases need no distinction.
;
; The table is padded out to MB_MODS_MAX entries whether they are used or not.
; A fixed-size table means the Makefile can seek straight to entry n and the
; layout does not depend on how many programs happen to exist, at a cost of
; MB_MODS_MAX * S2MOD_ENTRY_SIZE bytes -- 384 of the 8 KiB budget, which is
; cheap next to having two build steps disagree about where entry 3 begins.
        times   S2HDR_MODCOUNT_OFF-($-$$) db 0
s2_module_count:
        dd      0                       ; file offset 16 .. 19

        times   S2HDR_MODTAB_OFF-($-$$) db 0
s2_module_table:
        times   MB_MODS_MAX * S2MOD_ENTRY_SIZE db 0     ; file offset 20 ..

; ---------------------------------------------------------------------------
s2_start:
        cli
        ; Do not trust any segment register stage 1 left behind. Everything in
        ; this file addresses low memory as a flat 0-based space, so all of the
        ; real mode segments are zero and the org above supplies the offset.
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     sp, STAGE2_STACK
        cld
        sti

        ; dl is the only thing stage 1 promised us. Save it before the first
        ; BIOS call gets a chance to eat it.
        mov     [s2_boot_drive], dl

        ; --- 0. the escape hatch --------------------------------------------
        ;
        ; THIS IS A RECOVERY PATH FOR A USER WHO CAN SEE NOTHING AT ALL, and
        ; every part of its design follows from that one sentence.
        ;
        ; The failure it answers: vbe.inc picks the best mode the CARD offers,
        ; and on a laptop the card can offer modes the PANEL cannot display.
        ; When that happens nothing reports an error -- 4F02h returns success,
        ; the kernel boots, the console draws, the machine is running
        ; perfectly -- and the screen is black. vbe.inc now reads the EDID to
        ; avoid choosing such a mode, but the EDID is exactly the thing a
        ; machine in this state is most likely to be lying about or not
        ; providing, so there has to be a way out that does not depend on it.
        ;
        ; It must therefore work with NO FEEDBACK WHATSOEVER: the user cannot
        ; see a menu, cannot see a countdown, cannot see whether the keypress
        ; registered, and on a machine that boots in two seconds cannot react
        ; to anything either. That rules out every interactive scheme and
        ; leaves one: a key already HELD DOWN before the machine is switched
        ; on, so it is down whenever we happen to look.
        ;
        ; 0040:0017 is where the BIOS keeps the live shift state, and it is
        ; readable in real mode with one instruction. It reflects the physical
        ; state of the keys right now, not something in the keystroke queue,
        ; which is what makes a held key work at all.
        ;
        ; WHY EITHER SHIFT KEY, and not ctrl, alt, a combination, or a letter:
        ;
        ;   * A MODIFIER produces no keystroke, so holding it does not fill
        ;     the BIOS keyboard buffer and does not produce the "stuck key"
        ;     beeping that holding a letter through POST gives on a ThinkPad.
        ;     Holding it for the whole of POST is genuinely free.
        ;   * SHIFT specifically, because it is the convention -- it is what
        ;     GRUB watches for to force its menu, so it is the key somebody
        ;     with a black screen is most likely to try unprompted.
        ;   * EITHER shift, because the user cannot see which one worked. One
        ;     key to describe, two keys that satisfy it.
        ;   * NOT ctrl or alt: on a ThinkPad both are involved in the BIOS's
        ;     own boot-time gestures, and neither is worth the ambiguity.
        ;   * NOT a two-key combination: it has to be holdable with one hand
        ;     while the other reaches the power button, in the dark, by
        ;     somebody already convinced the machine is broken.
        ;
        ; The asymmetry of being wrong is the whole argument for making it
        ; this easy to trigger. A false positive costs a boot in 80x25 text,
        ; which works and is obvious and is fixed by rebooting without the
        ; key. A false negative costs a black screen with no way out. So this
        ; is deliberately biased towards firing.
        ;
        ; READ HERE, at the very top, and not at step 5 where it is used.
        ; Between the two is the whole kernel and module load -- seconds of
        ; disk activity on a USB stick -- and nobody holds a key that long on
        ; purpose. Sampling early catches the state the user set up before
        ; pressing the power button, which is the only state they can set up
        ; at all.
        mov     al, [BIOS_KBD_FLAGS]
        and     al, KBD_SHIFT_MASK
        mov     [s2_safe_mode], al      ; non-zero = forced text mode

        mov     si, s2_msg_banner
        call    s2_print

        ; Say so, audibly. s2_msg_safe opens with a BEL, which int 10h's
        ; teletype call turns into a beep on the PC speaker -- the one channel
        ; that still works when the display does not, and the only
        ; confirmation available to the person this feature is for. The text
        ; after it is for the luckier case where the screen works.
        cmp     byte [s2_safe_mode], 0
        je      .not_safe
        mov     si, s2_msg_safe
        call    s2_print
.not_safe:

        call    s2_mbi_clear

        ; --- 1. A20 --------------------------------------------------------
        mov     si, s2_msg_a20
        call    s2_print
        call    s2_a20_enable
        jnc     .a20_ok
        mov     si, s2_msg_fail
        jmp     s2_die
.a20_ok:
        mov     si, s2_msg_ok
        call    s2_print

        ; --- 2. memory map -------------------------------------------------
        mov     si, s2_msg_e820
        call    s2_print
        call    s2_e820
        jnc     .e820_ok
        ; Not fatal: the kernel's pmm.c falls back to mem_lower/mem_upper when
        ; MB_FLAG_MMAP is clear. Say so and carry on.
        mov     si, s2_msg_skip
        call    s2_print
        jmp     .e820_done
.e820_ok:
        mov     si, s2_msg_ok
        call    s2_print
.e820_done:

        ; --- 3. mem_lower / mem_upper and the boot device -------------------
        call    s2_mem_sizes
        call    s2_boot_device

        ; --- 4. kernel image ------------------------------------------------
        mov     si, s2_msg_load
        call    s2_print
        call    s2_unreal_enter
        call    s2_load_kernel          ; halts on its own on a disk error
        mov     si, s2_msg_ok
        call    s2_print

        ; --- 4b. multiboot modules -------------------------------------------
        ; The user programs, if the Makefile patched any into the table above.
        ; "none" here is a perfectly good boot on a disk built without them,
        ; and a diagnosis on a disk that was supposed to have them.
        mov     si, s2_msg_mods
        call    s2_print
        call    s2_load_modules         ; halts on its own on a bad table
        mov     si, s2_msg_ok
        test    eax, eax
        jnz     .mods_print
        mov     si, s2_msg_skip
.mods_print:
        call    s2_print

        ; --- 5. graphics mode ------------------------------------------------
        ; Last thing that prints, because a successful mode set turns the
        ; screen into a framebuffer and int 10h teletype output disappears.
        mov     si, s2_msg_vbe
        call    s2_print

        ; The escape hatch, sampled at the top of this file. Skipping the call
        ; entirely rather than letting vbe_setup run and undoing it afterwards
        ; is the point: a mode that has been SET cannot reliably be unset, and
        ; the screen we are trying to save is dark from the instant 4F02h
        ; returns. Not asking is the only way to be sure.
        cmp     byte [s2_safe_mode], 0
        jne     .vbe_forced_text

        call    vbe_setup               ; see the assumed contract below
        jc      .vbe_failed

        xor     ax, ax
        mov     ds, ax                  ; vbe.inc is allowed to clobber ds
        ; The framebuffer description was written by vbe.inc; we own the flag.
        or      dword [MB_INFO + MBI_FLAGS], MB_FLAG_FRAMEBUFFER
        jmp     .vbe_done

.vbe_failed:
        xor     ax, ax
        mov     ds, ax
        ; No VBE. Describe the ordinary EGA text screen instead, exactly the
        ; way GRUB does when it cannot give the kernel a graphics mode. The
        ; kernel then comes up in text mode rather than not at all.
        call    s2_fb_text
        mov     si, s2_msg_no
        call    s2_print
        jmp     .vbe_done

.vbe_forced_text:
        ; The same destination as a failed vbe_setup, reached on purpose
        ; rather than by accident -- which is exactly why this path is cheap
        ; to have. s2_fb_text hands the kernel an EGA text screen, the kernel
        ; sees MB_FB_TYPE_TEXT and keeps its console on 0xB8000, and nothing
        ; anywhere else has to know that a human asked for it.
        ;
        ; It reports a different word from the failure above, because the two
        ; are diagnosed differently: "text" means the machine has no usable
        ; VBE, "safe" means it has one and was told not to use it.
        call    s2_fb_text
        mov     si, s2_msg_safe_short
        call    s2_print
.vbe_done:

        ; --- 6. protected mode ----------------------------------------------
        jmp     s2_go_protected

; ---------------------------------------------------------------------------
; s2_print -- write a NUL terminated string at ds:si via int 10h teletype.
; Clobbers: nothing the caller can see.
; ---------------------------------------------------------------------------
s2_print:
        pusha
        mov     ah, 0x0E
        xor     bx, bx                  ; page 0, colour 0 in graphics modes
.loop:
        lodsb
        test    al, al
        jz      .done
        int     0x10
        jmp     .loop
.done:
        popa
        ret

; ---------------------------------------------------------------------------
; s2_die -- print ds:si and stop. Interrupts stay on so ctrl-alt-del still
; reboots the machine; there is nothing useful left to do here.
; ---------------------------------------------------------------------------
s2_die:
        call    s2_print
        mov     si, s2_msg_halt
        call    s2_print
.hang:
        hlt
        jmp     .hang

; ===========================================================================
; 1. A20
; ===========================================================================
;
; The 8086 had 20 address lines, so 0x100000 wrapped to 0x000000 and software
; came to depend on it. The AT kept the wrap as a gate that has to be opened
; explicitly. If we leave it shut, the kernel image we copy to 0x00100000
; lands on the IVT and on this very code instead, and nothing reports an
; error -- the boot just dies somewhere unrelated.
;
; There are three ways to open it and none of them is universally present, so
; we try all three. Crucially, we *test* after each one instead of believing
; the return code: INT 15h AX=2401h returning "success" on a machine where it
; does nothing at all is common enough that trusting it is a real bug source.
; ---------------------------------------------------------------------------

; s2_a20_enable -- returns CF clear if A20 is (now) enabled.
s2_a20_enable:
        ; Often it is already open, e.g. because the BIOS or a previous stage
        ; opened it. Testing first saves poking at the keyboard controller.
        call    s2_a20_test
        jnc     .done

        ; Method 1: the BIOS. The cleanest one when it exists.
        mov     ax, 0x2401
        int     0x15
        call    s2_a20_test
        jnc     .done

        ; Method 2: the keyboard controller's output port, bit 1. The original
        ; AT route, slow but nearly universal on real hardware.
        call    s2_a20_kbd
        call    s2_a20_test
        jnc     .done

        ; Method 3: the "fast A20" gate in the system control port. Bit 1 is
        ; A20; bit 0 is FAST RESET, so it must be masked off with great care --
        ; writing it back as 1 reboots the machine instead.
        in      al, 0x92
        test    al, 0x02
        jnz     .fast_done              ; already set, writing again is pointless
        or      al, 0x02
        and     al, 0xFE                ; never touch the fast reset bit
        out     0x92, al
.fast_done:
        call    s2_a20_test
.done:
        ret

; ---------------------------------------------------------------------------
; s2_a20_test -- the actual verification. CF clear = A20 works.
;
; 0000:0500 and FFFF:0510 are the same physical byte (0xFFFF*16 + 0x510 =
; 0x100500) whenever A20 is closed, and two different bytes when it is open.
; So: write different values through the two aliases and see whether the
; first one changed.
;
; It is done twice with different patterns. A single round can pass by
; accident if the high address happens to already hold the value we are
; comparing against -- for instance a leftover from an earlier probe. Two
; rounds whose expected values differ cannot both be fooled that way.
;
; 0x0500 is free memory per layout.inc, and 0x100500 is inside the region the
; kernel will later be loaded into -- which is fine, because this runs before
; the load.
; ---------------------------------------------------------------------------
s2_a20_test:
        push    ax
        push    ds
        push    es

        xor     ax, ax
        mov     ds, ax                  ; ds:0x0500 -> 0x00000500
        dec     ax
        mov     es, ax                  ; es:0x0510 -> 0x00100500 (or the alias)

        ; Round 1
        mov     byte [ds:0x0500], 0x00
        mov     byte [es:0x0510], 0xFF
        cmp     byte [ds:0x0500], 0xFF
        je      .wrapped

        ; Round 2, different values
        mov     byte [ds:0x0500], 0x55
        mov     byte [es:0x0510], 0xAA
        cmp     byte [ds:0x0500], 0xAA
        je      .wrapped

        clc
        jmp     .out
.wrapped:
        stc
.out:
        pop     es
        pop     ds
        pop     ax
        ret

; ---------------------------------------------------------------------------
; s2_a20_kbd -- the 8042 route. Every step has a bounded wait so a machine
; without a keyboard controller cannot hang the boot forever.
; ---------------------------------------------------------------------------
s2_a20_kbd:
        pushf
        cli                             ; a keystroke IRQ in the middle of this
                                        ; sequence would eat our status byte
        call    s2_kbd_wait_in
        mov     al, 0xAD                ; disable the keyboard
        out     0x64, al

        call    s2_kbd_wait_in
        mov     al, 0xD0                ; read output port
        out     0x64, al

        call    s2_kbd_wait_out
        in      al, 0x60
        mov     bl, al

        call    s2_kbd_wait_in
        mov     al, 0xD1                ; write output port
        out     0x64, al

        call    s2_kbd_wait_in
        mov     al, bl
        or      al, 0x02                ; bit 1 is A20
        out     0x60, al

        call    s2_kbd_wait_in
        mov     al, 0xAE                ; enable the keyboard again
        out     0x64, al

        call    s2_kbd_wait_in
        popf
        ret

; Wait for the input buffer to drain (status bit 1 clear), bounded.
s2_kbd_wait_in:
        push    ax
        push    cx
        mov     cx, 0xFFFF
.loop:
        in      al, 0x64
        test    al, 0x02
        jz      .done
        loop    .loop
.done:
        pop     cx
        pop     ax
        ret

; Wait for output data to be available (status bit 0 set), bounded.
s2_kbd_wait_out:
        push    cx
        mov     cx, 0xFFFF
.loop:
        in      al, 0x64
        test    al, 0x01
        jnz     .done
        loop    .loop
.done:
        pop     cx
        ret

; ===========================================================================
; 2. The E820 memory map
; ===========================================================================
;
; ENTRY FORMAT -- this is the part that is easy to get wrong.
;
; E820 hands back 20 bytes per entry:
;
;       +0  base   (64 bit)
;       +8  length (64 bit)
;       +16 type   (32 bit)
;
; A multiboot memory map entry is those same 20 bytes with a 32-bit `size`
; field glued on the FRONT:
;
;       +0  size   (32 bit)  <- counts the bytes AFTER itself, so 20
;       +4  base   (64 bit)
;       +12 length (64 bit)
;       +20 type   (32 bit)
;
; An entry is therefore 24 bytes long while its size field says 20, and a
; consumer walks the list with `addr + size + 4` -- which is exactly what
; src/mm/pmm.c does. Writing 24 into size, or leaving the size field out and
; packing 20-byte entries, both make pmm.c walk off into garbage.
;
; The trick that keeps this simple: point es:di at entry+4 for the BIOS call,
; so the BIOS drops its 20 bytes straight into their final place, and then
; write the constant 20 into entry+0 ourselves.
;
; ACPI 3.0 extended entries: some BIOSes return 24 bytes, the extra dword
; being an attribute word whose bit 0 means "this entry is valid". Those land
; at entry+24, i.e. on the next entry's size field, which we overwrite on the
; next iteration anyway -- the buffer just needs four bytes of slack at the
; end. We pre-seed that dword with 1 so a BIOS that returns only 20 bytes but
; claims 24 in ecx does not make us throw a perfectly good entry away.
; ---------------------------------------------------------------------------

; s2_e820 -- returns CF clear if a usable map was written.
s2_e820:
        pushad
        push    es

        xor     ax, ax
        mov     es, ax
        mov     di, MB_MMAP + 4         ; the BIOS writes at entry+4
        xor     ebx, ebx                ; continuation value, 0 = start over
        ; The entry counter lives in memory, not in a register. int 15h is
        ; documented to preserve everything but eax/ebx/ecx/edx, and in
        ; practice BIOSes have been caught trampling bp and si as well.
        mov     word [s2_mmap_count], 0

.next:
        ; Every call needs eax, ecx and edx set up again: the BIOS returns
        ; values in eax/ecx and is free to destroy edx.
        mov     eax, 0x0000E820
        mov     edx, 0x534D4150         ; 'SMAP'
        mov     ecx, 24                 ; ask for the ACPI 3.0 form
        mov     dword [es:di + 20], 1   ; default "valid" attribute, see above
        int     0x15
        jc      .finished               ; CF on the first call = unsupported,
                                        ; on a later one = that was the last
        cmp     eax, 0x534D4150         ; the BIOS echoes 'SMAP' back in eax
        jne     .finished

        ; A returned length below 20 is nonsense; skip such an entry.
        cmp     ecx, 20
        jb      .skip

        ; If the BIOS gave us the ACPI attribute word, honour bit 0: clear
        ; means "ignore this entry".
        cmp     ecx, 24
        jb      .no_attr
        test    byte [es:di + 20], 1
        jz      .skip
.no_attr:

        ; Zero-length regions are legal to return and useless to record.
        mov     eax, [es:di + 8]        ; length low
        or      eax, [es:di + 12]       ; length high
        jz      .skip

        ; Commit the entry: write the multiboot size field in front of it.
        ; 20, not 24 -- it counts the bytes that follow it, and the four bytes
        ; of the size field itself are what pmm.c's `addr + size + 4` adds
        ; back on.
        mov     dword [es:di - 4], 20
        inc     word [s2_mmap_count]
        add     di, 24

.skip:
        ; ebx == 0 means the BIOS just handed us the last entry.
        test    ebx, ebx
        jz      .finished
        cmp     word [s2_mmap_count], MB_MMAP_MAX
        jb      .next
        ; Out of room. Better a truncated map than a smashed multiboot info
        ; structure; the entries we have are still valid.

.finished:
        movzx   eax, word [s2_mmap_count]
        test    eax, eax
        jz      .none

        ; mmap_length is the total number of BYTES written -- all 24 of every
        ; entry, not the entry count and not entries * 20.
        imul    eax, eax, 24
        mov     [MB_INFO + MBI_MMAP_LENGTH], eax
        mov     dword [MB_INFO + MBI_MMAP_ADDR], MB_MMAP
        or      dword [MB_INFO + MBI_FLAGS], MB_FLAG_MMAP

        pop     es
        popad
        clc
        ret

.none:
        pop     es
        popad
        stc
        ret

; ===========================================================================
; 3. mem_lower / mem_upper, and the boot device
; ===========================================================================

; s2_mem_sizes -- fill mem_lower and mem_upper (both in KiB) and set the flag.
; This is the fallback the kernel uses when the E820 map is missing, so it is
; worth having even when E820 worked.
s2_mem_sizes:
        pusha

        ; mem_lower: conventional memory below 640 KiB, in KiB. int 12h is the
        ; oldest and most reliable way to ask.
        int     0x12                    ; -> ax = KiB
        jc      .lower_default
        test    ax, ax
        jz      .lower_default
        movzx   eax, ax
        jmp     .lower_store
.lower_default:
        mov     eax, 640
.lower_store:
        mov     [MB_INFO + MBI_MEM_LOWER], eax

        ; mem_upper: memory above 1 MiB, in KiB.
        ; AX=E801h returns two ranges because a single 16-bit KiB count cannot
        ; describe more than 64 MiB:
        ;   ax/cx = KiB in 1 MiB .. 16 MiB   (capped at 0x3C00 = 15 MiB)
        ;   bx/dx = 64 KiB blocks above 16 MiB
        ; Some BIOSes answer in ax/bx, some only in cx/dx, hence the fallback.
        xor     cx, cx
        xor     dx, dx
        mov     ax, 0xE801
        int     0x15
        jc      .try88
        cmp     ah, 0x86                ; unsupported function
        je      .try88
        cmp     ah, 0x80                ; invalid command
        je      .try88

        test    ax, ax
        jnz     .have_e801
        test    bx, bx
        jnz     .have_e801
        mov     ax, cx                  ; nothing in ax/bx, use cx/dx
        mov     bx, dx
        test    ax, ax
        jnz     .have_e801
        test    bx, bx
        jz      .try88

.have_e801:
        movzx   eax, ax                 ; KiB between 1 and 16 MiB
        movzx   ebx, bx                 ; 64 KiB blocks above 16 MiB
        shl     ebx, 6                  ; -> KiB
        add     eax, ebx
        jmp     .upper_store

.try88:
        ; AH=88h: ax = KiB above 1 MiB, saturating at 15 MiB (or 63 MiB on
        ; later BIOSes). Crude, but better than telling the kernel nothing.
        clc
        mov     ah, 0x88
        int     0x15
        jc      .upper_default
        test    ax, ax
        jz      .upper_default
        movzx   eax, ax
        jmp     .upper_store
.upper_default:
        mov     eax, 0                  ; honestly report "unknown" as zero

.upper_store:
        mov     [MB_INFO + MBI_MEM_UPPER], eax
        or      dword [MB_INFO + MBI_FLAGS], MB_FLAG_MEMORY
        popa
        ret

; s2_boot_device -- multiboot packs the boot device as
;   (drive << 24) | (part1 << 16) | (part2 << 8) | part3
; with 0xFF in every partition byte that does not apply. We booted the whole
; disk, not a partition, so all three are 0xFF.
s2_boot_device:
        push    eax
        movzx   eax, byte [s2_boot_drive]
        shl     eax, 24
        or      eax, 0x00FFFFFF
        mov     [MB_INFO + MBI_BOOT_DEVICE], eax
        or      dword [MB_INFO + MBI_FLAGS], MB_FLAG_BOOTDEV
        pop     eax
        ret

; s2_mbi_clear -- zero the multiboot info structure and the memory map area.
; Every field we do not fill in must read as zero, and the flags word must
; start empty so the bits we set are the only ones the kernel believes.
s2_mbi_clear:
        pusha
        push    es
        xor     ax, ax
        mov     es, ax
        mov     di, MB_INFO
        mov     cx, MBI_SIZE
        rep     stosb
        mov     di, MB_MMAP
        mov     cx, MB_MMAP_MAX * 24 + 8        ; + slack for an ACPI dword
        rep     stosb
        pop     es
        popa
        ret

; s2_fb_text -- describe the plain 80x25 EGA text screen in the framebuffer
; fields, for when no VBE mode could be set. GRUB reports text mode the same
; way, so the kernel needs no special case for "booted without graphics".
s2_fb_text:
        push    eax
        mov     dword [MB_INFO + MBI_FB_ADDR_LOW],  TEXT_FB_ADDR
        mov     dword [MB_INFO + MBI_FB_ADDR_HIGH], 0
        mov     dword [MB_INFO + MBI_FB_PITCH],     TEXT_FB_PITCH
        mov     dword [MB_INFO + MBI_FB_WIDTH],     TEXT_FB_WIDTH
        mov     dword [MB_INFO + MBI_FB_HEIGHT],    TEXT_FB_HEIGHT
        mov     byte  [MB_INFO + MBI_FB_BPP],       TEXT_FB_BPP
        mov     byte  [MB_INFO + MBI_FB_TYPE],      MB_FB_TYPE_TEXT
        or      dword [MB_INFO + MBI_FLAGS],        MB_FLAG_FRAMEBUFFER
        pop     eax
        ret

; ===========================================================================
; 4. Unreal mode and the kernel load
; ===========================================================================
;
; The kernel has to end up at physical 0x00100000, and real mode addressing
; tops out just above 1 MiB. The way out is "unreal mode", also called big
; real mode, and it works because of how the 386 caches segments.
;
; A segment register does not address memory by itself. Loading it fills a
; hidden descriptor cache with a base, a limit and access rights, and every
; memory reference goes through that cache -- never through the register
; value again. In real mode a segment load sets base = value * 16 and limit =
; 0xFFFF. In protected mode it sets base and limit from a GDT entry.
;
; Nothing reloads the cache when protected mode is switched back off. So:
;
;   1. turn protected mode on,
;   2. load fs from a descriptor with base 0 and a 4 GiB limit,
;   3. turn protected mode back off.
;
; Afterwards cs, ds, es, gs and ss are ordinary real mode segments -- the BIOS
; still works, int 13h still works, this code still runs unchanged -- but fs
; still carries the 4 GiB limit, so a 32-bit offset through it reaches
; anywhere in memory.
;
; ONLY fs, deliberately. The cached limit survives leaving protected mode, but
; it does NOT survive the segment register being written again, and in real
; mode such a write also recomputes the base as value * 16. fs holds the
; selector 0x10, so a stray `pop fs` would silently move the whole segment to
; base 0x100 and shrink it to 64 KiB -- writes would land 256 bytes off and
; anything past 64 KiB would fault. Nothing in the BIOS calling conventions
; touches fs, whereas es is an argument register for half of int 13h, so
; making es big too would be an accident waiting to happen.
;
; Interrupts must be off across the transition: for those few instructions
; the CPU is in protected mode while the IDT register still describes the
; BIOS's real mode vector table, and any interrupt would be fatal.
; ---------------------------------------------------------------------------

s2_unreal_enter:
        pusha
        push    ds
        cli

        ; o32 so the full 32-bit base is loaded. A plain 16-bit lgdt only
        ; takes 24 bits of base; ours is small enough either way, but relying
        ; on that is the kind of thing that breaks when the file moves.
        o32 lgdt [s2_gdtr]

        mov     eax, cr0
        or      al, 1
        mov     cr0, eax                ; protected mode, briefly

        ; No far jump needed: we are not changing cs, only loading a data
        ; segment, and that takes effect immediately.
        mov     bx, SEL_DATA32
        mov     fs, bx

        mov     eax, cr0
        and     al, 0xFE
        mov     cr0, eax                ; back to real mode, caches intact

        sti
        pop     ds
        popa

        ; Sanity check. If unreal mode silently did not take, or A20 dropped
        ; again behind our back, the write below goes somewhere other than
        ; 0x00100000 and the read back does not match. Cheap insurance before
        ; we spend the next few seconds copying a kernel into the void.
        ; (0x00100000 is the kernel's future home and still untouched.)
        push    eax
        push    edi
        mov     edi, KERNEL_PHYS
        mov     eax, 0xC0DE1234
        mov     [fs:edi], eax
        xor     eax, eax
        mov     eax, [fs:edi]
        cmp     eax, 0xC0DE1234
        pop     edi
        pop     eax
        je      .ok
        mov     si, s2_msg_unreal
        jmp     s2_die
.ok:
        ret

; ---------------------------------------------------------------------------
; s2_load_kernel -- read the kernel image from disk to KERNEL_PHYS.
;
; The BIOS can only read into memory it can address, i.e. below 1 MiB, so
; every chunk goes into DISK_BUFFER first and is then copied up through the
; 4 GiB fs segment established above.
; ---------------------------------------------------------------------------
s2_load_kernel:
        ; How much to read. The Makefile patches s2_kernel_sectors with the
        ; real image size; clamp it so a bad or unpatched value cannot make us
        ; run off the end of the reserved area into the filesystem.
        mov     eax, [s2_kernel_sectors]
        test    eax, eax
        jnz     .have_count
        mov     eax, KERNEL_MAX_SECTORS
.have_count:
        cmp     eax, KERNEL_MAX_SECTORS
        jbe     .count_ok
        mov     eax, KERNEL_MAX_SECTORS
.count_ok:
        mov     [s2_remaining], eax

        mov     dword [s2_lba], KERNEL_LBA
        mov     dword [s2_dest], KERNEL_PHYS

        call    s2_disk_probe
        call    s2_read_run
        ret

; ---------------------------------------------------------------------------
; s2_disk_probe -- decide once how this drive is going to be read.
;
; int 13h AH=41h asks whether the drive supports the packet interface (AH=42h,
; the one that takes an LBA outright). Everything the boot chain reads lives
; below LBA 1040, comfortably inside CHS range, so the fallback is not a
; formality: on a machine without the extensions the CHS path really does have
; to work, and it is exercised by every BIOS old enough to lack AH=42h.
;
; Cheap enough to call again before each load rather than making one caller
; responsible for having probed for the others; the answer cannot change.
; ---------------------------------------------------------------------------
s2_disk_probe:
        pusha
        mov     ah, 0x41
        mov     bx, 0x55AA
        mov     dl, [s2_boot_drive]
        int     0x13
        jc      .no_lba
        cmp     bx, 0xAA55
        jne     .no_lba
        test    cl, 1                   ; bit 0: packet access supported
        jz      .no_lba
        mov     byte [s2_use_lba], 1
        popa
        ret
.no_lba:
        mov     byte [s2_use_lba], 0
        call    s2_chs_geometry
        popa
        ret

; ---------------------------------------------------------------------------
; s2_read_run -- read [s2_remaining] sectors from [s2_lba] to [s2_dest],
; buffering each chunk through DISK_BUFFER. The caller sets those three and
; has already called s2_disk_probe.
;
; Split out of s2_load_kernel so the module loader can use it as it stands:
; the two differ only in where they read from and where they put it, and a
; second copy of the chunking arithmetic is a second place to get the
; sectors-to-bytes shift wrong.
; ---------------------------------------------------------------------------
s2_read_run:
        pushad
.loop:
        mov     eax, [s2_remaining]
        test    eax, eax
        jz      .done

        ; Chunk size: as much as fits in the buffer, or whatever is left.
        mov     ecx, DISK_BUFFER_SECTORS
        cmp     eax, ecx
        jae     .have_chunk
        mov     ecx, eax
.have_chunk:
        mov     [s2_chunk], cx

        call    s2_read_chunk           ; halts itself on a hard error
        call    s2_copy_chunk

        movzx   eax, word [s2_chunk]
        sub     [s2_remaining], eax
        add     [s2_lba], eax
        shl     eax, 9                  ; sectors -> bytes
        add     [s2_dest], eax
        jmp     .loop

.done:
        popad
        ret

; ---------------------------------------------------------------------------
; s2_read_chunk -- read [s2_chunk] sectors from [s2_lba] into DISK_BUFFER.
; Retries a few times (a reset plus another go is the classic cure for a
; spurious floppy/USB error) and dies loudly if it cannot.
; ---------------------------------------------------------------------------
s2_read_chunk:
        pushad
        mov     byte [s2_retries], 4
.attempt:
        cmp     byte [s2_use_lba], 0
        je      .chs

        ; --- LBA path: int 13h AH=42h with a disk address packet at ds:si.
        ; The packet is rebuilt from scratch on every attempt because the BIOS
        ; is allowed to write its own results back into it.
        mov     ax, [s2_chunk]
        mov     [s2_dap_count], ax
        mov     word [s2_dap_off], DISK_BUFFER
        mov     word [s2_dap_seg], 0
        mov     eax, [s2_lba]
        mov     [s2_dap_lba_lo], eax
        mov     dword [s2_dap_lba_hi], 0

        mov     si, s2_dap
        mov     ah, 0x42
        mov     dl, [s2_boot_drive]
        int     0x13
        jnc     .ok
        jmp     .retry

.chs:
        ; --- CHS path: one sector at a time. Reading several at once would
        ; have to stop at every track boundary; at 511 sectors total the extra
        ; calls cost nothing worth optimising away.
        ;
        ; AH=02h takes its buffer in es:bx, so es must be a plain zero real
        ; mode segment here. Set it explicitly rather than assuming: an
        ; earlier BIOS call is entitled to have left something else in it.
        xor     ax, ax
        mov     es, ax
        movzx   ecx, word [s2_chunk]
        mov     eax, [s2_lba]
        mov     bx, DISK_BUFFER
.chs_one:
        push    ecx
        push    eax
        call    s2_chs_read_one         ; eax = LBA, es:bx = target
        pop     eax
        pop     ecx
        jc      .retry
        inc     eax
        add     bx, 512
        dec     ecx
        jnz     .chs_one
        jmp     .ok

.retry:
        ; Reset the controller and try again.
        xor     ah, ah
        mov     dl, [s2_boot_drive]
        int     0x13
        dec     byte [s2_retries]
        jnz     .attempt
        mov     si, s2_msg_disk
        jmp     s2_die

.ok:
        popad
        ret

; s2_chs_geometry -- ask the BIOS for sectors-per-track and head count.
; Falls back to 63/16, a geometry that works for the images we build.
s2_chs_geometry:
        pusha
        push    es
        mov     ah, 0x08
        mov     dl, [s2_boot_drive]
        xor     di, di
        mov     es, di                  ; some BIOSes write through es:di
        int     0x13
        pop     es
        jc      .fallback
        and     cl, 0x3F                ; bits 0..5 are sectors per track
        jz      .fallback
        movzx   ax, cl
        mov     [s2_spt], ax
        movzx   ax, dh                  ; dh is the LAST head number
        inc     ax
        mov     [s2_heads], ax
        popa
        ret
.fallback:
        mov     word [s2_spt], 63
        mov     word [s2_heads], 16
        popa
        ret

; s2_chs_read_one -- read the single sector at LBA eax into es:bx.
;
;   sector = lba % spt + 1          (CHS sector numbers are 1-based)
;   head   = (lba / spt) % heads
;   cyl    = (lba / spt) / heads
;
; The intermediate results go to memory rather than being juggled in
; registers, because int 13h AH=02h wants cx and dx packed in an awkward way
; (cylinder split across ch and the top two bits of cl) and the division
; needs edx as the remainder register at the same time.
;
; Returns the BIOS's carry flag. Preserves everything else, es:bx included.
s2_chs_read_one:
        pushad

        xor     edx, edx
        movzx   ecx, word [s2_spt]
        div     ecx                     ; eax = track, edx = sector - 1
        mov     [s2_chs_sector], dl

        xor     edx, edx
        movzx   ecx, word [s2_heads]
        div     ecx                     ; eax = cylinder, edx = head
        mov     [s2_chs_head], dl
        mov     [s2_chs_cyl], ax

        mov     ax, [s2_chs_cyl]
        mov     ch, al                  ; cylinder bits 0..7
        mov     cl, ah
        shl     cl, 6                   ; cylinder bits 8..9 -> cl bits 6..7
        mov     al, [s2_chs_sector]
        inc     al                      ; 1-based
        and     al, 0x3F
        or      cl, al

        mov     dh, [s2_chs_head]
        mov     dl, [s2_boot_drive]
        mov     ax, 0x0201              ; AH=02h read, AL=1 sector
        int     0x13

        ; popad would restore the flags' worth of nothing but does not touch
        ; eflags itself, so the BIOS's carry survives to the caller.
        popad
        ret

; ---------------------------------------------------------------------------
; s2_copy_chunk -- move [s2_chunk] * 512 bytes from DISK_BUFFER up to
; [s2_dest] through the 4 GiB fs segment.
;
; This is an explicit dword loop rather than `a32 rep movsd` on purpose. movsd
; can only write through es, and es is exactly the segment a BIOS call is most
; likely to have quietly clobbered -- a single `mov es, ax` inside int 13h
; would reset its cached limit to 64 KiB and turn the first store into a
; fault. fs is not part of any BIOS calling convention, so it survives.
;
; ds keeps its ordinary real mode base of 0, so the source offsets (all below
; 0x10000) are within its 64 KiB limit even though esi is a 32-bit register.
; ---------------------------------------------------------------------------
s2_copy_chunk:
        pushad
        movzx   ecx, word [s2_chunk]
        shl     ecx, 7                  ; sectors * 512 / 4 = dwords
        mov     esi, DISK_BUFFER
        mov     edi, [s2_dest]
.loop:
        mov     eax, [esi]
        mov     [fs:edi], eax
        add     esi, 4
        add     edi, 4
        dec     ecx
        jnz     .loop
        popad
        ret

; ---------------------------------------------------------------------------
; s2_load_modules -- read the user programs off the disk and describe them to
; the kernel as multiboot modules. Returns the number loaded in eax.
;
; WHY STAGE 2 DOES THIS AT ALL. The kernel cannot read the medium it booted
; from on the machine this is aimed at: a USB stick is not an IDE or a SATA
; controller, so the ATA driver's port I/O does not reach it, and the USB mass
; storage driver talks UHCI, which every chipset since Panther Point replaced
; with EHCI and xHCI. The BIOS has no such trouble -- int 13h is how stage 1
; and stage 2 got into memory in the first place -- so the programs are read
; here, while int 13h is still callable, and handed over through the same
; mechanism GRUB uses. Without this the machine boots to a shell with no
; commands in it.
;
; WHAT IS BUILT. Three things, in low memory where the kernel's direct mapping
; can see them (layout.inc has the map):
;
;   MODULES_PHYS  the module images themselves, back to back, each one
;                 starting on a 4 KiB boundary
;   MB_MODS       one 16-byte multiboot_module per module: mod_start, mod_end
;                 (exclusive), cmdline, reserved
;   MB_MODSTR     the names, NUL terminated and packed, which the entries'
;                 cmdline fields point at
;
; The page alignment is not required by multiboot; it is a kindness to the
; other end. The kernel's ELF loader copies out of the module and its pmm
; reserves the frames a module covers, and both of those are simpler when a
; module neither begins nor ends in the middle of a frame that another module
; also lives in. It costs at most 4 KiB of RAM per program.
;
; WHY IT REFUSES RATHER THAN TRUNCATES. Every limit below is one that could be
; silently exceeded instead: read a sector too many and the filesystem's first
; FAT ends up inside a program, write a byte too many and the module lands on
; whatever the kernel put above MODULES_PHYS, drop a name that did not fit and
; the module keeps a cmdline pointing at the previous module's name. All three
; surface much later as a program that fails to parse as an ELF, or as a file
; that reads back as garbage -- symptoms with no path back to this routine. A
; named halt here costs a boot and explains itself.
; ---------------------------------------------------------------------------
s2_load_modules:
        mov     eax, [s2_module_count]
        test    eax, eax
        jz      .none                   ; nothing patched in: not a failure,
                                        ; and the flag stays clear
        cmp     eax, MB_MODS_MAX
        ja      .too_many
        mov     [s2_mod_left], eax

        ; Four cursors, all kept in memory rather than in registers: the loop
        ; body calls the BIOS by way of s2_read_run, and this file has already
        ; been bitten once (see s2_e820) by BIOSes that return with registers
        ; they had no business touching.
        mov     word  [s2_mod_entry], s2_module_table
        mov     word  [s2_mod_arr], MB_MODS
        mov     word  [s2_mod_str], MB_MODSTR
        mov     dword [s2_mod_dest], MODULES_PHYS

        call    s2_disk_probe

.next:
        mov     bx, [s2_mod_entry]

        ; --- how much of the disk this module claims ------------------------
        mov     eax, [bx + S2MOD_BYTES_OFF]
        mov     [s2_mod_bytes], eax
        add     eax, 511                ; a partial last sector is still read
        jc      .bad_disk               ; a length near 4 GiB is not a program
        shr     eax, 9
        mov     [s2_remaining], eax

        mov     edx, [bx + S2MOD_LBA_OFF]
        mov     [s2_lba], edx
        cmp     edx, MODULES_LBA        ; below the region is the kernel image
        jb      .bad_disk
        add     eax, edx                ; first sector behind this module
        jc      .bad_disk
        cmp     eax, MODULES_LBA + MODULES_MAX_SECTORS
        ja      .bad_disk               ; above it is the FAT16 volume

        ; --- and how much memory it needs -----------------------------------
        ; Against the rounded-up length, not the byte count: whole sectors are
        ; what s2_read_run actually writes, so those are what has to fit.
        mov     eax, [s2_remaining]
        shl     eax, 9
        add     eax, [s2_mod_dest]
        cmp     eax, MODULES_PHYS + MODULES_MAX_SECTORS * 512
        ja      .bad_mem

        ; --- the name, and room for it among the strings --------------------
        ; S2MOD_NAME_MAX bytes, NUL padded -- so a name that fills the field
        ; has no terminator in it and the length is the field width.
        lea     si, [bx + S2MOD_NAME_OFF]
        xor     cx, cx
.name_len:
        cmp     cx, S2MOD_NAME_MAX
        jae     .name_end
        cmp     byte [si], 0
        je      .name_end
        inc     si
        inc     cx
        jmp     .name_len
.name_end:
        sub     si, cx                  ; back to the first byte for the copy

        mov     di, [s2_mod_str]
        mov     [s2_mod_name], di       ; where this module's cmdline points
        mov     ax, di
        add     ax, cx
        inc     ax                      ; the NUL the kernel reads up to
        cmp     ax, MB_MODSTR + MB_MODSTR_MAX
        ja      .bad_name
        mov     [s2_mod_str], ax

        ; movsb writes through es, and int 13h's CHS path sets es for its own
        ; purposes, so it is loaded here rather than assumed.
        xor     ax, ax
        mov     es, ax
        rep     movsb
        mov     byte [es:di], 0

        ; --- the multiboot_module entry -------------------------------------
        mov     bx, [s2_mod_arr]
        mov     eax, [s2_mod_dest]
        mov     [bx], eax                       ; mod_start
        add     eax, [s2_mod_bytes]
        mov     [bx + 4], eax                   ; mod_end, one past the last
                                                ; byte, and the true length --
                                                ; not the sector padding
        movzx   eax, word [s2_mod_name]
        mov     [bx + 8], eax                   ; cmdline
        mov     dword [bx + 12], 0              ; reserved

        ; --- read it in ------------------------------------------------------
        ; s2_lba and s2_remaining are already set from the range checks above.
        mov     eax, [s2_mod_dest]
        mov     [s2_dest], eax
        call    s2_read_run             ; halts itself on a hard disk error

        ; --- on to the next --------------------------------------------------
        ; Round the destination up to the next page. Rounding the byte count
        ; rather than the sector count is safe and not an oversight: 4 KiB is
        ; a multiple of 512, so the page boundary above a length is never
        ; below the sector boundary above it, and the padding sector cannot
        ; reach into the next module.
        mov     eax, [s2_mod_dest]
        add     eax, [s2_mod_bytes]
        add     eax, 0xFFF
        and     eax, 0xFFFFF000
        mov     [s2_mod_dest], eax

        add     word [s2_mod_arr], MB_MOD_SIZE
        add     word [s2_mod_entry], S2MOD_ENTRY_SIZE

        dec     dword [s2_mod_left]
        jnz     .next

        ; --- and tell the kernel where it all is ------------------------------
        mov     eax, [s2_module_count]
        mov     [MB_INFO + MBI_MODS_COUNT], eax
        mov     dword [MB_INFO + MBI_MODS_ADDR], MB_MODS
        or      dword [MB_INFO + MBI_FLAGS], MB_FLAG_MODS
.none:
        ret                             ; eax = the count, zero on the .none path

.too_many:
        mov     si, s2_msg_mod_many
        jmp     s2_die
.bad_disk:
        mov     si, s2_msg_mod_disk
        jmp     s2_die
.bad_mem:
        mov     si, s2_msg_mod_mem
        jmp     s2_die
.bad_name:
        mov     si, s2_msg_mod_name
        jmp     s2_die

; ===========================================================================
; 5. Graphics mode -- provided by vbe.inc
; ===========================================================================
;
; CONTRACT (vbe.inc is written by another agent; this matches its header):
;
;   vbe_setup
;     in   real mode, ds = 0, ss:sp usable
;     out  CF clear -> a mode is active, the MBI_FB_* fields at MB_INFO are
;                      filled in and MB_FLAG_FRAMEBUFFER is already set
;          CF set   -> no mode was set, MB_INFO untouched, still in text mode
;     preserves every general register, ds and es; only the flags change.
;
; vbe.inc sets MB_FLAG_FRAMEBUFFER itself on success. The `or` on the success
; path below is therefore redundant -- kept anyway because it is idempotent
; and it keeps this file readable without cross-referencing the include. The
; reload of ds is redundant for the same reason; both are cheap insurance
; against the include's register discipline changing under us.
;
; vbe.inc also requires layout.inc to have been included already (it has, at
; the top of this file) and assembles into stage 2's own org, so it shares
; this file's size budget -- see the assertions at the bottom.
;
; Included here, in the 16-bit section, because it needs int 10h.
; ---------------------------------------------------------------------------

%include "vbe.inc"

; Re-assert the mode in case the include ended in a different one. Everything
; from here to the far jump is still 16-bit real mode code.
[BITS 16]

; ===========================================================================
; 6. Protected mode
; ===========================================================================

s2_go_protected:
        ; Interrupts off for good now. Two independent reasons: the IDTR still
        ; describes the BIOS's real mode interrupt vector table, which is
        ; meaningless once protected mode is on, and the PIC is still at its
        ; power-on mapping where IRQ 0..7 land on vectors 8..15 -- straight on
        ; top of the CPU's own exception vectors. Any interrupt between here
        ; and the kernel installing its own IDT is a triple fault. The kernel
        ; re-enables them after remapping the PIC.
        cli

        ; cli does not stop an NMI, and an NMI in the window before the kernel
        ; installs its own IDT is just as fatal as an IRQ. Bit 7 of the CMOS
        ; index port masks it. The port is write-only on most chipsets, so the
        ; value is written outright rather than read-modify-written; the dummy
        ; read of 0x71 afterwards completes the index cycle, which some
        ; chipsets insist on before they accept the next index write.
        ; (The kernel is free to clear this again once its IDT is up.)
        mov     al, 0x80
        out     0x70, al
        in      al, 0x71

        o32 lgdt [s2_gdtr]

        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax

        ; The far jump is not optional. Setting PE does not change cs; only
        ; loading cs does, and only a far transfer loads cs. It also flushes
        ; the prefetch queue, which still holds bytes decoded under 16-bit
        ; rules.
        jmp     SEL_CODE32:s2_pm_entry

[BITS 32]
s2_pm_entry:
        ; Now genuinely 32-bit. Give every data segment the flat descriptor.
        mov     ax, SEL_DATA32
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, PM_STACK

        ; The multiboot handover, byte for byte what GRUB does.
        mov     eax, MULTIBOOT_MAGIC_EAX
        mov     ebx, MB_INFO

        ; The image on disk is flat, not ELF -- the Makefile ran objcopy -- so
        ; there are no program headers to parse and the entry point is simply
        ; the first byte of what we loaded.
        jmp     KERNEL_PHYS

[BITS 16]

; ===========================================================================
; The GDT
; ===========================================================================
; Three entries: the mandatory null descriptor, and a code and a data
; descriptor that both span the whole address space (base 0, limit 0xFFFFF
; pages with the granularity bit set = 4 GiB). Flat segmentation, so the
; kernel can pretend segmentation does not exist and use paging instead.
;
; The same table serves the unreal mode excursion and the final switch, which
; is why it lives outside both routines.

align 8
s2_gdt:
        ; 0x00 -- null descriptor
        dq      0x0000000000000000

        ; 0x08 -- 32-bit code, base 0, limit 4 GiB
        ;   access  0x9A = present, ring 0, code, readable
        ;   flags   0xCF = granularity 4 KiB, 32-bit, limit 19..16 = 0xF
        dw      0xFFFF          ; limit 15..0
        dw      0x0000          ; base 15..0
        db      0x00            ; base 23..16
        db      0x9A            ; access
        db      0xCF            ; flags | limit 19..16
        db      0x00            ; base 31..24

        ; 0x10 -- 32-bit data, base 0, limit 4 GiB
        ;   access  0x92 = present, ring 0, data, writable
        dw      0xFFFF
        dw      0x0000
        db      0x00
        db      0x92
        db      0xCF
        db      0x00
s2_gdt_end:

s2_gdtr:
        dw      s2_gdt_end - s2_gdt - 1 ; limit = size - 1
        dd      s2_gdt                  ; linear base; org makes this absolute

; ===========================================================================
; Data
; ===========================================================================

; Disk address packet for int 13h AH=42h.
align 4
s2_dap:
        db      0x10                    ; packet size
        db      0
s2_dap_count:   dw      0               ; sectors to transfer
s2_dap_off:     dw      0               ; buffer offset
s2_dap_seg:     dw      0               ; buffer segment
s2_dap_lba_lo:  dd      0
s2_dap_lba_hi:  dd      0

s2_lba:         dd      0               ; next LBA to read
s2_dest:        dd      0               ; next physical destination
s2_remaining:   dd      0               ; sectors still to read
s2_chs_lba:     dd      0

; Module loader state. In memory rather than in registers because the loop it
; belongs to makes BIOS calls; see the comment in s2_load_modules.
s2_mod_left:    dd      0               ; modules still to load
s2_mod_bytes:   dd      0               ; length of the current one
s2_mod_dest:    dd      0               ; next page-aligned physical address
s2_mod_entry:   dw      0               ; -> its entry in s2_module_table
s2_mod_arr:     dw      0               ; -> its multiboot_module at MB_MODS
s2_mod_str:     dw      0               ; next free byte of MB_MODSTR
s2_mod_name:    dw      0               ; where its name was written

s2_chunk:       dw      0               ; sectors in the current chunk
s2_mmap_count:  dw      0               ; E820 entries recorded
s2_spt:         dw      63              ; sectors per track
s2_heads:       dw      16
s2_chs_cyl:     dw      0
s2_chs_head:    db      0
s2_chs_sector:  db      0
s2_boot_drive:  db      0x80
s2_use_lba:     db      0
s2_retries:     db      0
s2_safe_mode:   db      0               ; shift was held: force 80x25 text

; Kept short on purpose: one line per major step, so a hang or a reboot can
; always be pinned to the step whose line was printed last.
;
; The banner is the one exception, and it earns its extra thirty bytes: it is
; the only place the escape hatch is ever advertised. Reading it does not help
; on THIS boot -- the shift state was already sampled a dozen instructions ago
; -- but somebody who has seen it once knows what to hold down the day the
; screen comes up black, and by then there is nothing left to read it from.
s2_msg_banner:  db      13, 10, "TomatOS stage2 (hold shift for text mode)"
                db      13, 10, 0
s2_msg_a20:     db      "a20 ", 0
s2_msg_e820:    db      "e820 ", 0
s2_msg_load:    db      "load ", 0
s2_msg_mods:    db      "mods ", 0
s2_msg_vbe:     db      "vbe ", 0
s2_msg_ok:      db      "ok", 13, 10, 0
s2_msg_no:      db      "text", 13, 10, 0

; The escape hatch, twice. The long one is printed at the top of the boot and
; opens with 7 -- BEL -- which int 10h AH=0Eh sounds on the PC speaker: on a
; machine whose panel shows nothing, that beep is the entire user interface,
; and it is the only way to learn that the held key was seen. The short one
; takes the place of "ok" at step 5 so the step list stays one word per line.
s2_msg_safe:    db      7, "safe mode: 80x25 text forced", 13, 10, 0
s2_msg_safe_short: db   "safe", 13, 10, 0
s2_msg_skip:    db      "none", 13, 10, 0
s2_msg_fail:    db      "FAILED", 13, 10, 0
s2_msg_disk:    db      "disk error", 13, 10, 0
s2_msg_unreal:  db      "unreal/a20 check failed", 13, 10, 0

; The four ways the patched-in module table can be wrong. Each names the limit
; it broke, because the fix is a different one in each case: fewer programs, a
; smaller one, a bigger MODULES_MAX_SECTORS, or a shorter name.
s2_msg_mod_many: db     "too many modules", 13, 10, 0
s2_msg_mod_disk: db     "module outside the module area", 13, 10, 0
s2_msg_mod_mem:  db     "modules too big for memory", 13, 10, 0
s2_msg_mod_name: db     "module names too long", 13, 10, 0
s2_msg_halt:    db      "halted", 13, 10, 0

; ===========================================================================
; Build-time assertions
; ===========================================================================

s2_end:

; These are `times` assertions, not %if ones, and that is deliberate: %if is
; a PREPROCESSOR directive and the preprocessor cannot see labels, so
; "%if (s2_end - $$) > ..." quietly evaluates the label as nothing at all and
; never fires however far over budget the file goes. `times <expr> db 0` is
; evaluated by the assembler, where the labels are real; a relational operator
; yields 1 or 0, so multiplying by -1 gives a negative repeat count and NASM
; rejects it with "TIMES value -1 is negative" on the offending line. When the
; condition is false the count is 0 and not a single byte is emitted.

; ASSERT: the content must not reach DISK_BUFFER, which layout.inc places
; directly above stage 2's span. Growing into it would make the disk reads
; land on stage 2's own tail. Stated in terms of the two symbols rather than
; the sector count so it keeps holding if DISK_BUFFER is ever moved down.
times (((s2_end - $$) > (DISK_BUFFER - STAGE2_ORG)) * -1) db 0

; ASSERT: and the load budget, since stage 1 only reads STAGE2_SECTORS
; sectors -- anything past that simply never arrives in memory.
; If either fires, shrink stage 2; remember vbe.inc counts towards both.
times (((s2_end - $$) > (STAGE2_SECTORS * 512)) * -1) db 0

; Pad to whole sectors so the Makefile can write the file out as-is.
times (((s2_end - $$) + 511) / 512) * 512 - (s2_end - $$) db 0
