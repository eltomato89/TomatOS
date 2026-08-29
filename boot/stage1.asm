; TomatOS - stage 1, the boot sector
;
; The BIOS loads this single sector to 0x7C00 and jumps into it. Its whole
; job is to get stage 2 off the disk and jump there; anything cleverer does
; not fit in the space that is left once the FAT16 BPB has taken its share.
;
; This sector is simultaneously the FAT16 volume boot record of the disk
; image, so its first 62 bytes are not ours to spend:
;
;   0x00  3 bytes   a short jump over the BPB, followed by a NOP
;   0x03  59 bytes  OEM name + BPB, written into the image by mformat
;   0x3E  ...       our code
;   0x1FE 2 bytes   the 0x55AA signature the BIOS looks for
;
; Everything here therefore has to live in 448 bytes.

[BITS 16]

%include "layout.inc"

org STAGE1_ORG

; ---------------------------------------------------------------------------
; 0x00: the fixed FAT16 header
; ---------------------------------------------------------------------------

entry:
    jmp short start                 ; 2 bytes, jumps clear of the BPB
    nop                             ; the third byte DOS/mformat expect

%if ($ - $$) != 3
    %error "the jump/nop preamble must be exactly 3 bytes"
%endif

; Placeholder for the BPB. We assemble zeros here and the Makefile lets
; mformat's own BPB overwrite this range in the finished image -- that way
; the geometry, cluster size and volume ID always describe the filesystem
; that was actually created, instead of numbers we guessed at build time.
; Nothing in stage 1 reads these bytes, so their content is irrelevant to us;
; only their size matters, because it fixes where our code begins.
    times 59 db 0

%if ($ - $$) != 0x3E
    %error "code must start at offset 0x3E, right after the BPB"
%endif

; ---------------------------------------------------------------------------
; 0x3E: our code
; ---------------------------------------------------------------------------

start:
    ; The BIOS only promises a sane CS:IP and the boot drive in DL. The
    ; segment and stack registers can hold anything at all, so set up every
    ; one of them ourselves before touching memory.
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STAGE1_ORG              ; stack grows down from below our code
    sti

    ; Some BIOSes enter at 07C0:0000 rather than 0000:7C00. Both address the
    ; same byte, but our labels are org'd for segment 0, so force CS to 0.
    jmp 0x0000:.canonical
.canonical:
    cld                             ; lodsb/string ops must count upwards

    ; DL is the drive we were booted from. Stash it: hardcoding 0x80 would
    ; break booting from a floppy, a USB stick or a second disk, and every
    ; INT 13h call below reloads DL from here because the BIOS is free to
    ; clobber it.
    mov [drive], dl

    ; --- Is the INT 13h extensions interface available? -------------------
    ; AH=41h with BX=55AAh. On success CF is clear, BX comes back as AA55h
    ; and bit 0 of CX says that extended read is actually implemented. All
    ; three have to agree before we trust AH=42h.
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [drive]                 ; never rely on DL still being intact
    int 0x13
    jc .use_chs
    cmp bx, 0xAA55
    jne .use_chs
    test cl, 1
    jz .use_chs
    inc byte [use_ext]              ; 0 -> 1, one byte cheaper than mov
    jmp .load

    ; --- No extensions: fetch the real geometry ---------------------------
    ; AH=08h reports sectors-per-track and the highest head number for this
    ; drive. We ask instead of assuming 18/2 (1.44M floppy) or 63/16, because
    ; a wrong track size turns the LBA->CHS division below into reads of the
    ; wrong sectors -- which the BIOS happily performs without an error.
    ; ES:DI = 0 works around BIOSes that scribble through that pointer.
.use_chs:
    mov ah, 0x08
    mov dl, [drive]
    xor di, di
    mov es, di
    int 0x13
    jc fail
    and cl, 0x3F                    ; CL bits 5..0 = sectors per track
    jz fail                         ; a zero would divide-by-zero later
    mov [spt], cl                   ; high byte of the word stays 0
    mov al, dh                      ; DH = highest head number...
    xor ah, ah
    inc ax                          ; ...so the count is one more
    mov [heads], ax
    xor ax, ax
    mov es, ax                      ; INT 13h AH=08h may have changed ES

    ; --- Load stage 2 -----------------------------------------------------
    ; One sector per call, so a read never straddles a track boundary (which
    ; the CHS path cannot express) and a retry only repeats the sector that
    ; actually failed.
.load:
    mov ax, STAGE2_LBA
    mov bx, STAGE2_ORG              ; ES:BX, ES is still 0
    mov cx, STAGE2_SECTORS
.next:
    call read_sector
    add bx, 512
    inc ax
    loop .next

    ; Hand over with the boot drive still in DL, exactly as we received it.
    mov dl, [drive]
    jmp 0x0000:STAGE2_ORG

; ---------------------------------------------------------------------------
; read_sector - read the single LBA in AX to ES:BX
; Clobbers nothing the caller needs; jumps to fail if it cannot succeed.
; ---------------------------------------------------------------------------
read_sector:
    mov di, 4                       ; attempts; floppies often fail the first
.retry:
    push ax
    push bx
    push cx
    push di

    cmp byte [use_ext], 0
    je .chs

    ; Extended read: the packet takes the LBA directly, so no geometry and
    ; no 1024-cylinder ceiling to worry about.
    mov [dap_offset], bx
    mov [dap_lba], ax               ; the upper 6 bytes are zero in the image
    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jmp .check

    ; Classic read. Convert the LBA with the geometry we queried:
    ;   sector   = LBA % spt + 1      (CHS sectors are 1-based)
    ;   head     = (LBA / spt) % heads
    ;   cylinder = (LBA / spt) / heads
    ; The cylinder is 10 bits: its low 8 go in CH, its high 2 into the top
    ; two bits of CL, above the 6-bit sector number.
.chs:
    xor dx, dx
    div word [spt]                  ; AX = LBA/spt, DX = LBA%spt
    inc dx
    mov cl, dl                      ; sector, bits 5..0
    xor dx, dx
    div word [heads]                ; AX = cylinder, DX = head
    mov ch, al
    shl ah, 6
    or cl, ah
    mov dh, dl                      ; head
    mov dl, [drive]
    mov ax, 0x0201                  ; AH=02h read, AL=1 sector
    int 0x13

.check:
    pop di
    pop cx
    pop bx
    pop ax
    jnc .done

    ; Reset the controller before trying again -- after an error the drive
    ; may need to recalibrate, and a bare retry would just fail identically.
    push ax
    xor ax, ax
    mov dl, [drive]
    int 0x13
    pop ax
    dec di
    jnz .retry
    jmp fail
.done:
    ret

; ---------------------------------------------------------------------------
; The one and only error path. There is no room for a message system, so it
; is a single string and a teletype loop.
; ---------------------------------------------------------------------------
fail:
    mov si, msg_err
.putc:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 0x0007                  ; page 0, light grey (BL matters in gfx)
    int 0x10
    jmp .putc
.halt:
    cli
    hlt
    jmp .halt                       ; an NMI could wake us; stay put

; ---------------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------------

msg_err     db "S1 disk error", 0

drive       db 0                    ; boot drive as handed to us in DL
use_ext     db 0                    ; nonzero once AH=41h has said yes
spt         dw 0                    ; sectors per track, CHS path only
heads       dw 0                    ; head count, CHS path only

; Disk address packet for AH=42h. Kept in the image rather than built on the
; stack so the six high bytes of the LBA are permanently zero and we only
; ever have to patch the low word.
dap:
            db 0x10                 ; packet size
            db 0                    ; reserved
            dw 1                    ; sectors to transfer
dap_offset: dw 0                    ; buffer offset...
            dw 0                    ; ...and segment
dap_lba:    dq 0                    ; starting LBA

; ---------------------------------------------------------------------------
; Padding and signature
; ---------------------------------------------------------------------------

%if ($ - $$) > 0x1FE
    %error "stage 1 does not fit in the boot sector"
%endif

    times 0x1FE - ($ - $$) db 0
    dw 0xAA55
