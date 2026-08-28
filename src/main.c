
#include <system.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <asm.h>
#include <math.h>
//#include <wmessages.h>

#define NULL 0
#define BCD2BIN(val) (((val) & 0x0F) + ((val) >> 4) * 10)

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
void help(void);
//void network_test(char *cmd);

void main()
{
	char cmd[256];

    printf("eltomato's TomatOS 0.31 [Version 0.31 Build 2011/27/09]\n");
    printf("(c) Copyright 2006-2011 Jens Köhler\n\n");
	
	taskmgr_task_start(taskmgr_add_task( update_infobar, "Statusbar Update Task", TASK_PRIORITY_HIGH ));
	//taskmgr_task_start(taskmgr_add_task( task, "Test Task", TASK_PRIORITY_LOW ));
	
	do{
		printf("\n@TomatOS> ");
		
		scan(cmd);
		
		/* strcmp() liefert jetzt 0 bei Gleichheit (uebliche C-Semantik). */
		if(strcmp(prmv(0, cmd), "taskmgr") == 0) taskmanager(cmd);
		if(strcmp(prmv(0, cmd), "reboot") == 0) reboot();
		if(strcmp(prmv(0, cmd), "help") == 0) help();
		if(strcmp(prmv(0, cmd), "start") == 0) taskmgr_task_start(taskmgr_add_task( task, "Test Task", TASK_PRIORITY_LOW ));

		//if(strcmp(prmv(0, cmd), "test") == 0) network_test(cmd);

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

void help(void)
{
	printf("TomatOS Help\n");
	printf("Verfügbare Befehle: taskmgr");
}
