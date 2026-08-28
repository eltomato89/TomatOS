
#include <system.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <asm.h>
#include <math.h>
#include <mm.h>
//#include <wmessages.h>

#define NULL 0
#define BCD2BIN(val) (((val) & 0x0F) + ((val) >> 4) * 10)

/* Groesse der Bloecke, mit denen "mem -t" den Heap abklopft. */
#define MEM_TEST_SIZE   64
#define MEM_TEST_SMALL  32
#define MEM_TEST_LARGE  128

void update_infobar() {
	while(1)
	{
		display_update_statusbar();
		sleep(10);
	}
}

void task() {
	while(1)
	{
		sleep(1000);
	}
}	

void taskmanager(char *cmd);
void memory(char *cmd);
void help(void);
//void network_test(char *cmd);

static void mem_print_right(uint32_t value, int width);
static void mem_show_status(void);
static void mem_selftest(void);
static void mem_check(int ok);

/* Zaehler des Selbsttests, von mem_check() gefuehrt. */
static int mem_tests_run = 0;
static int mem_tests_ok = 0;

void main()
{
	char cmd[256];
	char word[100];

    printf("eltomato's TomatOS 0.31 [Version 0.31 Build 2011/27/09]\n");
    printf("(c) Copyright 2006-2011 Jens Köhler\n\n");
	
	taskmgr_task_start(taskmgr_add_task( update_infobar, "Statusbar Update Task", TASK_PRIORITY_HIGH ));
	//taskmgr_task_start(taskmgr_add_task( task, "Test Task", TASK_PRIORITY_LOW ));
	
	do{
		printf("\n@TomatOS> ");
		
		scan(cmd);

		/* prmv() gibt einen Zeiger auf einen statischen Puffer zurueck --
		   das erste Wort wird deshalb weggesichert, bevor irgendwo sonst
		   (z.B. in taskmanager()) erneut prmv() aufgerufen wird. */
		strcpy(word, prmv(0, cmd));

		/* strcmp() liefert jetzt 0 bei Gleichheit (uebliche C-Semantik). */
		if(strcmp(word, "taskmgr") == 0) taskmanager(cmd);
		else if(strcmp(word, "mem") == 0) memory(cmd);
		else if(strcmp(word, "reboot") == 0) reboot();
		else if(strcmp(word, "help") == 0) help();
		else if(strcmp(word, "start") == 0) taskmgr_task_start(taskmgr_add_task( task, "Test Task", TASK_PRIORITY_LOW ));
		else if(strcmp(word, "exit") == 0) ; /* wird von der Schleifenbedingung erledigt */
		/* Nur bei leerer Eingabe kommentarlos ein neuer Prompt. */
		else if(word[0] != EOS) printf("Unbekannter Befehl: %s\n", word);

		//if(strcmp(word, "test") == 0) network_test(cmd);

	} while(strcmp(cmd, "exit") != 0);

	//taskmgr_killall();

	cls();
	printf("Sie können den Computer jetzt ausschalten!");

	/* main() laeuft als Task und darf nicht zurueckkehren. */
	for(;;);
}
/*
void network_test(char *cmd)
{
	char mac_address[7];
	char i;
	for (i = 0; i < 6; i++)
	{
		mac_address[i] = inportb(ioaddr + i); // ioaddr is the base address obtainable from the PCI device configuration space.
	}
	
	printf("MAC ADDRESS: %s", mac_address);
}
*/
void taskmanager(char *cmd)
{
	if(prmc(cmd)==0)
	{
		printf("Syntax: taskmgr [-l] [-k pid] [-s pid] [-r pid]\n");
		printf("\t-l        Tasks auflisten\n");
		printf("\t-k PID    Task beenden\n");
		printf("\t-s PID    Task anhalten\n");
		printf("\t-r PID    Task fortführen\n");
	}
	
	/* strcmp() liefert 0 bei Gleichheit -- der Rueckgabewert darf deshalb
	   nicht mehr direkt als Wahrheitswert benutzt werden. */
	if(strcmp(prmv(1, cmd), "-l") == 0) //List tasks
	{
		taskmgr_list_tasks();
	}
	
	if(strcmp(prmv(1, cmd), "-k") == 0) //Kill PID
	{
		if(prmc(cmd) < 2)
		{
			printf("Keine PID angegeben!\n");
		} else {
			taskmgr_task_abort(atoi(prmv(2, cmd)), 0, "Canceled by user");
		}
	}
	
	if(strcmp(prmv(1, cmd), "-s") == 0) //Suspend PID
	{
		if(prmc(cmd) < 2)
		{
			printf("Keine PID angegeben!\n");
		} else {
			taskmgr_task_suspend(atoi(prmv(2, cmd)));
		}
	}
	if(strcmp(prmv(1, cmd), "-r") == 0) //Resume PID
	{
		if(prmc(cmd) < 2)
		{
			printf("Keine PID angegeben!\n");
		} else {
			taskmgr_task_start(atoi(prmv(2, cmd)));
		}
	}
	
}

/* --- mem ---------------------------------------------------------------- */

/* Gibt value rechtsbündig in einem Feld der Breite width aus. printf() kennt
   keine Feldbreiten wie %5i, also zählen wir die Stellen selbst ab und legen
   die fehlenden Leerzeichen davor. */
static void mem_print_right(uint32_t value, int width)
{
	uint32_t rest;
	int digits;

	digits = 1;
	rest = value;
	while(rest >= 10)
	{
		rest = rest / 10;
		digits++;
	}

	while(digits < width)
	{
		putch(' ');
		digits++;
	}

	printf("%u", (int)value);
}

/* Eine Zeile der Tabelle: Beschriftung, MiB, KiB und Frame-Zahl.
   Alles wird aus der Frame-Zahl gerechnet, damit die drei Zeilen
   zueinander passen: 1 Frame = 4 KiB, 256 Frames = 1 MiB. */
static void mem_print_frames(char *label, uint32_t frames)
{
	printf("%s", label);
	mem_print_right(frames / 256, 6);
	mem_print_right(frames * (PMM_FRAME_SIZE / 1024), 11);
	mem_print_right(frames, 12);
	printf("\n");
}

static void mem_show_status(void)
{
	uint32_t used;
	uint32_t heapused;
	uint32_t heaptotal;

	used = pmm_used_frames();
	heapused = heap_used();
	heaptotal = heap_total();

	printf("Speicherbelegung:\n");
	printf("                     MiB        KiB       Frames\n");
	mem_print_frames("  Physisch gesamt:", pmm_total_frames());
	mem_print_frames("  Physisch belegt:", used);
	mem_print_frames("  Physisch frei:  ", pmm_free_frames_count());
	printf("  Frame-Größe %u Bytes, Memory-Map %u KiB nutzbar\n",
	       (int)PMM_FRAME_SIZE, (int)(pmm_total_bytes() / 1024));

	printf("  Heap belegt:  ");
	mem_print_right(heapused, 8);
	printf(" Bytes (");
	mem_print_right(heapused / 1024, 6);
	printf(" KiB)\n");

	printf("  Heap gesamt:  ");
	mem_print_right(heaptotal, 8);
	printf(" Bytes (");
	mem_print_right(heaptotal / 1024, 6);
	printf(" KiB)\n");
}

/* Vermerkt das Ergebnis einer Prüfung und schreibt die Marke an den
   Zeilenanfang. Den Rest der Zeile gibt der Aufrufer aus. */
static void mem_check(int ok)
{
	mem_tests_run++;

	if(ok)
	{
		mem_tests_ok++;
		printf("  [  OK  ] ");
	} else {
		printf("  [FEHLER] ");
	}
}

/* Muster, das in die Testbloecke geschrieben wird. Bewusst von der Position
   abhaengig, damit ein verschobener oder ueberschriebener Block auffaellt. */
static unsigned char mem_pattern(int i)
{
	return (unsigned char)((i * 7 + 3) & 0xFF);
}

static void mem_selftest(void)
{
	unsigned char *a;
	unsigned char *b;
	unsigned char *c;
	unsigned char *r;
	unsigned char *r2;
	uint32_t base;
	uint32_t used_before;
	uint32_t used_after;
	int i;
	int good;
	int good2;

	mem_tests_run = 0;
	mem_tests_ok = 0;
	base = heap_used();

	printf("Heap-Selbsttest:\n");
	printf("  Start: %u Bytes belegt, %u Bytes angefordert\n",
	       (int)base, (int)heap_total());

	/* 1. malloc() liefert Speicher, der sich beschreiben und wieder
	      auslesen lässt. */
	a = (unsigned char *)malloc(MEM_TEST_SIZE);
	good = 0;
	if(a != 0)
	{
		for(i = 0; i < MEM_TEST_SIZE; i++)
		{
			a[i] = mem_pattern(i);
		}
		for(i = 0; i < MEM_TEST_SIZE; i++)
		{
			if(a[i] == mem_pattern(i)) good++;
		}
	}
	mem_check(a != 0 && good == MEM_TEST_SIZE);
	printf("malloc(%i) = 0x%X, Muster %i/%i Bytes\n",
	       MEM_TEST_SIZE, (int)a, good, MEM_TEST_SIZE);

	/* 2. Eine zweite Allokation darf die erste weder überlappen noch
	      beschädigen -- b wird deshalb vollgeschrieben und a nachgeprüft. */
	b = (unsigned char *)malloc(MEM_TEST_SIZE);
	good2 = 0;
	if(a != 0 && b != 0)
	{
		memset(b, (char)0xAA, MEM_TEST_SIZE);
		for(i = 0; i < MEM_TEST_SIZE; i++)
		{
			if(a[i] == mem_pattern(i)) good2++;
		}
	}
	mem_check(a != 0 && b != 0 && good2 == MEM_TEST_SIZE &&
	          (a + MEM_TEST_SIZE <= b || b + MEM_TEST_SIZE <= a));
	printf("0x%X und 0x%X getrennt, %i/%i Bytes unberührt\n",
	       (int)a, (int)b, good2, MEM_TEST_SIZE);

	/* 3. Nach free() muss dieselbe Größe wieder aus dem freigewordenen
	      Platz kommen, der Heap also nicht endlos wachsen. */
	used_before = heap_used();
	free(a);
	free(b);
	a = (unsigned char *)malloc(MEM_TEST_SIZE);
	b = (unsigned char *)malloc(MEM_TEST_SIZE);
	used_after = heap_used();
	mem_check(a != 0 && b != 0 && used_after == used_before);
	printf("free + malloc: belegt %u -> %u Bytes\n",
	       (int)used_before, (int)used_after);
	free(a);
	free(b);

	/* 4. calloc() muss genullten Speicher liefern. */
	c = (unsigned char *)calloc(16, 4);
	good = 0;
	if(c != 0)
	{
		for(i = 0; i < MEM_TEST_SIZE; i++)
		{
			if(c[i] == 0) good++;
		}
	}
	mem_check(c != 0 && good == MEM_TEST_SIZE);
	printf("calloc(16,4) = 0x%X, %i/%i Bytes genullt\n",
	       (int)c, good, MEM_TEST_SIZE);
	free(c);

	/* 5. realloc() muss den bisherigen Inhalt mitnehmen. */
	r = (unsigned char *)malloc(MEM_TEST_SMALL);
	if(r != 0)
	{
		for(i = 0; i < MEM_TEST_SMALL; i++)
		{
			r[i] = mem_pattern(i);
		}
	}
	r2 = (unsigned char *)realloc(r, MEM_TEST_LARGE);
	good = 0;
	if(r2 != 0)
	{
		for(i = 0; i < MEM_TEST_SMALL; i++)
		{
			if(r2[i] == mem_pattern(i)) good++;
		}
	}
	mem_check(r != 0 && r2 != 0 && good == MEM_TEST_SMALL);
	printf("realloc(%i -> %i) = 0x%X, %i/%i Bytes erhalten\n",
	       MEM_TEST_SMALL, MEM_TEST_LARGE, (int)r2, good, MEM_TEST_SMALL);
	if(r2 != 0)
	{
		free(r2);
	} else {
		free(r);
	}

	/* 6. free(0) ist erlaubt und darf nicht abstürzen. Kommen wir hier
	      wieder heraus, ist die Prüfung bestanden. */
	free(NULL);
	mem_check(1);
	printf("free(0) überstanden\n");

	/* 7. Alles Angeforderte ist zurückgegeben -- der Heap muss wieder
	      auf dem Ausgangswert stehen. */
	used_after = heap_used();
	mem_check(used_after == base);
	printf("Heap wieder bei %u Bytes (Start %u Bytes)\n",
	       (int)used_after, (int)base);

	printf("  Ergebnis: %i von %i Prüfungen bestanden\n",
	       mem_tests_ok, mem_tests_run);
}

void memory(char *cmd)
{
	char opt[100];

	if(prmc(cmd) == 0)
	{
		mem_show_status();
		return;
	}

	strcpy(opt, prmv(1, cmd));

	if(strcmp(opt, "-t") == 0)
	{
		mem_selftest();
	} else {
		printf("Syntax: mem [-t]\n");
		printf("\t          Speicherbelegung anzeigen\n");
		printf("\t-t        Heap-Selbsttest ausführen\n");
	}
}

void help(void)
{
	printf("TomatOS Help\n");
	printf("Verfügbare Befehle:\n");
	printf("\thelp      Diese Übersicht anzeigen\n");
	printf("\ttaskmgr   Tasks auflisten und steuern\n");
	printf("\tstart     Einen Test-Task starten\n");
	printf("\tmem       Speicherbelegung anzeigen, mem -t testet den Heap\n");
	printf("\treboot    Rechner neu starten\n");
	printf("\texit      Shell beenden\n");
}
