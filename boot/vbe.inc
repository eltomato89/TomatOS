; TomatOS - VESA BIOS Extensions mode setup, real mode part of the boot chain
;
; %include'd by stage 2. This file provides routines and data, never an
; entry point of its own -- control reaches it only through "call vbe_setup".
;
; Why this lives here and nowhere else: setting a graphics mode means asking
; the video BIOS, and the video BIOS is only reachable through int 0x10, which
; only exists in real mode. Once stage 2 has switched to protected mode the
; door is shut: getting back would need a v86 monitor in the kernel. That is
; precisely why the kernel has been stuck with 320x200 or plain text until
; now. These few hundred bytes of real mode code are the entire reason the
; kernel can have a truecolour framebuffer at all -- up to 1920x1080 where the
; card offers it, and down to 640x480 where it does not, from the same binary.
;
; Assumptions about the environment stage 2 provides:
;
;   * "boot/layout.inc" has ALREADY been %include'd by the caller. This file
;     deliberately does not include it itself, because layout.inc has no
;     include guard and NASM rejects a redefined "equ". We use MB_INFO, the
;     MBI_FB_* offsets, MB_FB_TYPE_RGB and MB_FLAG_FRAMEBUFFER from it.
;   * [BITS 16], and the file assembled into stage 2's own "org", so labels
;     here are offsets from STAGE2_ORG just like the rest of stage 2.
;   * DS = 0, as everywhere else in stage 2: every address in layout.inc is a
;     flat address below 1 MiB and only means what it says with a zero data
;     segment. Our own buffers are addressed the same way. ES is set by us
;     where the BIOS needs it and restored on the way out.
;   * A 386 or better, since we use 32-bit operands to move the framebuffer
;     address and pitch about. Stage 2 needs a 386 anyway for unreal mode.

%ifndef TOMATOS_VBE_INC
%define TOMATOS_VBE_INC

; ---------------------------------------------------------------------------
; VbeInfoBlock offsets (returned by AX=4F00h, 512 bytes)
; ---------------------------------------------------------------------------
VBE_CI_SIGNATURE        equ 0x00    ; 4 bytes, 'VESA' on return
VBE_CI_VERSION          equ 0x04    ; word, BCD: 0x0200 = VBE 2.0, 0x0300 = 3.0
VBE_CI_VIDEO_MODE_PTR   equ 0x0E    ; dword, real mode FAR pointer to the list
VBE_CI_VIDEO_MODE_OFF   equ 0x0E    ;   low word  = offset
VBE_CI_VIDEO_MODE_SEG   equ 0x10    ;   high word = segment
VBE_CI_TOTAL_MEMORY     equ 0x12    ; word, video memory in 64 KiB blocks

; ---------------------------------------------------------------------------
; ModeInfoBlock offsets (returned by AX=4F01h, 256 bytes)
;
; Every single one of these is documented with its offset on purpose. A wrong
; offset here does not produce an error, it produces a framebuffer description
; that looks entirely plausible and paints garbage -- the worst kind of bug to
; chase from inside a kernel that has no output yet.
; ---------------------------------------------------------------------------
VBE_MI_ATTRIBUTES       equ 0x00    ; word  ModeAttributes
                                    ;   bit 0 = mode supported by the hardware
                                    ;   bit 3 = colour mode
                                    ;   bit 4 = graphics mode (0 would be text)
                                    ;   bit 7 = linear framebuffer available
VBE_MI_PITCH            equ 0x10    ; word  BytesPerScanLine
VBE_MI_XRES             equ 0x12    ; word  XResolution, pixels
VBE_MI_YRES             equ 0x14    ; word  YResolution, pixels
VBE_MI_PLANES           equ 0x18    ; byte  NumberOfPlanes
VBE_MI_BPP              equ 0x19    ; byte  BitsPerPixel
VBE_MI_MEMORY_MODEL     equ 0x1B    ; byte  MemoryModel, 4 = packed pixel,
                                    ;                    6 = direct colour
VBE_MI_RED_MASK         equ 0x1F    ; byte  RedMaskSize
VBE_MI_RED_POS          equ 0x20    ; byte  RedFieldPosition
VBE_MI_GREEN_MASK       equ 0x21    ; byte  GreenMaskSize
VBE_MI_GREEN_POS        equ 0x22    ; byte  GreenFieldPosition
VBE_MI_BLUE_MASK        equ 0x23    ; byte  BlueMaskSize
VBE_MI_BLUE_POS         equ 0x24    ; byte  BlueFieldPosition
VBE_MI_PHYS_BASE        equ 0x28    ; dword PhysBasePtr, VBE 2.0 and later only
VBE_MI_LIN_PITCH        equ 0x32    ; word  LinBytesPerScanLine, VBE 3.0 only
VBE_MI_LIN_RED_MASK     equ 0x36    ; byte  LinRedMaskSize,       VBE 3.0 only
VBE_MI_LIN_RED_POS      equ 0x37    ; byte  LinRedFieldPosition
VBE_MI_LIN_GREEN_MASK   equ 0x38    ; byte  LinGreenMaskSize
VBE_MI_LIN_GREEN_POS    equ 0x39    ; byte  LinGreenFieldPosition
VBE_MI_LIN_BLUE_MASK    equ 0x3A    ; byte  LinBlueMaskSize
VBE_MI_LIN_BLUE_POS     equ 0x3B    ; byte  LinBlueFieldPosition

; Note that in both groups the mask SIZE comes first and the field POSITION
; second, while multiboot wants position first. Swapping them is an easy and
; very confusing mistake, so the copy below is written out channel by channel.

VBE_MODE_LFB            equ 0x4000  ; bit 14 of the mode number, see vbe_setup
VBE_MODE_MASK           equ 0x3FFF  ; bits 0..13 are the mode number proper

VBE_MAX_MODES           equ 128     ; mode list entries we are willing to keep

; ---------------------------------------------------------------------------
; The ceiling, mirrored from src/include/fbcon.h
;
; FBCON_MAX_WIDTH / FBCON_MAX_HEIGHT there are the authority: they size the
; console's shadow buffer, which is statically allocated, and a mode larger
; than that is CLIPPED rather than refused. The console would then live in the
; top left corner of the screen and the rest would keep whatever the BIOS left
; on it -- a boot that looks broken but reports no error anywhere. That is the
; quiet failure fbcon.h warns about, and this file is one of the four places
; that have to agree with it.
;
; The value is restated here rather than derived, and it should be said why:
; NASM's preprocessor cannot read a C header (the comments and declarations
; around those two #defines are not assembly), and this file may not reach
; outside the boot directory for an include. Single-sourcing it would mean
; having the build generate an .inc from fbcon.h, which is the obvious next
; step if a fifth place ever needs the number. Until then the mirror is
; defended by the assertion in VBE_RES below, so a table row that outgrows the
; console breaks the BUILD instead of the display.
;
; %define rather than equ on purpose: %if is a preprocessor directive and
; cannot see an equ, so an assertion written against an equ would silently
; evaluate it as nothing and never fire. Single-line macros are visible to it,
; and are just as usable as an immediate operand further down.
; ---------------------------------------------------------------------------
%define VBE_MAX_WIDTH   1920        ; == FBCON_MAX_WIDTH  in src/include/fbcon.h
%define VBE_MAX_HEIGHT  1080        ; == FBCON_MAX_HEIGHT in src/include/fbcon.h

; ---------------------------------------------------------------------------
; One row of vbe_res_table: width, height, rank.
;
; A macro rather than three raw directives so that every row is checked
; against the ceiling as it is written. %error aborts the assembly with the
; offending line named, which is the loudest failure available here and much
; better than the alternative: a mode that boots, paints, and is wrong.
;
; The row is exactly 5 bytes, which is what vbe_score_mode's "add si, 5"
; assumes. Keep them in step if the layout ever changes.
; ---------------------------------------------------------------------------
%macro VBE_RES 3
  %if (%1) > VBE_MAX_WIDTH || (%2) > VBE_MAX_HEIGHT
    %error "vbe_res_table row exceeds FBCON_MAX_WIDTH/FBCON_MAX_HEIGHT -- the framebuffer console cannot use a mode this large, it would clip"
  %endif
  %if (%3) > 63
    %error "vbe_res_table rank must fit in 6 bits: vbe_score_mode shifts it left by 2 in a byte register"
  %endif
    dw %1, %2
    db %3
%endmacro

; ---------------------------------------------------------------------------
; vbe_setup
;
;   in:  nothing (DS = 0, real mode, interrupts may be on)
;   out: carry clear - a graphics mode is now active. MB_INFO's framebuffer
;                      fields are filled in and MB_FLAG_FRAMEBUFFER is set in
;                      MBI_FLAGS.
;        carry set   - no usable mode was found or the BIOS refused. Nothing
;                      was changed, the display is still in the text mode the
;                      caller was using, and MBI_FLAGS is untouched. If stage 2
;                      wants the kernel to have a framebuffer description in
;                      that case too, it is stage 2's job to fill in the text
;                      mode by hand (0xB8000, 80x25, MB_FB_TYPE_TEXT) and set
;                      MB_FLAG_FRAMEBUFFER itself. This routine never claims a
;                      framebuffer it did not establish.
;   clobbers: the usual real mode scratch registers are all preserved via
;             pusha/pop es, so from the caller's point of view only the flags
;             change. That keeps the call site in stage 2 trivial.
;
; IMPORTANT for whoever writes stage 2: on success the screen is a graphics
; framebuffer, and int 0x10 teletype output no longer produces anything a
; human can see. Any "loading kernel..." messages printed after this call are
; written into a framebuffer nobody is drawing text into, i.e. they vanish.
; A boot that appears to hang right after this point is very probably a
; perfectly healthy boot with no visible output -- print your progress
; messages BEFORE calling vbe_setup, not after.
; ---------------------------------------------------------------------------
vbe_setup:
    pusha
    push es

    ; -----------------------------------------------------------------
    ; 1. Ask for the controller information.
    ;
    ; Stamping 'VBE2' into the signature field before the call is how a
    ; caller tells a VBE 2.0+ BIOS that it has provided the full 512 byte
    ; block and wants the extended information. A VBE 1.x BIOS ignores it.
    ; -----------------------------------------------------------------
    push ds
    pop  es                             ; ES:DI must address our buffer
    mov  dword [vbe_ctrl_info + VBE_CI_SIGNATURE], 'VBE2'
    mov  di, vbe_ctrl_info
    mov  ax, 0x4F00
    push ds                             ; a few video BIOSes return with the
    int  0x10                           ; segment registers altered, and every
    pop  ds                             ; address we use afterwards is DS based

    ; A VBE call succeeded only if the whole of AX is 0x004F: AL=0x4F means
    ; the function is supported, AH=0x00 means it actually worked. Machines
    ; with no VBE at all leave AL alone, and they must still boot -- so this
    ; is a clean failure, not a halt.
    cmp  ax, 0x004F
    jne  .fail
    cmp  dword [vbe_ctrl_info + VBE_CI_SIGNATURE], 'VESA'
    jne  .fail

    ; Remember the version; it decides which fields are authoritative later.
    mov  ax, [vbe_ctrl_info + VBE_CI_VERSION]
    mov  [vbe_version], ax

    ; PhysBasePtr and the "LFB available" attribute bit were both introduced
    ; with VBE 2.0. On a VBE 1.x BIOS there is no linear framebuffer to be
    ; had, only bank switching, which is not what the kernel expects.
    cmp  ax, 0x0200
    jb   .fail

    ; -----------------------------------------------------------------
    ; 2. Copy the mode list somewhere safe.
    ;
    ; VideoModePtr is a real mode FAR pointer, segment in the high word and
    ; offset in the low word, so it needs assembling into a segment:offset
    ; pair rather than being used as a flat address.
    ;
    ; The list is copied rather than walked in place for a reason: on most
    ; VBE 2.0 BIOSes it lives at vbe_ctrl_info+0x100, i.e. inside the very
    ; buffer we just filled, and every 4F01h call we are about to make writes
    ; into a buffer of our own. Keeping our own copy means the list cannot be
    ; pulled out from under the walk no matter where the BIOS put it, and it
    ; also survives a BIOS that maps the list into a segment we later clobber.
    ; -----------------------------------------------------------------
    cmp  dword [vbe_ctrl_info + VBE_CI_VIDEO_MODE_PTR], 0
    je   .fail                          ; null pointer: nothing to walk

    cld
    mov  si, [vbe_ctrl_info + VBE_CI_VIDEO_MODE_OFF]
    mov  ax, [vbe_ctrl_info + VBE_CI_VIDEO_MODE_SEG]
    mov  di, vbe_mode_list
    mov  cx, VBE_MAX_MODES
    push ds
    mov  ds, ax                         ; DS:SI = the BIOS list from here on;
                                        ; our own labels are only valid via ES
.copy_mode:
    lodsw
    stosw
    cmp  ax, 0xFFFF                     ; the list is terminated by 0xFFFF
    je   .copy_done
    loop .copy_mode
    mov  word [es:di - 2], 0xFFFF       ; more modes than we kept: terminate
.copy_done:
    pop  ds

    ; -----------------------------------------------------------------
    ; 3. + 4. Walk the list, score every mode, keep the best.
    ;
    ; Scoring rather than taking the first match matters: the list is in no
    ; useful order, so "first mode that fits" would happily hand the kernel
    ; 640x480x16 on a machine that can do 1024x768x32.
    ; -----------------------------------------------------------------
    mov  word [vbe_best_mode], 0xFFFF
    mov  byte [vbe_best_score], 0
    mov  si, vbe_mode_list

.next_mode:
    lodsw
    cmp  ax, 0xFFFF
    je   .have_best
    and  ax, VBE_MODE_MASK              ; 4F01h wants bits 14/15 clear
    mov  [vbe_cur_mode], ax

    push si                             ; the video BIOS is allowed to trash
                                        ; every register it likes
    push ds
    pop  es                             ; ES:DI = our mode info buffer
    mov  cx, ax
    mov  di, vbe_mode_info
    mov  ax, 0x4F01
    push ds
    int  0x10
    pop  ds
    cmp  ax, 0x004F
    jne  .mode_done                     ; BIOS lists modes it cannot describe

    call vbe_score_mode                 ; AL = score, 0 means unusable
    cmp  al, [vbe_best_score]
    jbe  .mode_done                     ; strictly better wins, so on a tie
                                        ; the earlier mode is kept
    mov  [vbe_best_score], al
    mov  ax, [vbe_cur_mode]
    mov  [vbe_best_mode], ax

.mode_done:
    ; SI is restored here and not straight after the BIOS call, because
    ; vbe_score_mode uses SI to walk its preference tables as well.
    pop  si
    jmp  .next_mode

.have_best:
    cmp  byte [vbe_best_score], 0
    je   .fail                          ; nothing usable: keep text mode

    ; Re-read the winner's mode info, because the walk left the buffer full of
    ; whatever the last mode in the list happened to be.
    push ds
    pop  es
    mov  cx, [vbe_best_mode]
    mov  di, vbe_mode_info
    mov  ax, 0x4F01
    push ds
    int  0x10
    pop  ds
    cmp  ax, 0x004F
    jne  .fail

    ; -----------------------------------------------------------------
    ; 5. Set the mode.
    ;
    ; Bit 14 of the mode number is the whole point of this exercise. With it
    ; clear the BIOS gives us the mode in BANKED form: video memory appears
    ; through a 64 KiB window at 0xA0000 that has to be paged by hand with
    ; 4F05h for every scanline crossing a bank boundary. The kernel assumes a
    ; flat framebuffer at PhysBasePtr, which only exists if bit 14 asked for
    ; the linear framebuffer. Bit 15 stays clear so the BIOS is free to clear
    ; video memory, which conveniently gives us a black screen rather than
    ; whatever was left over.
    ;
    ; From the instruction after this int 0x10 onwards, int 0x10 teletype
    ; output is invisible. See the header comment.
    ; -----------------------------------------------------------------
    mov  bx, [vbe_best_mode]
    or   bx, VBE_MODE_LFB
    mov  ax, 0x4F02
    push ds
    int  0x10
    pop  ds
    cmp  ax, 0x004F
    jne  .fail                          ; a refused 4F02h leaves the old mode

    ; -----------------------------------------------------------------
    ; 6. Describe the result to the kernel in multiboot terms.
    ; -----------------------------------------------------------------

    ; Physical base of the linear framebuffer. PhysBasePtr is a 32-bit
    ; physical address, so the high half of the multiboot field is zero.
    mov  eax, [vbe_mode_info + VBE_MI_PHYS_BASE]
    mov  [MB_INFO + MBI_FB_ADDR_LOW], eax
    xor  eax, eax
    mov  [MB_INFO + MBI_FB_ADDR_HIGH], eax

    ; Pitch. This must come from the hardware and never from width*bpp/8:
    ; adapters routinely pad every row out to a convenient boundary, so a
    ; 1024 pixel wide 32bpp mode can perfectly well have a 4224 byte pitch.
    ; Computing it would put every scanline but the first in the wrong place.
    ;
    ; Which field is authoritative differs between VBE versions, and after a
    ; mode set the safest source is not a field at all:
    ;
    ;   * 4F06h/BL=1 (Get Logical Scan Line Length) reports the length of the
    ;     line currently in force, after the mode set, in BX. That is the one
    ;     answer that cannot be stale, so it is tried first.
    ;   * Failing that, on VBE 3.0 the linear framebuffer has its own pitch in
    ;     LinBytesPerScanLine (0x32); BytesPerScanLine (0x10) describes the
    ;     banked flavour of the same mode and may differ.
    ;   * On VBE 2.0 LinBytesPerScanLine does not exist and BytesPerScanLine
    ;     (0x10) is the only, and correct, answer.
    call vbe_get_pitch                  ; AX = bytes per scanline
    movzx eax, ax
    mov  [MB_INFO + MBI_FB_PITCH], eax

    movzx eax, word [vbe_mode_info + VBE_MI_XRES]
    mov  [MB_INFO + MBI_FB_WIDTH], eax
    movzx eax, word [vbe_mode_info + VBE_MI_YRES]
    mov  [MB_INFO + MBI_FB_HEIGHT], eax

    mov  al, [vbe_mode_info + VBE_MI_BPP]
    mov  [MB_INFO + MBI_FB_BPP], al

    ; Only modes of 16 bpp and up get past the scoring, and at those depths
    ; the pixel is a packed RGB value rather than a palette index whatever the
    ; BIOS calls its memory model. So the multiboot type is always RGB here.
    mov  byte [MB_INFO + MBI_FB_TYPE], MB_FB_TYPE_RGB

    ; Channel layout. VBE 3.0 gives the linear framebuffer its own set of
    ; fields at 0x36..0x3B, because a mode's LFB layout is allowed to differ
    ; from its banked one; earlier versions have only the 0x1F..0x24 set.
    ; Both groups have the same shape -- size, position, per channel -- so SI
    ; just points at whichever group applies.
    mov  si, vbe_mode_info + VBE_MI_RED_MASK
    cmp  word [vbe_version], 0x0300
    jb   .colours
    cmp  byte [vbe_mode_info + VBE_MI_LIN_RED_MASK], 0
    je   .colours                       ; 3.0 BIOS that left them empty
    mov  si, vbe_mode_info + VBE_MI_LIN_RED_MASK
.colours:
    cmp  byte [si], 0                   ; a zero red mask means the BIOS
    je   .synth_colours                 ; described no layout at all

    mov  al, [si + 1]                   ; RedFieldPosition
    mov  [MB_INFO + MBI_FB_RED_POS], al
    mov  al, [si + 0]                   ; RedMaskSize
    mov  [MB_INFO + MBI_FB_RED_SIZE], al
    mov  al, [si + 3]                   ; GreenFieldPosition
    mov  [MB_INFO + MBI_FB_GREEN_POS], al
    mov  al, [si + 2]                   ; GreenMaskSize
    mov  [MB_INFO + MBI_FB_GREEN_SIZE], al
    mov  al, [si + 5]                   ; BlueFieldPosition
    mov  [MB_INFO + MBI_FB_BLUE_POS], al
    mov  al, [si + 4]                   ; BlueMaskSize
    mov  [MB_INFO + MBI_FB_BLUE_SIZE], al
    jmp  .flags

.synth_colours:
    ; Some BIOSes leave the direct colour fields blank for modes they report
    ; as packed pixel. Rather than hand the kernel a black-on-black palette,
    ; fall back to the layout that is universal at each depth: 5:6:5 for 16
    ; bpp, 8:8:8 in the low three bytes for 24 and 32 bpp.
    mov  si, vbe_rgb888
    cmp  byte [vbe_mode_info + VBE_MI_BPP], 16
    jne  .synth_copy
    mov  si, vbe_rgb565
.synth_copy:
    mov  di, MB_INFO + MBI_FB_RED_POS
    push ds
    pop  es
    mov  cx, 6                          ; pos+size for red, green and blue
    cld
    rep  movsb

.flags:
    or   dword [MB_INFO + MBI_FLAGS], MB_FLAG_FRAMEBUFFER
    clc
    jmp  .done

.fail:
    stc
.done:
    ; Neither POP nor POPA touches the flags, so the carry set above survives
    ; the unwinding and is what the caller sees.
    pop  es
    popa
    ret

; ---------------------------------------------------------------------------
; vbe_get_pitch
;
;   in:  vbe_mode_info holds the mode info of the mode that is now set,
;        vbe_version holds the VBE version
;   out: AX = bytes per scanline
;   clobbers: AX, BX, CX, DX
; ---------------------------------------------------------------------------
vbe_get_pitch:
    push si
    mov  ax, 0x4F06
    mov  bl, 0x01                       ; 0x01 = get current scan line length
    push ds
    int  0x10
    pop  ds
    cmp  ax, 0x004F
    jne  .from_mode_info
    test bx, bx                         ; BX = bytes per scan line
    jz   .from_mode_info
    mov  ax, bx
    pop  si
    ret

.from_mode_info:
    mov  ax, [vbe_mode_info + VBE_MI_PITCH]         ; VBE 1.x/2.0 field
    cmp  word [vbe_version], 0x0300
    jb   .done
    mov  bx, [vbe_mode_info + VBE_MI_LIN_PITCH]     ; VBE 3.0 linear field
    test bx, bx
    jz   .done                          ; 3.0 BIOS that left it at zero
    mov  ax, bx
.done:
    pop  si
    ret

; ---------------------------------------------------------------------------
; vbe_score_mode
;
;   in:  vbe_mode_info filled in by a successful 4F01h
;   out: AL = score, 0 meaning the mode is unusable. Higher is better.
;   clobbers: AX, BX, CX, DX, SI
;
; The score is  resolution_rank * 4 + depth_rank, with both ranks taken from
; the tables at the bottom of this file and both at least 1 for an acceptable
; mode. Multiplying the resolution rank by 4 makes resolution dominate depth
; outright: 1024x768x16 (13) beats 800x600x32 (11), which is the right way
; round for a console and a windowing system alike.
; ---------------------------------------------------------------------------
vbe_score_mode:
    ; ModeAttributes bits 0 (supported), 4 (graphics, so not a text mode) and
    ; 7 (linear framebuffer available) must all be set. Bit 7 is the important
    ; one: without it 4F02h with bit 14 would fail or, worse, silently give us
    ; a banked mode.
    mov  ax, [vbe_mode_info + VBE_MI_ATTRIBUTES]
    and  al, 0x91
    cmp  al, 0x91
    jne  .unusable

    ; MemoryModel: 4 = packed pixel, 6 = direct colour. Everything else is
    ; planar, text, or a legacy CGA/Hercules arrangement the kernel cannot
    ; treat as a flat array of pixels.
    mov  al, [vbe_mode_info + VBE_MI_MEMORY_MODEL]
    cmp  al, 4
    je   .model_ok
    cmp  al, 6
    jne  .unusable
.model_ok:

    ; A single plane, i.e. not one of the planar VGA arrangements.
    cmp  byte [vbe_mode_info + VBE_MI_PLANES], 1
    jne  .unusable

    ; The attribute bit says a linear framebuffer exists; this checks that the
    ; BIOS actually told us where it is.
    cmp  dword [vbe_mode_info + VBE_MI_PHYS_BASE], 0
    je   .unusable

    ; Depth.
    mov  al, [vbe_mode_info + VBE_MI_BPP]
    mov  si, vbe_depth_table
.depth_loop:
    mov  cl, [si]
    test cl, cl
    jz   .unusable                      ; depth not one we accept
    cmp  cl, al
    je   .depth_found
    add  si, 2
    jmp  .depth_loop
.depth_found:
    mov  bh, [si + 1]                   ; BH = depth rank

    ; Resolution.
    mov  cx, [vbe_mode_info + VBE_MI_XRES]
    mov  dx, [vbe_mode_info + VBE_MI_YRES]

    ; The ceiling, enforced here as well as in the table.
    ;
    ; This is redundant on purpose. VBE_RES already refuses to assemble a row
    ; larger than FBCON_MAX_*, so no oversized mode can win by way of the
    ; table -- but the table is a whitelist maintained by hand, and the cost of
    ; not trusting it is four instructions on a path that runs a few dozen
    ; times during one boot. What it buys is that the invariant the kernel
    ; actually depends on ("the mode handed over is never larger than the
    ; console's shadow buffer") is checked against the mode the CARD reported,
    ; at the moment of the decision, rather than being inferred from the fact
    ; that somebody built the table correctly. A row added with raw dw/db
    ; directives, bypassing the macro, is caught here.
    cmp  cx, VBE_MAX_WIDTH
    ja   .unusable
    cmp  dx, VBE_MAX_HEIGHT
    ja   .unusable

    mov  si, vbe_res_table
.res_loop:
    mov  ax, [si]
    test ax, ax
    jz   .unusable                      ; resolution not one we want
    cmp  ax, cx
    jne  .res_next
    cmp  [si + 2], dx
    je   .res_found
.res_next:
    add  si, 5
    jmp  .res_loop
.res_found:
    mov  bl, [si + 4]                   ; BL = resolution rank
    shl  bl, 2
    add  bl, bh
    mov  al, bl
    ret

.unusable:
    xor  al, al
    ret

; ---------------------------------------------------------------------------
; Preference tables. Both are ordered by descending rank purely for reading
; convenience; only the rank byte decides anything, so extending either is a
; matter of adding a row.
; ---------------------------------------------------------------------------

; width, height, rank.
;
; What the rank means, now that there are six of them: it is an ORDER, not a
; measurement. Only "greater than" is ever asked of it (vbe_score_mode keeps
; the strictly better mode), so the numbers could be any ascending sequence;
; 1..6 is simply the shortest one that reads clearly. What the ordering says
; is "if the card offers both of these, take this one".
;
; The order here is by pixel count, descending. That is not the only defensible
; order, and it is worth saying exactly where it is wrong, because the table
; has NO notion of aspect ratio and "highest resolution" is not the same
; question as "best picture".
;
; The case that matters is a 16:9 panel whose card cannot do 1920x1080. Left
; to the classic list, the next mode down by pixel count is 1280x1024, a 5:4
; mode that a widescreen monitor either stretches or letterboxes -- worse to
; look at than a smaller mode of the right shape. 1600x900 is in the table for
; precisely that case, and it works because its pixel count (1440000) lands
; above 1280x1024's (1310720): the pixel-count order puts it where the
; aspect-ratio argument wants it anyway, so one rank column serves both.
;
; That is a repair, not a solution, and the remaining hole should be named: a
; card that offers 1280x1024 but no 16:9 mode at all still hands a widescreen
; monitor a stretched 5:4 picture, and this table cannot tell. Encoding the
; monitor's shape is not something a mode list can do -- the ratio the USER is
; looking at is not in the VBE information at all (it would have to come from
; the EDID, via int 0x10 AX=4F15h, which is a different feature and a
; different conversation). If it ever does, the honest fix is a fourth column
; here plus a term for it in the score, not a re-ordering of ranks: one rank
; can encode one preference, and any single order has to be wrong for one
; class of monitor or the other.
;
; The two ends of the table are the interesting ones:
;
;   1920x1080 is exactly FBCON_MAX_* and therefore the largest mode that can
;   ever be selected. It is NOT a standard VESA mode -- the VESA numbering
;   stops at 1600x1200 (modes 0x11E..0x120) -- and is reached through the
;   Bochs/VBE display extensions that QEMU's std VGA, virtio, VMware SVGA and
;   most real cards of the last twenty years implement. Asking for it costs
;   nothing where it is absent, and that is worth being precise about: the
;   walk in vbe_setup enumerates whatever VideoModePtr lists and matches on
;   the XResolution/YResolution fields a 4F01h call returns -- never on a mode
;   NUMBER. A size the card does not offer therefore never appears in the
;   list, is never scored, and is never handed to 4F02h; the only cost of its
;   row is the handful of instructions .res_loop spends walking past it, for
;   modes that end up rejected anyway. There is no failed mode set, no BIOS
;   probe and no delay -- which is what lets the table be optimistic.
;
;   640x480 stays the floor for the same reason it always was: it is the one
;   mode a VBE 2.0 card can be relied on to have, and landing there is still
;   enormously better than falling back to text.
;
; 1600x1200 is deliberately NOT here, although it IS a standard VESA mode and
; would otherwise have earned a row between 1920x1080 and 1600x900. Its height exceeds
; FBCON_MAX_HEIGHT (1200 > 1080), so the console would size itself to 67 rows
; on a screen with room for 75 and leave the bottom 128 pixel lines untouched
; for the rest of the session. The VBE_RES macro refuses to assemble it, which
; is how this was found rather than remembered. It can be added the day
; fbcon.h raises FBCON_MAX_HEIGHT to 1200 -- and not one day earlier.
;
; Ranks must be DISTINCT. vbe_score_mode keeps the strictly better mode, so
; two rows sharing a rank do not mean "either will do": they mean the winner
; is whichever the card happens to list first, which is a decision made by the
; BIOS rather than by this table.
vbe_res_table:
    VBE_RES 1920, 1080, 6               ; 16:9, == FBCON_MAX_*, Bochs/VBE ext
    VBE_RES 1600,  900, 5               ; 16:9, Bochs/VBE ext
    VBE_RES 1280, 1024, 4               ; 5:4,  standard VESA (0x11A/0x11B)
    VBE_RES 1024,  768, 3               ; 4:3,  standard VESA (0x117/0x118)
    VBE_RES  800,  600, 2               ; 4:3,  standard VESA (0x114/0x115)
    VBE_RES  640,  480, 1               ; 4:3,  the floor
    dw 0                                ; terminator

; bits per pixel, rank. 32 first because a four byte pixel means the kernel
; can write a whole pixel with one aligned store; 24 costs unaligned three
; byte writes but keeps full colour; 16 is the last resort. 8 bpp is
; deliberately absent -- it would need palette programming and is no better
; than the 320x200 mode the kernel already had.
vbe_depth_table:
    db 32, 3
    db 24, 2
    db 16, 1
    db 0                                ; terminator

; Fallback channel layouts, in multiboot's order: position then size, for
; red, green and blue.
vbe_rgb565:
    db 11, 5
    db  5, 6
    db  0, 5
vbe_rgb888:
    db 16, 8
    db  8, 8
    db  0, 8

; ---------------------------------------------------------------------------
; Buffers and state.
;
; A flat binary has no .bss, so every byte here is a byte of zeros in the
; stage 2 image and a byte of RAM immediately after stage 2's code, i.e. at
; STAGE2_ORG + (however far into stage 2 this file was included). That is
; comfortably below 1 MiB, which is all the BIOS requires of an ES:DI buffer.
;
; Total cost is 1032 bytes. Worth checking against layout.inc: DISK_BUFFER
; sits at 0x9000, only 0x1000 bytes above STAGE2_ORG, so stage 2's code plus
; these buffers has to stay under 4 KiB or the two will overlap. Should stage
; 2 ever grow past that, move these buffers to fixed addresses in the free
; 0x0500..0x6FFF region rather than letting them drift into DISK_BUFFER.
; ---------------------------------------------------------------------------
vbe_ctrl_info:      times 512 db 0      ; VbeInfoBlock, must be 512 for 'VBE2'
vbe_mode_info:      times 256 db 0      ; ModeInfoBlock, VBE 3.0 wants 256
vbe_mode_list:      times (VBE_MAX_MODES * 2) db 0  ; our copy of the mode list
vbe_version:        dw 0                ; BCD, e.g. 0x0300
vbe_cur_mode:       dw 0                ; mode currently under examination
vbe_best_mode:      dw 0xFFFF           ; best mode found, 0xFFFF = none
vbe_best_score:     db 0                ; its score, 0 = nothing usable yet
vbe_pad:            db 0                ; keeps what follows word aligned

%endif ; TOMATOS_VBE_INC
