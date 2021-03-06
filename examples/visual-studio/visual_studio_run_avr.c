/*

Windows Tweaks by Mark Sherman 2017.


Copyright 2008, 2010 Michel Pollet <buserror@gmail.com>

This file is part of simavr.

simavr is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

simavr is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with simavr.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <stdio.h>
//#include <libgen.h>
#include <string.h>
#include <signal.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_core.h"
#include "sim_gdb.h"
#include "sim_hex.h"
#include "sim_io.h"
#include "avr_ioport.h"

//#include "sim_network.h"
#include "avr_uart.h"
#include "vs_sim_core_decl.h"

#include <conio.h>

void display_usage(char * app)
{
	printf("Usage: %s [--list-cores] [--help] [-t] [-g] [-v] [-m <device>] [-f <frequency>] firmware\n", app);
	printf("       --list-cores      List all supported AVR cores and exit\n"
		"       --help, -h        Display this usage message and exit\n"
		"       -trace, -t        Run full scale decoder trace\n"
		"       -ti <vector>      Add trace vector at <vector>\n"
		"       -gdb, -g          Listen for gdb connection on port 1234\n"
		"       -ff               Load next .hex file as flash\n"
		"       -ee               Load next .hex file as eeprom\n"
		"       -v                Raise verbosity level (can be passed more than once)\n");
	exit(1);
}

void list_cores() {
	printf(
		"   Supported AVR cores:\n");
	for (int i = 0; avr_kind[i]; i++) {
		printf("       ");
		for (int ti = 0; ti < 4 && avr_kind[i]->names[ti]; ti++)
			printf("%s ", avr_kind[i]->names[ti]);
		printf("\n");
	}
	exit(1);
}

avr_t * avr = NULL;

void
sig_int(
	int sign)
{
	printf("signal caught, simavr terminating\n");
	if (avr)
		avr_terminate(avr);
	exit(0);
}

/* AVR UART to TERMINAL */
void
uart_tx_hook(
struct avr_irq_t * irq,
	uint32_t value,
	void * param)
{
	printf("%c", value);
}
struct avr_irq_t* irq_uart;
const char* names[] = { "s1", "s2", "s3","s4", NULL };



void init_uart_handler(struct avr_t* avr) {

	irq_uart = avr_alloc_irq(&avr->irq_pool, 0, 4, names);

	avr_irq_register_notify(irq_uart, uart_tx_hook, NULL);

	avr_irq_t * tx_irq = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT);
	avr_connect_irq(tx_irq, irq_uart +0 ); 


	avr_irq_t * rx_irq = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_INPUT);
	avr_connect_irq( irq_uart + 1, rx_irq);

	
}

/* DEFAULTS */

#define DEFAULT_MCU "atmega644"
#define DEFAULT_FREQ 20000000

int main(int argc, char *argv[])
{
	elf_firmware_t f = { { 0 } };
	long f_cpu = DEFAULT_FREQ;
	int trace = 0;
	int gdb = 0;
	int log = 1;
	char name[16] = DEFAULT_MCU;
	uint32_t loadBase = AVR_SEGMENT_OFFSET_FLASH;
	int trace_vectors[8] = { 0 };
	int trace_vectors_count = 0;

	if (argc == 1)
		display_usage(basename(argv[0]));

	for (int pi = 1; pi < argc; pi++) {
		if (!strcmp(argv[pi], "--list-cores")) {
			list_cores();
		}
		else if (!strcmp(argv[pi], "-h") || !strcmp(argv[pi], "--help")) {
			display_usage(basename(argv[0]));
		}
		else if (!strcmp(argv[pi], "-m") || !strcmp(argv[pi], "-mcu")) {
			if (pi < argc - 1)
				strcpy(name, argv[++pi]);
			else
				display_usage(basename(argv[0]));
		}
		else if (!strcmp(argv[pi], "-f") || !strcmp(argv[pi], "-freq")) {
			if (pi < argc - 1)
				f_cpu = atoi(argv[++pi]);
			else
				display_usage(basename(argv[0]));
		}
		else if (!strcmp(argv[pi], "-t") || !strcmp(argv[pi], "-trace")) {
			trace++;
		}
		else if (!strcmp(argv[pi], "-ti")) {
			if (pi < argc - 1)
				trace_vectors[trace_vectors_count++] = atoi(argv[++pi]);
		}
		else if (!strcmp(argv[pi], "-g") || !strcmp(argv[pi], "-gdb")) {
			gdb++;
		}
		else if (!strcmp(argv[pi], "-v")) {
			log++;
		}
		else if (!strcmp(argv[pi], "-ee")) {
			loadBase = AVR_SEGMENT_OFFSET_EEPROM;
		}
		else if (!strcmp(argv[pi], "-ff")) {
			loadBase = AVR_SEGMENT_OFFSET_FLASH;
		}
		else if (argv[pi][0] != '-') {
			char * filename = argv[pi];
			char * suffix = strrchr(filename, '.');

			char* flash = NULL;
			int flashsize = 0;

			if (suffix && !strcasecmp(suffix, ".hex")) {
				if (!name[0] || !f_cpu) {
					fprintf(stderr, "%s: -mcu and -freq are mandatory to load .hex files\n", argv[0]);
					exit(1);
				}
				ihex_chunk_p chunk = NULL;
				int cnt = read_ihex_chunks(filename, &chunk);
				if (cnt <= 0) {
					fprintf(stderr, "%s: Unable to load IHEX file %s\n",
						argv[0], argv[pi]);
					exit(1);
				}
			

				printf("Loaded %d section of ihex, calculating top address\n", cnt);
				for (int ci = 0;ci < cnt;ci++) {
					int tsize = chunk[ci].size + chunk[ci].baseaddr;

					if (chunk[ci].baseaddr > 1024 * 1024) //skip the eeprom sections
						continue;

					if (tsize > flashsize)
						flashsize = tsize;
				}
				printf(" Maximum flash address in hex segments is %d\n", flashsize);

				flash = malloc(flashsize);
				if (!flash)
				{
					fprintf(stderr, "Can't allocate flash %d bytes\n", flashsize);
					exit(1);
				}

				f.flash = flash;
				f.flashbase = 0;
				f.flashsize = flashsize;

				for (int ci = 0; ci < cnt; ci++) {
					
					if (chunk[ci].baseaddr < (1 * 1024 * 1024)) {
						memcpy(f.flash + chunk[ci].baseaddr, chunk[ci].data, chunk[ci].size);
						printf("Load HEX flash %08x, %d\n", chunk[ci].baseaddr, chunk[ci].size);
						//break;
					}
					else if (chunk[ci].baseaddr >= AVR_SEGMENT_OFFSET_EEPROM ||
						chunk[ci].baseaddr + loadBase >= AVR_SEGMENT_OFFSET_EEPROM) {
						// eeprom!
						f.eeprom = chunk[ci].data;
						f.eesize = chunk[ci].size;
						printf("Load HEX eeprom %08x, %d\n", chunk[ci].baseaddr, f.eesize);
					}
				}
			}
			else {
#ifdef _MSC_VER
				fprintf(stderr, "Sorry, no ELFs on Windows, please use .hex\n");
				exit(1);
#else
				if (elf_read_firmware(filename, &f) == -1) {
					fprintf(stderr, "%s: Unable to load firmware from file %s\n",
						argv[0], filename);
					exit(1);
				}
#endif
			}
		}
	}

	if (strlen(name))
		strcpy(f.mmcu, name);
	if (f_cpu)
		f.frequency = f_cpu;

	avr = avr_make_mcu_by_name(f.mmcu);
	if (!avr) {
		fprintf(stderr, "%s: AVR '%s' not known\n", argv[0], f.mmcu);
		exit(1);
	}
	avr_init(avr);
	avr_load_firmware(avr, &f);
	if (f.flashbase) {
		printf("Attempted to load a bootloader at %04x\n", f.flashbase);
	//	avr->pc = f.flashbase;
	}
//	f.flashbase = 0;
//	avr-> pc = 0;

	avr->log = (log > LOG_TRACE ? LOG_TRACE : log);
	avr->trace = trace;
	for (int ti = 0; ti < trace_vectors_count; ti++) {
		for (int vi = 0; vi < avr->interrupts.vector_count; vi++)
			if (avr->interrupts.vector[vi]->vector == trace_vectors[ti])
				avr->interrupts.vector[vi]->trace = 1;
	}

	// even if not setup at startup, activate gdb if crashing
	avr->gdb_port = 1234;
	if (gdb) {
		avr->state = cpu_Stopped;
		avr_gdb_init(avr);
	}


	init_uart_handler(avr);


	signal(SIGINT, sig_int);
	signal(SIGTERM, sig_int);

	int cnt = 0;

	
	unsigned char xram[128 * 1024]; //our xram
	unsigned char addrhigh=0xEE;
	unsigned int bank;
	unsigned char WEWENTHIGH = 0;

	for (;;) {
				
		int state = avr_run(avr);

		if (state == cpu_Done || state == cpu_Crashed)
			break;

		cnt++;

		avr_ioport_state_t porta;
		avr_ioport_state_t portb;
		avr_ioport_state_t portc;
		avr_ioport_state_t portd;

		avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE('A'), &porta);
		avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE('B'), &portb);
		avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE('C'), &portc);
		avr_ioctl(avr, AVR_IOCTL_IOPORT_GETSTATE('D'), &portd);


		int WE = portd.port & (1<<6);
		int OE = portd.port & (1<<2);
		int PL = portd.port & (1<<3);
		

		if (porta.port & (1 <<5)) //bank select bit
			bank = 1;
		else
			bank = 0;

		

		if (PL)
			addrhigh = portb.port;  //latch high address

		if (!WE && WEWENTHIGH) {
			printf(" Write at %x %x %x  (%x)\n", bank, addrhigh, portb.port, portc.port);
			xram[ (bank<<16) | (addrhigh << 8) | portb.port] = portc.port;
		//	getch();
		} 


		avr_ioport_external_t p;
		
		if (!OE) {
			
			p.mask = 0xff; //we take all the bits
			p.value = xram[ (bank << 16) |  (addrhigh << 8) | portb.port];

		}
		else {
			p.mask = 0x0; //not asserting anything on data bus
		}
		avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL('C'), &p);


		WEWENTHIGH |= WE; //goes high if WE ever went high
		
		if (!(cnt & 0xFF)) {

			int p;
			for (p = 'D';p <= 'D';p++) {
		

				//if (port.port != 0) {
				//	printf("%d %c PORT: %d DDR:%d %d\n", r, port.name, port.port, port.ddr, port.pin);
				//}
			}
		
		
			if (kbhit()) {
				char c = getch();

				if (c == 'x') {
					
					
					avr_ioport_external_t p;
					printf("set\n");
					p.mask = 0xff;
					p.value = 111;
					//p.name = 'D';

				//	avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL('D'), &p);

				}

				if (c == 'y') {
					
					
					avr_ioport_external_t p;
					printf("set\n");
					p.mask = 0xff;
					p.value = 222;
					//p.name = 'D';

					avr_ioctl(avr, AVR_IOCTL_IOPORT_SET_EXTERNAL('D'), &p);

				}
				
				avr_raise_irq(irq_uart+1, c);

			}
		}

		

	}

	avr_terminate(avr);
}
