; bkerndev - Bran's Kernel Development Tutorial
; By:   Brandon F. (friesenb@gmail.com)
; Desc: Kernel entry point, stack, and Interrupt Service Routines.
;
; Notes: No warranty expressed or implied. Use at own risk.
;
; This is the kernel's entry point. We could either call main here,
; or we can use this to setup the stack or other nice stuff, like
; perhaps setting up the GDT and segments. Please note that interrupts
; are disabled at this point: More on interrupts later!
[BITS 32]

; ---------------------------------------------------------------------------
; Higher half constants. These MUST agree with src/include/vmm.h.
;
; The kernel is linked for virtual 0xC0100000 but the bootloader loads and
; enters it at physical 0x00100000. Until paging is on, every linker supplied
; symbol therefore has to be corrected by hand:
;
;     physical address = symbol - KERNEL_VIRTUAL_BASE
;
; which is exactly what the V2P() macro does on the C side.
; ---------------------------------------------------------------------------
KERNEL_VIRTUAL_BASE equ 0xC0000000
KERNEL_PDE_INDEX    equ (KERNEL_VIRTUAL_BASE >> 22)   ; 768

; Page directory entry for a 4 MiB page at physical 0:
;   bit 0 P (present), bit 1 RW (writable), bit 7 PS (4 MiB page)
PDE_PRESENT         equ 0x001
PDE_WRITE           equ 0x002
PDE_PAGE_SIZE       equ 0x080
PDE_BOOT_FLAGS      equ (PDE_PRESENT | PDE_WRITE | PDE_PAGE_SIZE)

CR4_PSE             equ 0x00000010   ; page size extensions -> 4 MiB pages
CR0_PG              equ 0x80000000   ; paging enable

; Bounds of the .bss section, supplied by linker.ld. As VIRTUAL addresses,
; like every other linker symbol here, so both need the manual correction
; while paging is still off.
extern bss_start
extern bss_end

global start
start:
    ; ---------------------------------------------------------------------
    ; Physical world. eip is somewhere around 0x00100000, paging is off, the
    ; GDT is the flat one GRUB left behind (base 0, limit 4 GiB), so linear
    ; address == physical address for the moment.
    ; ---------------------------------------------------------------------

    ; ---------------------------------------------------------------------
    ; Zero .bss.
    ;
    ; This is the FIRST thing that happens, and it has to be, because every
    ; other piece of state this routine owns lives in .bss: the two multiboot
    ; slots below, the boot page directory further down, and the stack that
    ; higher_half switches to. Zeroing after any of them were set up would
    ; throw exactly that away again.
    ;
    ; Why we do it at all: .bss is NOBITS, i.e. it exists in the ELF program
    ; headers only as memsz > filesz, and GRUB zeroes that difference for us.
    ; A FLAT image carries no such information -- stage 2 copies a byte count
    ; off the disk and stops, so whatever the RAM behind the image happened to
    ; hold becomes our zero-initialised data. Doing it here makes the kernel
    ; correct under both loaders and dependent on neither.
    ;
    ; Paging is off, so bss_start/bss_end -- linked at 0xC0xxxxxx like
    ; everything else -- have to be corrected to their load addresses by hand.
    ;
    ; Register discipline, and this is the subtle part: eax and ebx still hold
    ; the multiboot handover (see below) and cannot be saved anywhere yet,
    ; there being no stack. "rep stosd" insists on eax as the pattern, so the
    ; magic is parked in esi for the duration; ebx, ecx, edx, esi and edi are
    ; all untouched by the string instruction itself, so ebx simply survives.
    ;
    ; The byte count is split into whole dwords plus a tail of at most three
    ; bytes rather than rounded up: rounding up would write past bss_end, and
    ; bss_end is only padded to a page boundary by linker.ld AFTER the symbol.
    ; ---------------------------------------------------------------------
    cld
    mov esi, eax                    ; magic out of the way of "rep stosd"

    mov edi, bss_start - KERNEL_VIRTUAL_BASE
    mov edx, bss_end   - KERNEL_VIRTUAL_BASE
    sub edx, edi                    ; size of .bss in bytes
    xor eax, eax

    mov ecx, edx
    shr ecx, 2
    rep stosd                       ; the bulk, four bytes at a time

    mov ecx, edx
    and ecx, 3
    rep stosb                       ; .bss need not be a multiple of 4

    ; Per the Multiboot specification the bootloader hands us the magic
    ; 0x2BADB002 in eax and the physical address of the info structure in ebx.
    ; Both are saved away as soon as .bss can hold them: we do not have a
    ; stack of our own yet (esp still points into the bootloader), so pushing
    ; them was never an option. From here on esi/ebx may be overwritten
    ; freely.
    ;
    ; The two slots live in .bss and are therefore linked at 0xC0xxxxxx, an
    ; address that does not exist yet. Store through their physical aliases.
    mov [mboot_magic - KERNEL_VIRTUAL_BASE], esi
    mov [mboot_info  - KERNEL_VIRTUAL_BASE], ebx

    ; ---------------------------------------------------------------------
    ; Build the boot page directory.
    ;
    ; It lives in .bss and is therefore already zero -- the loop that used to
    ; clear its 1024 entries here is gone, because the wholesale zeroing above
    ; covers it. What that loop was defending against was a bootloader that
    ; does not clear the bss (a stray present bit here is a triple fault with
    ; no output at all); that job is now done unconditionally by this same
    ; routine, ten instructions earlier and in plain sight, instead of being
    ; hoped for from outside. Only the two entries we actually want are
    ; written.
    ;
    ; 4 MiB pages (CR4.PSE) are used deliberately: two directory entries and
    ; no second level table at all, which means no extra 4 KiB of aligned
    ; scratch memory to find and no pointer chasing while paging is still
    ; off. A 4 KiB boot table would need 1024 entries filled in a loop for
    ; no benefit -- these mappings are throwaway, vmm_init() replaces them.
    ; ---------------------------------------------------------------------
    mov edi, boot_page_directory - KERNEL_VIRTUAL_BASE

    ; PDE 0: identity map the first 4 MiB (virtual 0x00000000 -> physical 0).
    ; This is what keeps the instruction right after "mov cr0, eax" fetchable,
    ; because eip is still a low physical address at that point.
    mov dword [edi], PDE_BOOT_FLAGS

    ; PDE 768: map 0xC0000000..0xC03FFFFF onto the same first 4 MiB, so the
    ; kernel image at physical 0x00100000 also appears at 0xC0100000, exactly
    ; where it was linked.
    mov dword [edi + KERNEL_PDE_INDEX * 4], PDE_BOOT_FLAGS

    ; ---------------------------------------------------------------------
    ; Turn paging on. CR4.PSE must be set BEFORE CR0.PG, otherwise the PS bit
    ; in our entries is a reserved bit and the first translation faults.
    ; ---------------------------------------------------------------------
    mov eax, cr4
    or  eax, CR4_PSE
    mov cr4, eax

    mov eax, boot_page_directory - KERNEL_VIRTUAL_BASE   ; CR3 wants physical
    mov cr3, eax

    mov eax, cr0
    or  eax, CR0_PG
    mov cr0, eax
    ; Paging is live. eip is still in the low range and resolves through the
    ; identity mapping, so the next fetch succeeds.

    ; A near jump is relative to eip and would keep us down in the low range
    ; forever. Load the LINKED (virtual) address of the target into a register
    ; and jump indirectly: the immediate is the absolute value 0xC01xxxxx that
    ; the linker resolved, so this write to eip is what actually moves
    ; execution into the higher half.
    mov eax, higher_half
    jmp eax

higher_half:
    ; ---------------------------------------------------------------------
    ; From here on eip is above 0xC0000000 and every linker symbol may be
    ; used as written -- no more manual "- KERNEL_VIRTUAL_BASE".
    ; ---------------------------------------------------------------------

    ; The stack only becomes valid at this instruction: sys_stack is a virtual
    ; address in .bss and until now there was no mapping for it. Everything
    ; above this point ran without a usable stack, which is why nothing above
    ; pushes, calls or uses ebp.
    mov esp, sys_stack

    ; The low identity mapping (PDE 0) is deliberately LEFT IN PLACE. It is
    ; still needed by anything that has not been converted to the higher half
    ; view yet, and vmm_init() is the one that rebuilds the page tables from
    ; scratch and drops it once nothing depends on it any more.

    jmp stublet

; This part MUST be 4byte aligned, so we solve that issue using 'ALIGN 4'
ALIGN 4
mboot:
    ; Multiboot macros to make a few lines later more readable
    MULTIBOOT_PAGE_ALIGN	equ 1<<0
    MULTIBOOT_MEMORY_INFO	equ 1<<1
    MULTIBOOT_VIDEO_MODE	equ 1<<2
    MULTIBOOT_HEADER_MAGIC	equ 0x1BADB002
    MULTIBOOT_HEADER_FLAGS	equ MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO | MULTIBOOT_VIDEO_MODE
    MULTIBOOT_CHECKSUM	equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

    ; The mode we would like. mode_type 0 is "linear graphics", 1 would be
    ; "EGA text"; width and height are then pixels and depth is bits per
    ; pixel. 1920x1080x32 is the top row of vbe_res_table in boot/vbe.inc as
    ; well, so both boot paths aim at the same screen and the framebuffer
    ; console gets the same 240x67 character grid either way.
    ;
    ; A zero in any of the three would mean "no preference"; we state all
    ; three, because a loader that cannot serve them is REQUIRED to fall back
    ; to something close rather than to fail, and "close to 1920x1080x32" is a
    ; far better guess than "close to nothing".
    ;
    ; This is a PREFERENCE and nothing more, which is why asking for the
    ; maximum is safe rather than greedy. Three things can happen to it:
    ;
    ;   * GRUB reads it, finds the closest mode the card actually has, and
    ;     sets it. What arrives is then GRUB's choice and not ours, and it
    ;     need not be a mode this project has ever heard of -- a card given
    ;     2 MiB of video memory produced 1280x800 here, which appears in no
    ;     table of ours. That is fine: the request below is the only lever
    ;     this path has, and the console sizes itself to whatever it is told.
    ;   * QEMU's -kernel loader ignores the video fields entirely -- its
    ;     source says as much ("multiboot knows VBE. we don't") -- and the
    ;     kernel comes up in text mode, exactly as it did before.
    ;   * Our own stage 2 never reads this header at all; it negotiates
    ;     through vbe.inc and hands over the result in the same multiboot
    ;     info structure.
    ;
    ; In all three cases the kernel is told what it actually got, in
    ; MBI_FB_*, and fbcon.c copes with a smaller mode, a different mode, or
    ; no framebuffer at all. Nothing downstream assumes the numbers below.
    MULTIBOOT_VIDEO_LINEAR	equ 0
    MULTIBOOT_VIDEO_WIDTH	equ 1920
    MULTIBOOT_VIDEO_HEIGHT	equ 1080
    MULTIBOOT_VIDEO_DEPTH	equ 32

    ; The ceiling, mirrored from src/include/fbcon.h. See the long comment
    ; there: this header is one of the four places that have to agree about
    ; FBCON_MAX_WIDTH/FBCON_MAX_HEIGHT, and asking a loader for a mode bigger
    ; than the console's statically sized shadow buffer would get us a screen
    ; whose top left corner is the console and whose remainder is whatever
    ; GRUB left behind -- with no error reported anywhere.
    FBCON_MAX_WIDTH		equ 1920
    FBCON_MAX_HEIGHT		equ 1080

    ; This is the GRUB Multiboot header. A boot signature
    ; No AOUT kludge: the bootloader uses the ELF program headers instead.
    ;
    ; The layout below is the whole reason this header is written out field by
    ; field instead of as three dwords. Multiboot 1 puts every field at a
    ; FIXED offset from the magic -- the flag bits say which ones are valid,
    ; they do not say which ones are present:
    ;
    ;   0x00 magic          0x0C header_addr    0x20 mode_type
    ;   0x04 flags          0x10 load_addr      0x24 width
    ;   0x08 checksum       0x14 load_end_addr  0x28 height
    ;                       0x18 bss_end_addr   0x2C depth
    ;                       0x1C entry_addr
    ;
    ; The five address fields at 0x0C..0x1C belong to the a.out kludge (flag
    ; bit 16), which we do NOT set -- the loader takes the ELF program headers
    ; instead. But the video fields sit at 0x20 regardless, so those five
    ; dwords have to be emitted as padding anyway. Leaving them out and
    ; writing mode_type straight after the checksum would put it at 0x0C,
    ; where the loader reads header_addr, and the header would describe a
    ; kernel to be loaded at address 0 with 1024 as its entry point. That is
    ; read and acted upon before a single instruction of ours runs, so there
    ; would be nothing to debug it with.
    ;
    ; They are zero and stay zero: with bit 16 clear the loader must not look
    ; at them at all, and zero is the value that is obviously not a real
    ; address should anything ever look anyway.
    dd MULTIBOOT_HEADER_MAGIC
    dd MULTIBOOT_HEADER_FLAGS
    dd MULTIBOOT_CHECKSUM

    dd 0                    ; header_addr    ) a.out kludge, flag bit 16 --
    dd 0                    ; load_addr      ) not set, so these are padding
    dd 0                    ; load_end_addr  ) that only exists to place the
    dd 0                    ; bss_end_addr   ) video fields at offset 0x20
    dd 0                    ; entry_addr     )

    dd MULTIBOOT_VIDEO_LINEAR   ; mode_type
    dd MULTIBOOT_VIDEO_WIDTH
    dd MULTIBOOT_VIDEO_HEIGHT
    dd MULTIBOOT_VIDEO_DEPTH

; ASSERT: the requested mode must not exceed the console's ceiling.
;
; Written as a `times` assertion rather than as "%if MULTIBOOT_VIDEO_WIDTH >
; FBCON_MAX_WIDTH", and that is not a style choice: %if is a PREPROCESSOR
; directive and the preprocessor cannot see an `equ`. Both operands would
; evaluate as nothing, the comparison would be 0 > 0, and the assertion would
; sit here looking reassuring while never firing at any value whatsoever.
; `times <expr> db 0` is evaluated by the assembler, where the equs are real;
; a relational operator yields 1 or 0, so multiplying by -1 gives a negative
; repeat count and NASM rejects it by name on this line. When the condition is
; false the count is 0 and not one byte is emitted -- which matters here more
; than usual, since a single stray byte between the header and stublet would
; be a byte of padding inside the .text the loader is reading.
;
; The same idiom guards the size budget at the bottom of boot/stage2.asm, for
; the same reason.
times (((MULTIBOOT_VIDEO_WIDTH  > FBCON_MAX_WIDTH)  | \
        (MULTIBOOT_VIDEO_HEIGHT > FBCON_MAX_HEIGHT)) * -1) db 0

; This is an endless loop here. Make a note of this: Later on, we
; will insert an 'extern kernel', followed by 'call kernel', right
; before the 'jmp $'.
stublet:
	 extern kernel
	 ; cdecl: arguments are pushed from right to left, so for
	 ; kernel(magic, mbi) the mbi pointer goes first, then the magic.
	 push dword [mboot_info]
	 push dword [mboot_magic]
	 call kernel
	 add esp, 8             ; cdecl: the caller cleans up the arguments
	 jmp $

; This will set up our new segment registers. We need to do
; something special in order to set CS. We do what is called a
; far jump. A jump that includes a segment as well as an offset.
; This is declared in C as 'extern void gdt_flush();'
global gdt_flush
extern gp
gdt_flush:
    lgdt [gp]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:flush2
flush2:
    ret

; Loads the IDT defined in 'idtp' into the processor.
; This is declared in C as 'extern void idt_load();'
global idt_load
extern idtp
idt_load:
    lidt [idtp]
    ret

; In just a few pages in this tutorial, we will add our Interrupt
; Service Routines (ISRs) right here!
global isr0
global isr1
global isr2
global isr3
global isr4
global isr5
global isr6
global isr7
global isr8
global isr9
global isr10
global isr11
global isr12
global isr13
global isr14
global isr15
global isr16
global isr17
global isr18
global isr19
global isr20
global isr21
global isr22
global isr23
global isr24
global isr25
global isr26
global isr27
global isr28
global isr29
global isr30
global isr31

;  0: Divide By Zero Exception
isr0:
    cli
    push byte 0
    push byte 0
    jmp isr_common_stub

;  1: Debug Exception
isr1:
    cli
    push byte 0
    push byte 1
    jmp isr_common_stub

;  2: Non Maskable Interrupt Exception
isr2:
    cli
    push byte 0
    push byte 2
    jmp isr_common_stub

;  3: Int 3 Exception
isr3:
    cli
    push byte 0
    push byte 3
    jmp isr_common_stub

;  4: INTO Exception
isr4:
    cli
    push byte 0
    push byte 4
    jmp isr_common_stub

;  5: Out of Bounds Exception
isr5:
    cli
    push byte 0
    push byte 5
    jmp isr_common_stub

;  6: Invalid Opcode Exception
isr6:
    cli
    push byte 0
    push byte 6
    jmp isr_common_stub

;  7: Coprocessor Not Available Exception
isr7:
    cli
    push byte 0
    push byte 7
    jmp isr_common_stub

;  8: Double Fault Exception (With Error Code!)
isr8:
    cli
    push byte 8
    jmp isr_common_stub

;  9: Coprocessor Segment Overrun Exception
isr9:
    cli
    push byte 0
    push byte 9
    jmp isr_common_stub

; 10: Bad TSS Exception (With Error Code!)
isr10:
    cli
    push byte 10
    jmp isr_common_stub

; 11: Segment Not Present Exception (With Error Code!)
isr11:
    cli
    push byte 11
    jmp isr_common_stub

; 12: Stack Fault Exception (With Error Code!)
isr12:
    cli
    push byte 12
    jmp isr_common_stub

; 13: General Protection Fault Exception (With Error Code!)
isr13:
    cli
    push byte 13
    jmp isr_common_stub

; 14: Page Fault Exception (With Error Code!)
isr14:
    cli
    push byte 14
    jmp isr_common_stub

; 15: Reserved Exception
isr15:
    cli
    push byte 0
    push byte 15
    jmp isr_common_stub

; 16: Floating Point Exception
isr16:
    cli
    push byte 0
    push byte 16
    jmp isr_common_stub

; 17: Alignment Check Exception
isr17:
    cli
    push byte 0
    push byte 17
    jmp isr_common_stub

; 18: Machine Check Exception
isr18:
    cli
    push byte 0
    push byte 18
    jmp isr_common_stub

; 19: Reserved
isr19:
    cli
    push byte 0
    push byte 19
    jmp isr_common_stub

; 20: Reserved
isr20:
    cli
    push byte 0
    push byte 20
    jmp isr_common_stub

; 21: Reserved
isr21:
    cli
    push byte 0
    push byte 21
    jmp isr_common_stub

; 22: Reserved
isr22:
    cli
    push byte 0
    push byte 22
    jmp isr_common_stub

; 23: Reserved
isr23:
    cli
    push byte 0
    push byte 23
    jmp isr_common_stub

; 24: Reserved
isr24:
    cli
    push byte 0
    push byte 24
    jmp isr_common_stub

; 25: Reserved
isr25:
    cli
    push byte 0
    push byte 25
    jmp isr_common_stub

; 26: Reserved
isr26:
    cli
    push byte 0
    push byte 26
    jmp isr_common_stub

; 27: Reserved
isr27:
    cli
    push byte 0
    push byte 27
    jmp isr_common_stub

; 28: Reserved
isr28:
    cli
    push byte 0
    push byte 28
    jmp isr_common_stub

; 29: Reserved
isr29:
    cli
    push byte 0
    push byte 29
    jmp isr_common_stub

; 30: Reserved
isr30:
    cli
    push byte 0
    push byte 30
    jmp isr_common_stub

; 31: Reserved
isr31:
    cli
    push byte 0
    push byte 31
    jmp isr_common_stub


; We call a C function in here. We need to let the assembler know
; that 'fault_handler' exists in another file
extern fault_handler

; This is our common ISR stub. It saves the processor state, sets
; up for kernel mode segments, calls the C-level fault handler,
; and finally restores the stack frame.
isr_common_stub:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov eax, esp
    push eax
    mov eax, fault_handler
    call eax
    pop eax
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

global irq0
global irq1
global irq2
global irq3
global irq4
global irq5
global irq6
global irq7
global irq8
global irq9
global irq10
global irq11
global irq12
global irq13
global irq14
global irq15

; 32: IRQ0
irq0:
    cli
    push byte 0
    push byte 32
    ;jmp irq_common_stub
	jmp irq_common_stub

; 33: IRQ1
irq1:
    cli
    push byte 0
    push byte 33
    jmp irq_common_stub

; 34: IRQ2
irq2:
    cli
    push byte 0
    push byte 34
    jmp irq_common_stub

; 35: IRQ3
irq3:
    cli
    push byte 0
    push byte 35
    jmp irq_common_stub

; 36: IRQ4
irq4:
    cli
    push byte 0
    push byte 36
    jmp irq_common_stub

; 37: IRQ5
irq5:
    cli
    push byte 0
    push byte 37
    jmp irq_common_stub

; 38: IRQ6
irq6:
    cli
    push byte 0
    push byte 38
    jmp irq_common_stub

; 39: IRQ7
irq7:
    cli
    push byte 0
    push byte 39
    jmp irq_common_stub

; 40: IRQ8
irq8:
    cli
    push byte 0
    push byte 40
    jmp irq_common_stub

; 41: IRQ9
irq9:
    cli
    push byte 0
    push byte 41
    jmp irq_common_stub

; 42: IRQ10
irq10:
    cli
    push byte 0
    push byte 42
    jmp irq_common_stub

; 43: IRQ11
irq11:
    cli
    push byte 0
    push byte 43
    jmp irq_common_stub

; 44: IRQ12
irq12:
    cli
    push byte 0
    push byte 44
    jmp irq_common_stub

; 45: IRQ13
irq13:
    cli
    push byte 0
    push byte 45
    jmp irq_common_stub

; 46: IRQ14
irq14:
    cli
    push byte 0
    push byte 46
    jmp irq_common_stub

; 47: IRQ15
irq15:
    cli
    push byte 0
    push byte 47
    jmp irq_common_stub

extern irq_handler

irq_common_stub:

    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
	
    push esp

	call irq_handler
	
	mov esp, eax
	
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8

    iret

; ---------------------------------------------------------------------------
; Ring 3 support.
;
; Segment selectors, as laid out by the GDT:
;
;   0x08  ring 0 code      0x10  ring 0 data
;   0x18  ring 3 code      0x20  ring 3 data
;
; The two user selectors are always used with RPL 3 (the low two bits), which
; is what turns 0x18/0x20 into 0x1B/0x23. Loading a user data selector with
; RPL 0 from ring 3 would fault, and an iret to a CS whose RPL is not 3 would
; not leave ring 0 at all.
; ---------------------------------------------------------------------------
KERNEL_DATA_SEL     equ 0x10
USER_CODE_SEL       equ (0x18 | 3)     ; 0x1B
USER_DATA_SEL       equ (0x20 | 3)     ; 0x23

; EFLAGS for a freshly started ring 3 thread: bit 1 is the reserved
; always-one bit, bit 9 is IF. IF MUST be set here -- ring 3 cannot execute
; sti (it is privileged), so a task started with interrupts disabled could
; never be preempted by the timer and would own the CPU forever.
USER_EFLAGS         equ 0x202

extern syscall_handler

; System call entry, installed as the handler for vector 0x80.
;
; Deliberately no 'cli': the gate for 0x80 is a TRAP gate, so the CPU leaves
; IF alone and a system call stays preemptible by the timer. That is what
; makes a blocking call such as sleep() possible in the first place -- inside
; an interrupt gate IF would be clear and the tick that has to wake us could
; never arrive. The stub itself is agnostic: it neither clears nor sets IF, so
; it also behaves correctly behind an interrupt gate, only without preemption.
;
; The frame built here is byte for byte the one the ISR/IRQ stubs build, so
; the C side sees a plain 'struct regs *'. syscall_handler() reads the call
; number and the arguments out of the saved eax/ebx/ecx/edx and writes the
; result back into r->eax; the popa below is what hands it to the caller.
global syscall_stub
syscall_stub:
    ; int 0x80 pushes no error code, so supply a dummy one to keep the layout
    ; identical to the exceptions that do, then the vector number.
    push byte 0                 ; err_code
    push dword 0x80             ; int_no (0x80 does not fit in a signed imm8)

    pusha
    push ds
    push es
    push fs
    push gs

    ; Coming from ring 3, ds/es/fs/gs still hold the USER selectors (0x23).
    ; Every kernel access through them would run against a DPL 3 descriptor,
    ; so they are reloaded before a single line of C runs. The originals were
    ; saved above and are restored on the way out, which is what lets the
    ; iret return into ring 3 with the user's segments intact.
    mov ax, KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                    ; struct regs *r
    call syscall_handler
    add esp, 4                  ; drop the argument; the handler returns void

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8                  ; discard int_no and err_code
    iret

; void enter_user_mode(uint32_t entry, uint32_t user_stack);
;
; There is no instruction that switches to ring 3. The way in is to build the
; exact stack frame an iret coming back from a ring 3 interrupt would consume
; and then execute that iret: because the target CS has RPL 3, the CPU treats
; it as a return to a less privileged level and pops SS:ESP as well.
;
; Never returns.
global enter_user_mode
enter_user_mode:
    ; cdecl arguments, read BEFORE anything is pushed -- once the iret frame
    ; is being built, [esp + 4] no longer points at our own arguments.
    mov eax, [esp + 4]          ; entry     -> user EIP
    mov ecx, [esp + 8]          ; user_stack -> user ESP

    ; iret restores SS from the frame, but not ds/es/fs/gs: those have to be
    ; switched to the user data segment by hand, here, while we still may.
    mov dx, USER_DATA_SEL
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    ; The frame, in the order the CPU pops it (last pushed is popped first).
    push dword USER_DATA_SEL    ; SS
    push ecx                    ; ESP
    push dword USER_EFLAGS      ; EFLAGS, IF set
    push dword USER_CODE_SEL    ; CS, RPL 3 -- this is what drops the CPL
    push eax                    ; EIP
    iret

; Here is the definition of our BSS section. Right now, we'll use
; it just to store the stack. Remember that a stack actually grows
; downwards, so we declare the size of the data before declaring
; the identifier 'sys_stack'
global EnableA20Gate
EnableA20Gate:
	push	ebp
	mov		ebp, esp
	
	;Wait until a command can be sent
.1:
	in		al, 0x64
	test	al, 00000010b
	jnz		.1
	
	;Send the command to read the status byte
	mov		al, 0xD0
	out		0x64, al
	
	;Wait until the byte can be read
.2:
	in		al, 0x64
	test	al, 00000001b
	jz		.2
	
	;Read the status byte
	in		al, 0x60
	or		al, 00000010b	;set the A20 gate enable bit
	push	eax
	
	;Wait until a command can be sent
.3:
	in		al, 0x64
	test	al, 00000010b
	jnz		.3
	
	;Send the command to write the status byte
	mov		al, 0xD1
	out		0x64, al
	
	;Wait until a byte can be sent
.4:	
	in		al, 0x64
	test	al, 00000010b
	jnz		.4
	
	;Send the status byte
	pop		eax
	out		0x60, al
	
	;Wait until a command can be sent
.5:	
	in		al, 0x64
	test	al, 00000010b
	jnz		.5
	
	;Send the command to read the status byte
	mov		al, 0xD0
	out		0x64, al
	
	;Wait until a byte can be read
.6:
	in		al, 0x64
	test	al, 00000001b
	jz		.6
	
	;Read the status byte
	in		al, 0x60
	test	al, 00000010b
	jnz		.success
	
	mov		eax, 0
	jmp		.end
	
.success:
	mov		eax, 1

.end:	
	mov		esp, ebp
	pop		ebp
	ret

; The whole section needs 4 KiB alignment because the boot page directory
; lives in it; CR3 only keeps bits 31..12 of the address.
SECTION .bss align=4096

; Copies of the multiboot registers saved in 'start'. They deliberately sit
; BEFORE the stack area: the stack grows downwards from sys_stack, so values
; placed behind sys_stack would be destroyed by the very first push.
; Written through their physical aliases before paging is on, read through
; their virtual ones afterwards.
mboot_magic: resd 1
mboot_info:  resd 1

; The boot stack. Its size is picked to end exactly on the 4 KiB boundary the
; page directory below needs, which is why it is written as an arithmetic
; expression rather than as a round number: the two saved multiboot words plus
; the stack have to add up to a multiple of 4096, or the "alignb 4096" below
; inserts padding that is reserved, mapped and never touched by anything.
; At 8192 it did exactly that -- 8200 rounded up to 12288 and 4088 bytes were
; lost. The same section footprint now holds a stack half again as large.
    resb 12288 - 8          ; 12280 bytes; 12280 + 8 = 12288 = 3 pages
sys_stack:

; The page directory used to get into the higher half. Placed ABOVE the top of
; the stack on purpose, so a stack overflow (which grows downwards away from
; sys_stack) cannot scribble over the live paging structures.
; Zeroed and filled in by 'start'; thrown away again by vmm_init().
alignb 4096
global boot_page_directory
boot_page_directory:
    resd 1024               ; 1024 entries * 4 bytes = one 4 KiB page

; Tell the linker that this object does not require an executable stack.
section .note.GNU-stack noalloc noexec nowrite progbits
