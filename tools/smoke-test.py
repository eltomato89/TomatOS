#!/usr/bin/env python3
"""Boot build/tomatos_boot.img in a headless QEMU and check that it came up.

WHY THIS EXISTS
---------------
The GitHub workflow in .github/workflows/build.yml exists so that a machine
without a Linux toolchain -- a Mac running UTM -- can get a bootable TomatOS
image out of a push. An image that was built but never booted is a guess: the
compiler was happy, the linker was happy, mtools wrote 32 MB of something, and
nobody has any evidence that a BIOS jumping to LBA 0 of it ends up at a shell
prompt. Everything this project can get wrong that a compiler cannot see lives
below that line -- a stage 2 that outgrew its sectors, a module table written
at the wrong offset, a kernel whose .bss now overlaps where the modules get
loaded, a filesystem whose reserved sector count no longer matches the boot
chain. All of those produce a clean build and a dead machine.

So the image is booted here, in the same configuration the owner boots it in,
and the boot is read back and asserted on. The workflow only publishes an
artefact after this has passed.

HOW THE OUTPUT IS READ BACK
---------------------------
Through COM1. src/video/scrn.c mirrors every character that reaches putch()
onto the serial port, so "-serial file:..." hands us the whole boot as plain
text -- and putch() is the single funnel that printf(), puts(), the keyboard
echo and the SYS_WRITE syscall all pass through, so the log is the screen.

Reading the VGA text buffer at 0xB8000 through the monitor instead would not
work for this image and it is worth writing down why, because it is the
obvious thing to reach for: the boot chain in boot/ sets a VBE graphics mode
before it enters the kernel, the console is then a framebuffer console, and
0xB8000 is a buffer nothing writes to any more. It holds whatever the BIOS
left there. The "-kernel" path does stay in EGA text mode and would be
readable that way, but that path is not what boots on a real machine or in
UTM, so it is not what is worth testing.

HOW THE SHELL IS DRIVEN
-----------------------
Through the QEMU monitor's "sendkey", i.e. by pressing keys on the emulated
PS/2 keyboard, because that is the only input the kernel has: the serial port
is output-only as far as the console is concerned, and nothing reads from it.
See KEYMAP below for the two things that makes surprising.

USAGE
-----
    tools/smoke-test.py                     # after "make bootdisk"
    tools/smoke-test.py --timeout 300       # a slow machine, or a slow runner
    tools/smoke-test.py --keep              # leave the QEMU process' log dir
    tools/smoke-test.py --image other.img

The exit status is 0 if every assertion held and 1 otherwise, and on failure
the captured boot log is printed in full. CI runs exactly this command, which
is the point of it being a file rather than a "run:" block in the workflow --
a test that only exists inside a YAML file is one nobody runs before pushing.
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time


# --- what the image has to say for itself ----------------------------------
#
# Five things, chosen because each one is a different way for a build to be
# broken while still compiling and linking without a word of complaint.

# 1. The kernel banner. Printed by kernel.c immediately after init_video(),
#    which is the first moment there is a console at all. Reaching this line
#    means stage 1 loaded stage 2, stage 2 loaded the kernel off the disk and
#    entered it with a Multiboot register contract the kernel accepted, and
#    the framebuffer the loader set up is one the console can drive. Almost
#    every way of breaking the boot chain stops before this.
BANNER = "TomatOS/x86 boot v0.2"

# 2. The module line, "Modules: 7 (hello, ls, cat, fetch, rm, cp, gui)".
#    The programs are not on the filesystem at boot; they are written into the
#    image's reserved sectors and handed to the kernel as Multiboot modules by
#    our own stage 2, through a table at a hard-coded offset inside stage2.bin
#    that the Makefile patches after assembling it. If that table's offset, its
#    entry size or its count ever drift apart from what stage2.asm reads, this
#    line is where it shows -- as a wrong count, as garbage names, or as
#    nothing at all. Checked against the Makefile's own list, see
#    expected_modules().
MODULE_LINE = re.compile(r"^Modules: (\d+) \(([^)\n]*)\)", re.M)

# 3. The filesystem. The boot chain and the FAT16 volume share one disk: the
#    boot chain lives in the volume's reserved sectors, which only works while
#    the reserved sector count in the BPB and the sector map in boot/layout.inc
#    agree. When they stop agreeing, the machine still boots -- stage 1 does
#    not read the BPB to find stage 2 -- and the kernel then mounts either
#    nothing or something with the boot chain inside its data area. kernel.c
#    prints "Filesystem: none (...)" for the first case, so the check is not
#    just "the word Filesystem appeared".
FILESYSTEM_LINE = re.compile(r"^Filesystem: (FAT\d+) on ([^,\s]+)", re.M)

# 4. The shell's own banner, from main() in src/kernel/main.c. Between the
#    kernel banner and this one lie the GDT, the IDT, the syscall gate, the
#    scheduler and the handover in taskmgr_boot_complete(); this line is
#    printed by a task, so it says the scheduler actually switched into one.
SHELL_BANNER = "eltomato's TomatOS 0.31"

# 5. The prompt. The shell is at scan(), waiting for the keyboard, which means
#    the keyboard IRQ arrived and the console task is not spinning somewhere.
#    It is also what we have to see before it is any use pressing keys.
PROMPT = "@TomatOS>"

# And the two things that mean there is no point waiting any longer. Both come
# from the fault handler in src/kernel/isrs.c: the first is the banner over its
# ASCII tomato, the second is what it prints just before it halts, and the
# scheduler prints the same words when it finds nothing left to run.
#
# Without this the test still fails -- it would simply sit out its whole
# timeout first, because a halted CPU produces no further output and looks
# exactly like a slow one. Four minutes of a runner and, worse, four minutes
# before anybody reading the log finds out that the answer was on screen the
# entire time.
PANIC_MARKERS = ("The kernel made a boo-boo", "CPU HALT")


# --- and what it has to be able to DO --------------------------------------
#
# A boot that reaches a prompt but cannot run a program is still a build worth
# failing, and it is a failure the five checks above cannot see: the module
# table can be perfect and the ELF loader still broken. So two commands are
# typed at the prompt and their output is asserted on.
#
# Both are words the shell does not know, so both go through run_program(),
# which resolves them as /BIN/NAME.ELF on the mounted volume. That is the path
# a person actually uses, and it drags the whole stack behind it: read the
# directory, read the file, parse an ET_EXEC, build an address space, map the
# segments, start a ring 3 task, and carry its output back through SYS_WRITE.
#
# "ls" is the one that would notice a volume that mounted but cannot be read:
# it walks a directory and prints what is in it. "hello" is the more decisive
# one about the loader -- it prints, it sleeps, and it returns an exit status
# that has to travel from a "return" in main() through _start() and sys_exit()
# back to the shell, so asserting on the number checks the way out of a
# program as well as the way in.
#
# The other half of the picture, the programs carried in the image's reserved
# sectors as Multiboot modules, is covered by the module line above rather than
# by running one: the Makefile already verifies those bytes against build/*.elf
# when it writes the image, and the kernel printing their names back proves
# stage 2 delivered the table.
#
# Both are matched against the LAST thing typed, not against the whole log, so
# a string that happened to appear during boot cannot satisfy a command check.
COMMANDS = [
    (
        "ls",
        [
            # The header ls prints before the listing, with the path it was
            # given -- so this is also the proof that argv arrived.
            "Directory of /",
            # The directory the programs live in, put there from sysroot/.
            # Upper case 8.3 on purpose, and that is the point of asserting on
            # it: mtools writes a VFAT long name entry for anything that is not
            # valid 8.3, and the kernel's directory reader skips those. A name
            # that stopped being 8.3 would still be copied onto the image, take
            # up clusters, and be invisible to every program that lists a
            # directory -- a failure with no other symptom.
            "BIN",
        ],
    ),
    (
        "hello",
        [
            "Hello from user space!",
            # HELLO_EXIT_STATUS in user/hello/hello.c. Asserting the number
            # rather than just the word makes this a check on the exit path:
            # the value travels from a "return" in main(), through _start() in
            # user/lib/lib.c, through sys_exit(), to the shell.
            "Goodbye - exiting with status 42.",
        ],
    ),
]


# --- the keyboard ----------------------------------------------------------
#
# "sendkey" presses a key by its position on a US keyboard, and the kernel's
# keymap in src/drivers/input/kb.c is German -- so the character the guest sees
# is the one printed on a German key in that position, not the one QEMU's key
# name suggests. Two consequences, both of which have cost time before:
#
#   "-"  is where a US board has "/",  so it is  sendkey slash
#        ("sendkey minus" lands on the German sharp s instead)
#   "/"  is Shift+7 on a German board, so it is  sendkey shift-7
#
# and because QWERTZ swaps two letters, "y" and "z" have to be swapped back.
# There is no upper case: sendkey has no notion of a shifted letter, it would
# have to be spelled "shift-a", so the commands above are lower case only.
#
# Neither command this file types needs any of that today. The table is here
# because the next command somebody adds might, and finding this out from the
# symptom -- a shell that answers "Unknown command: sllash" -- is a bad
# afternoon.
KEYMAP = {" ": "spc", ".": "dot", "-": "slash", "/": "shift-7", "y": "z", "z": "y"}


def key_for(char):
    """The sendkey name for one character, or None if we cannot type it."""
    if char in KEYMAP:
        return KEYMAP[char]
    if char.isdigit():
        return char
    if char.isascii() and char.isalpha() and char.islower():
        return char
    return None


def expected_modules(repo_root):
    """The programs the image should carry, read out of the Makefile.

    Hard-coding the list here would mean that adding an eighth program to
    USER_PROGS turns this test red for no reason and in a place nobody would
    think to look. Reading it out of the Makefile keeps the one list the one
    list -- and if the parse ever fails, we say so and fall back rather than
    silently checking nothing.
    """
    makefile = os.path.join(repo_root, "Makefile")
    try:
        with open(makefile, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = re.match(r"^USER_PROGS\s*:?=\s*(.*)$", line)
                if match:
                    names = match.group(1).split()
                    if names:
                        return names
    except OSError:
        pass
    print("  note: could not read USER_PROGS out of %s, falling back to the "
          "list built into this script" % makefile)
    return ["hello", "ls", "cat", "fetch", "rm", "cp", "gui"]


class Monitor(object):
    """The QEMU monitor on a Unix socket.

    A Unix socket rather than a TCP port because two of these may well run at
    once -- a workflow with a matrix, or somebody running this locally while
    CI runs it too -- and a fixed port number would have them fight over it
    while a private socket cannot collide with anything.
    """

    def __init__(self, path, deadline, qemu):
        self.sock = None
        while time.time() < deadline:
            if qemu.poll() is not None:
                raise RuntimeError(
                    "QEMU exited with status %d before opening its monitor "
                    "socket. What it said:\n%s"
                    % (qemu.returncode, qemu.communicate()[0].decode(
                        "utf-8", errors="replace").rstrip()))
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                self.sock = None
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError("the QEMU monitor socket at %s never accepted a "
                               "connection -- did QEMU start at all?" % path)
        # Every recv() has a timeout, so no read here can wedge the test. We
        # never parse the monitor's answers: the assertions all come from the
        # serial log, and the monitor is only used to press keys and to take a
        # picture. Draining is purely so the socket buffer cannot fill up.
        self.sock.settimeout(0.5)

    def command(self, text, settle=0.05):
        self.sock.sendall((text + "\n").encode("ascii"))
        time.sleep(settle)
        try:
            self.sock.recv(65536)
        except OSError:
            pass

    def type_line(self, text):
        """Type one line at the guest's prompt and press Return."""
        for char in text:
            name = key_for(char)
            if name is None:
                raise RuntimeError(
                    "cannot type %r with sendkey -- see KEYMAP in %s"
                    % (char, os.path.basename(__file__)))
            # A key press is a scancode pair with a hold in between, and the
            # keyboard IRQ has to be taken for each one. Typing flat out drops
            # characters on a TCG guest that is also still doing its own work,
            # and a dropped character shows up as a mystifying "Unknown
            # command" rather than as anything resembling a timing problem.
            self.command("sendkey " + name, settle=0.08)
        self.command("sendkey ret", settle=0.08)

    def screendump(self, path):
        self.command("screendump " + path, settle=1.0)

    def close(self):
        if self.sock is not None:
            try:
                self.command("quit", settle=0.2)
            except OSError:
                pass
            self.sock.close()
            self.sock = None


def read_log(path):
    """The serial log so far, as text with the CRs taken back out.

    write_serial() sends CR LF for a newline because that is what a terminal on
    the other end of a real serial line expects. Nothing here wants to see them
    and a stray CR in the middle of a match would be a maddening way to fail.
    """
    try:
        with open(path, "rb") as handle:
            return handle.read().decode("utf-8", errors="replace").replace("\r", "")
    except OSError:
        return ""


def wait_for(log_path, needle, deadline, qemu, since=0):
    """Poll the serial log until `needle` appears in it, and say what happened.

    Returns (outcome, log text), where outcome is one of:

        "ok"       the needle turned up
        "panic"    the kernel died and said so
        "gone"     the QEMU process exited
        "timeout"  the deadline passed with the guest still running

    `since` is an offset into the log: everything before it is ignored for the
    purpose of matching, which is how a command's output is told apart from
    text that happened to appear during boot.

    All four outcomes are given names rather than being folded into a bare
    True/False because the caller has something different and more useful to
    say about each of them, and "it did not work" is the least helpful thing a
    failing test can print.
    """
    while True:
        text = read_log(log_path)
        if needle in text[since:]:
            return "ok", text
        for marker in PANIC_MARKERS:
            if marker in text[since:]:
                return "panic", text
        if qemu.poll() is not None:
            # A guest that triple faults exits, because we start QEMU with
            # -no-reboot. Sitting out the rest of the timeout waiting for
            # output from a process that is gone buys nothing at all.
            return "gone", text
        if time.time() >= deadline:
            return "timeout", text
        time.sleep(0.25)


def fail(message, log_text, shot=None):
    """Report a failure with the evidence attached, and exit non-zero.

    The log is printed in full and not summarised. A red X with no output is
    useless to whoever reads it: the interesting part of a boot failure is
    almost always the last line before it stopped, and which line that is
    cannot be known in advance.
    """
    print("")
    print("SMOKE TEST FAILED: %s" % message)
    print("")
    print("--- captured boot log (%d bytes) %s" % (len(log_text), "-" * 30))
    if log_text.strip():
        for number, line in enumerate(log_text.rstrip("\n").split("\n"), 1):
            print("%4d | %s" % (number, line))
    else:
        print("     | (nothing at all came out of COM1)")
        print("     |")
        print("     | Either the guest never got far enough to print anything --")
        print("     | the line above this block says which -- or the console is")
        print("     | no longer being mirrored to the serial port. For the")
        print("     | second: putch() in src/video/scrn.c has to call")
        print("     | serial_console_putc(), which is the only way this test")
        print("     | can see what the guest is doing.")
    print("--- end of boot log " + "-" * 46)
    if shot:
        print("")
        print("A screenshot of the guest at the moment of failure: %s" % shot)
    sys.exit(1)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    parser = argparse.ArgumentParser(
        description="Boot the TomatOS disk image headless and assert that it "
                    "comes up, mounts its filesystem and can run a program.")
    parser.add_argument(
        "--image", default=os.path.join(repo_root, "build", "tomatos_boot.img"),
        help="the disk image to boot (default: build/tomatos_boot.img)")
    parser.add_argument(
        "--qemu", default="qemu-system-x86_64",
        help="the QEMU binary (default: qemu-system-x86_64, the one UTM uses)")
    parser.add_argument(
        "--outdir", default=None,
        help="where to write the boot log and any screenshot "
             "(default: <image dir>/smoke-test)")
    parser.add_argument(
        "--timeout", type=float, default=240.0,
        help="give up after this many seconds in total (default: 240). This is "
             "a ceiling, not a wait: everything below polls and finishes as "
             "soon as it can.")
    parser.add_argument(
        "--keep", action="store_true",
        help="print the boot log even when everything passed")
    args = parser.parse_args()

    if not os.path.isfile(args.image):
        print("smoke test: no image at %s -- run \"make bootdisk\" first."
              % args.image)
        return 1

    outdir = args.outdir or os.path.join(os.path.dirname(os.path.abspath(args.image)),
                                         "smoke-test")
    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    log_path = os.path.join(outdir, "boot.log")
    shot_path = os.path.join(outdir, "screen.ppm")
    for stale in (log_path, shot_path):
        if os.path.exists(stale):
            os.unlink(stale)

    # The monitor socket does NOT go next to the log, and that is not a taste
    # decision. A Unix socket address is a fixed 108 byte field in the kernel,
    # path included, and bind() fails outright above it -- so a checkout under
    # a deep enough directory would make this test unrunnable for a reason
    # having nothing whatsoever to do with the kernel being tested. A private
    # directory under $TMPDIR keeps the path short wherever the repository
    # happens to live; it is removed again in the finally block below.
    sockdir = tempfile.mkdtemp(prefix="tomatos-smoke-")
    monitor_path = os.path.join(sockdir, "mon")

    # The first six arguments are, deliberately and exactly, the ones the owner
    # boots this image with in UTM on a Mac. A smoke test that passed under a
    # different machine type, a different accelerator or a different amount of
    # memory would be testing a configuration nobody runs. -accel tcg is not a
    # concession to CI either: a runner has no KVM, and neither does UTM when
    # it emulates x86 on Apple silicon, so plain interpretation is the real
    # target rather than a slow substitute for one.
    #
    # What follows them is the test harness and nothing else:
    #
    #   -serial file:      the console, mirrored out of COM1. The whole reason
    #                      this test can assert on text instead of pixels.
    #   -monitor unix:     the keyboard. Also the camera, on failure.
    #   -no-reboot         a triple fault ends the process instead of starting
    #                      the boot again. Without it a kernel that dies early
    #                      loops, the log fills with identical copies of the
    #                      same half boot, and the failure looks like a hang.
    argv = [
        args.qemu,
        "-machine", "pc",
        "-accel", "tcg",
        "-drive", "file=%s,format=raw,index=0,media=disk" % os.path.abspath(args.image),
        "-m", "64",
        "-display", "none",
        "-serial", "file:%s" % log_path,
        "-monitor", "unix:%s,server=on,wait=off" % monitor_path,
        "-no-reboot",
    ]

    # The whole command line is printed, not summarised: when this fails on a
    # runner the first question is always "what exactly was it running", and
    # the answer should be copy-pasteable into a local terminal.
    print("smoke test: %s (%.1f MB)"
          % (args.image, os.path.getsize(args.image) / (1024.0 * 1024.0)))
    print("  qemu    : %s" % " ".join(argv))
    print("  log     : %s" % log_path)

    started = time.time()
    deadline = started + args.timeout

    qemu = subprocess.Popen(argv, cwd=outdir,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    monitor = None
    try:
        monitor = Monitor(monitor_path, min(deadline, started + 30.0), qemu)

        def expect(what, needle, since=0):
            """Wait for one string, or fail saying which one and why.

            The failure text is assembled here so that all four ways of not
            seeing a string read the same and each says the one useful thing
            about itself. A screenshot is taken only when the guest is still
            alive to be photographed, which rules out the "gone" case; on a
            graphics-mode boot it is the only view of the screen there is,
            because the framebuffer never touches the text buffer.
            """
            outcome, log_text = wait_for(log_path, needle, deadline, qemu, since)
            if outcome == "ok":
                return log_text
            if outcome == "gone":
                reason = ("QEMU exited with status %d before %s"
                          % (qemu.returncode, what))
            else:
                if outcome == "panic":
                    reason = "the kernel panicked before %s" % what
                else:
                    reason = ("timed out after %.0f s waiting for %s (%r)"
                              % (time.time() - started, what, needle))
                monitor.screendump(shot_path)
            fail(reason, log_text, shot_path if os.path.exists(shot_path) else None)

        # --- the boot ---------------------------------------------------
        for label, needle in (("kernel banner", BANNER),
                              ("shell banner", SHELL_BANNER),
                              ("shell prompt", PROMPT)):
            expect("the %s" % label, needle)
            print("  ok      : %-14s %.1f s" % (label, time.time() - started))

        text = read_log(log_path)

        # --- the module table -------------------------------------------
        wanted = expected_modules(repo_root)
        match = MODULE_LINE.search(text)
        if match is None:
            fail("no \"Modules: N (...)\" line in the boot output -- stage 2 "
                 "handed the kernel no modules at all, so nothing from the "
                 "reserved sectors made it into memory", text)
        count = int(match.group(1))
        names = [name.strip() for name in match.group(2).split(",") if name.strip()]
        if count != len(wanted) or names != wanted:
            fail("the module table is not what the Makefile builds: the image "
                 "reports %d (%s), USER_PROGS is %d (%s)"
                 % (count, ", ".join(names), len(wanted), ", ".join(wanted)), text)
        print("  ok      : %-14s %d (%s)" % ("modules", count, ", ".join(names)))

        # --- the filesystem ---------------------------------------------
        match = FILESYSTEM_LINE.search(text)
        if match is None:
            fail("nothing was mounted: kernel.c printed no \"Filesystem: FAT.. "
                 "on ..\" line, so either no drive answered or the volume in "
                 "the image is not one the FAT driver recognises", text)
        print("  ok      : %-14s %s on %s"
              % ("filesystem", match.group(1), match.group(2)))

        # --- and now make it do something -------------------------------
        for command, expectations in COMMANDS:
            # Everything already in the log was printed before this command was
            # typed and must not be allowed to satisfy it, so each expectation
            # is matched only against what comes after this mark.
            mark = len(text)
            monitor.type_line(command)
            for needle in expectations:
                text = expect(
                    "the output of \"%s\" at the prompt -- the system boots "
                    "but cannot run a program" % command, needle, since=mark)

            # And then wait for the prompt to come back before typing the next
            # one. Not politeness: a program's last line of output is printed
            # while it is still running, so without this the next command is
            # typed into a shell that is not at scan() and the characters sit
            # in the keyboard buffer while another task is on the CPU. That is
            # a legitimate thing for a person to do and the kernel is supposed
            # to survive it -- but it is not what this test is trying to find
            # out, and a test that quietly depends on a race is one that will
            # eventually fail for a reason that has nothing to do with the
            # commit that turned it red.
            text = expect("the prompt to come back after \"%s\"" % command,
                          PROMPT, since=mark)
            print("  ok      : %-14s %.1f s" % ("ran \"%s\"" % command,
                                                time.time() - started))

        print("")
        print("smoke test PASSED in %.1f s -- %s booted, mounted its "
              "filesystem and ran %s."
              % (time.time() - started, os.path.basename(args.image),
                 " and ".join('"%s"' % name for name, _ in COMMANDS)))
        if args.keep:
            print("")
            print(read_log(log_path))
        return 0

    except RuntimeError as problem:
        # Something went wrong with the harness rather than with the guest --
        # QEMU refused to start, the monitor never opened, a command contains a
        # character sendkey cannot type. Reported the same way as any other
        # failure, and with the log attached, because a Python traceback in the
        # middle of a CI log is a worse way to say the same thing.
        fail(str(problem), read_log(log_path))

    finally:
        # Nothing below may raise, and nothing below may block: this is the
        # only path on which the QEMU process is guaranteed to be reaped, and
        # a smoke test that leaves a guest running on a CI runner is worse
        # than one that fails.
        if monitor is not None:
            try:
                monitor.close()
            except OSError:
                pass
        if qemu.poll() is None:
            qemu.terminate()
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()
                try:
                    qemu.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        shutil.rmtree(sockdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
