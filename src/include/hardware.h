extern int detect_cpu(void);
extern int do_intel(void);
extern void printregs(int eax, int ebx, int ecx, int edx);
extern int do_amd(void);
extern void init_serial();
extern int serial_received();
extern char read_serial();
extern int is_transmit_empty();
extern void write_serial(char a);
extern void writes_serial(char*);
/* One character of console output onto COM1: newline translated to CR LF,
*  non-ASCII reduced, and a bounded wait for the transmitter. Called by
*  putch() in src/video/scrn.c for every character that reaches the screen. */
extern void serial_console_putc(unsigned char c);
extern void setup_pit();
extern void audio_switch_on();
extern void audio_switch_off();
extern void audio_sound(unsigned frequency);
extern void audio_beep(unsigned frequency, int duration);
