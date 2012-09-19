/*----------------------------------------------------------------------*/
/* FAT file system sample project for FatFs            (C)ChaN, 2010    */
/*----------------------------------------------------------------------*/


#include <string.h>
#include "LPC2300.h"
#include "integer.h"
#include "interrupt.h"
#include "rtc.h"
#include "uart.h"
#include "disp.h"
#include "filer.h"
#include "xprintf.h"
#include "diskio.h"
#include "ff.h"



DWORD AccSize;				/* Work register for fs command */
WORD AccFiles, AccDirs;
FILINFO Finfo;
#if _USE_LFN
char Lfname[512];
#endif

char Line[256];				/* Console input buffer */

FATFS Fatfs[_VOLUMES];		/* File system object for each logical drive */
FIL File[2];				/* File objects */
DIR Dir;					/* Directory object */
BYTE Buff[32768] __attribute__ ((aligned (4))) ;	/* Working buffer */

volatile UINT Timer;		/* Performance timer (1kHz increment) */

IMPORT_BIN(".rodata", "bigsight.bin", BitMap1);	/* Built-in picture */
extern const uint16_t BitMap1[];



/*---------------------------------------------------------*/
/* 1000Hz timer interrupt generated by TIMER0              */
/*---------------------------------------------------------*/

void Isr_TIMER0 (void)
{
	T0IR = 1;			/* Clear irq flag */

	Timer++;			/* Performance timer */
	TmrFrm += 1000;		/* Video frame timer (disp.c) */

	MCI_timerproc();	/* Disk timer process */

}



/*---------------------------------------------------------*/
/* User Provided RTC Function for FatFs module             */
/*---------------------------------------------------------*/
/* This is a real time clock service to be called from     */
/* FatFs module. Any valid time must be returned even if   */
/* the system does not support an RTC.                     */
/* This function is not required in read-only cfg.         */


DWORD get_fattime ()
{
	RTC rtc;

	/* Get local time */
	rtc_gettime(&rtc);

	/* Pack date and time into a DWORD variable */
	return	  ((DWORD)(rtc.year - 1980) << 25)
			| ((DWORD)rtc.month << 21)
			| ((DWORD)rtc.mday << 16)
			| ((DWORD)rtc.hour << 11)
			| ((DWORD)rtc.min << 5)
			| ((DWORD)rtc.sec >> 1);
}


/*--------------------------------------------------------------------------*/
/* Monitor                                                                  */
/*--------------------------------------------------------------------------*/

static
FRESULT scan_files (
	char* path		/* Pointer to the path name working buffer */
)
{
	DIR dirs;
	FRESULT res;
	BYTE i;
	char *fn;


	if ((res = f_opendir(&dirs, path)) == FR_OK) {
		i = strlen(path);
		while (((res = f_readdir(&dirs, &Finfo)) == FR_OK) && Finfo.fname[0]) {
			if (_FS_RPATH && Finfo.fname[0] == '.') continue;
#if _USE_LFN
			fn = *Finfo.lfname ? Finfo.lfname : Finfo.fname;
#else
			fn = Finfo.fname;
#endif
			if (Finfo.fattrib & AM_DIR) {
				AccDirs++;
				*(path+i) = '/'; strcpy(path+i+1, fn);
				res = scan_files(path);
				*(path+i) = '\0';
				if (res != FR_OK) break;
			} else {
			//	xprintf("%s/%s\n", path, fn);
				AccFiles++;
				AccSize += Finfo.fsize;
			}
		}
	}

	return res;
}



static
void put_rc (FRESULT rc)
{
	const char *str =
		"OK\0" "DISK_ERR\0" "INT_ERR\0" "NOT_READY\0" "NO_FILE\0" "NO_PATH\0"
		"INVALID_NAME\0" "DENIED\0" "EXIST\0" "INVALID_OBJECT\0" "WRITE_PROTECTED\0"
		"INVALID_DRIVE\0" "NOT_ENABLED\0" "NO_FILE_SYSTEM\0" "MKFS_ABORTED\0" "TIMEOUT\0"
		"LOCKED\0" "NOT_ENOUGH_CORE\0" "TOO_MANY_OPEN_FILES\0";
	FRESULT i;

	for (i = 0; i != rc && *str; i++) {
		while (*str++) ;
	}
	xprintf("rc=%u FR_%s\n", (UINT)rc, str);
}



static
void show_disk_status (		/* Show physical disk status */
	BYTE drv
)
{
	DWORD dw;
	BYTE ct, buf[64];
	WORD w;
	char *ty, *am;


	xprintf("rc=%d\n", disk_status(drv));

	if (disk_ioctl(drv, GET_SECTOR_COUNT, &dw) == RES_OK)
		xprintf("Drive size: %lu sectors\n", dw);

	if (disk_ioctl(drv, GET_BLOCK_SIZE, &dw) == RES_OK)
		xprintf("Erase block size: %lu sectors\n", dw);

	if (disk_ioctl(drv, MMC_GET_TYPE, &ct) == RES_OK) {
		ty = "Unknown"; am = "";
		if (ct & CT_MMC) ty = "MMC";
		if (ct & CT_SD1) ty = "SDv1";
		if (ct & CT_SD2) {
			ty = "SDv2";
			am = (ct & CT_BLOCK) ? "(Block)" : "(Byte)";
		}
		xprintf("Card type: %s%s\n", ty, am);
	}

	if (disk_ioctl(drv, MMC_GET_CSD, buf) == RES_OK) {
		xputs("CSD:\n");
		put_dump(buf, 0, 16, DW_CHAR);
	}

	if (disk_ioctl(drv, MMC_GET_CID, buf) == RES_OK) {
		xputs("CID:\n");
		put_dump(buf, 0, 16, DW_CHAR);
	}

	if (disk_ioctl(drv, MMC_GET_OCR, buf) == RES_OK) {
		xputs("OCR:\n");
		put_dump(buf, 0, 4, DW_CHAR);
	}

	if ((ct & CT_SDC) && disk_ioctl(drv, MMC_GET_SDSTAT, buf) == RES_OK) {
		xputs("SD Status:\n");
		for (w = 0; w < 64; w += 16)
			put_dump(buf+w, w, 16, DW_CHAR);
	}
}



static
int fmt_progress (	/* Call-back function for CTRL_FORMAT ioctl command */
	UINT c_blk,		/* Processed units */
	UINT t_blk,		/* Total units to be processed */
	UINT t_bad		/* Total bad units */
)
{
	xprintf("%u/%u, %u\r", c_blk, t_blk, t_bad);

	return 1;	/* 0:Abort, 1:Continue */
}



static
void IoInit (void)
{
#define PLL_N		2UL
#define PLL_M		72UL
#define CCLK_DIV	4

	if ( PLLSTAT & (1 << 25) ) {
		PLLCON = 1;				/* Disconnect PLL output if PLL is in use */
		PLLFEED = 0xAA; PLLFEED = 0x55;
	}
	PLLCON = 0;				/* Disable PLL */
	PLLFEED = 0xAA; PLLFEED = 0x55;
	CLKSRCSEL = 0;			/* Select IRC (4MHz) as the PLL clock source */

	PLLCFG = ((PLL_N - 1) << 16) | (PLL_M - 1);	/* Re-configure PLL */
	PLLFEED = 0xAA; PLLFEED = 0x55;
	PLLCON = 1;				/* Enable PLL */
	PLLFEED = 0xAA; PLLFEED = 0x55;

	while ((PLLSTAT & (1 << 26)) == 0);	/* Wait for PLL locked */

	CCLKCFG = CCLK_DIV-1;	/* Select CCLK frequency (divide ratio of hclk) */
	PLLCON = 3;				/* Connect PLL output to the sysclk */
	PLLFEED = 0xAA; PLLFEED = 0x55;

	MAMCR = 0;				/* Configure MAM with 0 wait operation */
	MAMTIM = 3;
	MAMCR = 2;

	PCLKSEL0 = 0x00000000;	/* Initialize peripheral clock to default */
	PCLKSEL1 = 0x00000000;

	rtc_initialize();		/* Initialize RTC */

	ClearVector();			/* Initialie VIC */

	SCS |= 1;				/* Enable FIO0 and FIO1 */

	/* Initialize Timer0 as 1kHz interval timer */
	RegisterIrq(TIMER0_IRQn, Isr_TIMER0, PRI_LOWEST);
	T0CTCR = 0;
	T0MR0 = 18000 - 1;		/* 18M / 1k = 18000 */
	T0MCR = 0x3;			/* Clear TC and Interrupt on MR0 match */
	T0TCR = 1;

	IrqEnable();			/* Enable Irq */
}



int main (void)
{
	char *ptr, *ptr2;
	long p1, p2, p3, p4, p5;
	BYTE res, b, drv = 0;
	UINT s1, s2, cnt, blen = sizeof Buff;
	static const BYTE ft[] = {0, 12, 16, 32};
	DWORD ofs = 0, sect = 0, blk[2];
	FATFS *fs;				/* Pointer to file system object */
	RTC rtc;


	IoInit();				/* Initialize PLL, VIC and timer */

	uart_init();			/* Initialize UART and join it to the console */
	xdev_in(uart_getc);
	xdev_out(uart_putc);

	xputs("\nFatFs module test monitor for LPC2300/MCI/NAND\n");
	xputs(_USE_LFN ? "LFN Enabled" : "LFN Disabled");
	xprintf(", Code page: %u\n", _CODE_PAGE);
	xprintf("MMC/SD -> Drive %u\nNAND-FTL -> Drive %u\n", DN_MCI, DN_NAND);

	for (b = 0; b < _VOLUMES; b++)	/* Pre-mount all volumes */
		f_mount(b, Fatfs + b);

#if _USE_LFN
	Finfo.lfname = Lfname;
	Finfo.lfsize = sizeof Lfname;
#endif

	for (;;) {
		xputc('>');
		xgets(Line, sizeof Line);
		ptr = Line;

		switch (*ptr++) {
		case 'F' :
			switch (*ptr++) {
			case 'D' :	/* FD - Start filer */
				disp_init();
				filer(File, Buff, sizeof Buff);
				break;
			case 'L' :	/* FL <file> - Launch file loader */
				while (*ptr == ' ') ptr++;
				load_file(ptr, File, Buff, sizeof Buff);
				break;
			}
			break;

		case 'g' :	/* Graphic controls */
			switch (*ptr++) {
			case 'i' :	/* gi - Initialize display */
				disp_init();
				break;

			case 'k' :	/* gk <l> <r> <t> <b> - Set mask */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2) || !xatoi(&ptr, &p3) || !xatoi(&ptr, &p4)) break;
				disp_mask(p1, p2, p3, p4);
				break;

			case 'f' :	/* gf <l> <r> <t> <b> <col> - Rectangular fill */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2) || !xatoi(&ptr, &p3) || !xatoi(&ptr, &p4) || !xatoi(&ptr, &p5)) break;
				disp_fill(p1, p2, p3, p4, p5);
				break;

			case 'm' :	/* gm <x> <y> - Set current position */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2)) break;
				disp_moveto(p1, p2);
				break;

			case 'l' :	/* gl <x> <y> <col> - Draw line */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2) || !xatoi(&ptr, &p3)) break;
				disp_lineto(p1, p2, p3);
				break;

			case 'p' :	/* gp <x> <y> <col> - Set point */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2) || !xatoi(&ptr, &p3)) break;
				disp_pset(p1, p2, p3);
				break;

			case 'b' :	/* gb <x> <y> - Put built-in bitmap image */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2)) break;
				disp_blt(p1, BitMap1[0] - 1 + p1, p2, BitMap1[1] - 1 + p2, &BitMap1[2]);
				break;

			}
			break;

		case 'm' :	/* Memory dump/fill/edit */
			switch (*ptr++) {
			case 'd' :	/* md[b|h|w] <address> [<count>] - Dump memory */
				switch (*ptr++) {
				case 'w': p3 = DW_LONG; break;
				case 'h': p3 = DW_SHORT; break;
				default: p3 = DW_CHAR;
				}
				if (!xatoi(&ptr, &p1)) break;
				if (!xatoi(&ptr, &p2)) p2 = 128 / p3;
				for (ptr = (char*)p1; p2 >= 16 / p3; ptr += 16, p2 -= 16 / p3)
					put_dump(ptr, (DWORD)ptr, 16 / p3, p3);
				if (p2) put_dump((BYTE*)ptr, (UINT)ptr, p2, p3);
				break;
			case 'f' :	/* mf <address> <byte> <count> - Fill memory */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2) || !xatoi(&ptr, &p3)) break;
				while (p3--) {
					*(BYTE*)p1 = (BYTE)p2;
					p1++;
				}
				break;
			case 'e' :	/* me[b|h|w] <address> [<value> ...] - Edit memory */
				switch (*ptr++) {	/* Get data width */
				case 'w': p3 = 4; break;
				case 'h': p3 = 2; break;
				default: p3 = 1;
				}
				if (!xatoi(&ptr, &p1)) break;	/* Get start address */
				if (xatoi(&ptr, &p2)) {	/* 2nd parameter is given (direct mode) */
					do {
						switch (p3) {
						case 4: *(DWORD*)p1 = (DWORD)p2; break;
						case 2: *(WORD*)p1 = (WORD)p2; break;
						default: *(BYTE*)p1 = (BYTE)p2;
						}
						p1 += p3;
					} while (xatoi(&ptr, &p2));	/* Get next value */
					break;
				}
				for (;;) {				/* 2nd parameter is not given (interactive mode) */
					switch (p3) {
					case 4: xprintf("%08X 0x%08X-", p1, *(DWORD*)p1); break;
					case 2: xprintf("%08X 0x%04X-", p1, *(WORD*)p1); break;
					default: xprintf("%08X 0x%02X-", p1, *(BYTE*)p1);
					}
					ptr = Line; xgets(ptr, sizeof Line);
					if (*ptr == '.') break;
					if ((BYTE)*ptr >= ' ') {
						if (!xatoi(&ptr, &p2)) continue;
						switch (p3) {
						case 4: *(DWORD*)p1 = (DWORD)p2; break;
						case 2: *(WORD*)p1 = (WORD)p2; break;
						default: *(BYTE*)p1 = (BYTE)p2;
						}
					}
					p1 += p3;
				}
				break;
			}
			break;

		case 'd' :	/* Disk I/O layer controls */
			switch (*ptr++) {
			case 'd' :	/* dd [<drv> [<lba>]] - Dump secrtor */
				if (!xatoi(&ptr, &p1)) {
					p1 = drv;
				} else {
					if (!xatoi(&ptr, &p2)) p2 = sect;
				}
				drv = (BYTE)p1; sect = p2 + 1;
				res = disk_read((BYTE)p1, Buff, p2, 1);
				if (res) { xprintf("rc=%d\n", (WORD)res); break; }
				xprintf("D:%lu S:%lu\n", p1, p2);
				for (ptr=(char*)Buff, ofs = 0; ofs < 0x200; ptr+=16, ofs+=16)
					put_dump((BYTE*)ptr, ofs, 16, DW_CHAR);
				break;

			case 'i' :	/* di <drv> - Initialize disk */
				if (!xatoi(&ptr, &p1)) break;
				xprintf("rc=%d\n", (WORD)disk_initialize((BYTE)p1));
				break;

			case 's' :	/* ds <drv> - Show disk status */
				if (!xatoi(&ptr, &p1)) break;
				show_disk_status((BYTE)p1);
				break;

			case 'c' :	/* Disk ioctl */
				switch (*ptr++) {
				case 's' :	/* dcs <drv> - CTRL_SYNC */
					if (!xatoi(&ptr, &p1)) break;
					xprintf("rc=%d\n", disk_ioctl((BYTE)p1, CTRL_SYNC, 0));
					break;
				case 'e' :	/* dce <drv> <start> <end> - CTRL_ERASE_SECTOR */
					if (!xatoi(&ptr, &p1) || !xatoi(&ptr, (long*)&blk[0]) || !xatoi(&ptr, (long*)&blk[1])) break;
					xprintf("rc=%d\n", disk_ioctl((BYTE)p1, CTRL_ERASE_SECTOR, blk));
					break;
				case 'f' :	/* dcf <drv> - CTRL_FORMAT */
					if (!xatoi(&ptr, &p1)) break;
					xprintf("\nrc=%d\n", disk_ioctl((BYTE)p1, CTRL_FORMAT, fmt_progress));
					break;
				}
				break;
			}
			break;

		case 'b' :	/* Buffer controls */
			switch (*ptr++) {
			case 'd' :	/* bd <addr> - Dump R/W buffer */
				if (!xatoi(&ptr, &p1)) break;
				for (ptr=(char*)&Buff[p1], ofs = p1, cnt = 32; cnt; cnt--, ptr+=16, ofs+=16)
					put_dump((BYTE*)ptr, ofs, 16, DW_CHAR);
				break;

			case 'e' :	/* be <addr> [<data>] ... - Edit R/W buffer */
				if (!xatoi(&ptr, &p1)) break;
				if (xatoi(&ptr, &p2)) {
					do {
						Buff[p1++] = (BYTE)p2;
					} while (xatoi(&ptr, &p2));
					break;
				}
				for (;;) {
					xprintf("%04X %02X-", (WORD)(p1), (WORD)Buff[p1]);
					xgets(Line, sizeof Line);
					ptr = Line;
					if (*ptr == '.') break;
					if (*ptr < ' ') { p1++; continue; }
					if (xatoi(&ptr, &p2))
						Buff[p1++] = (BYTE)p2;
					else
						xputs("???\n");
				}
				break;

			case 'r' :	/* br <drv> <lba> [<num>] - Read disk into R/W buffer */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2)) break;
				if (!xatoi(&ptr, &p3)) p3 = 1;
				xprintf("rc=%u\n", (WORD)disk_read((BYTE)p1, Buff, p2, p3));
				break;

			case 'w' :	/* bw <drv> <lba> [<num>] - Write R/W buffer into disk */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2)) break;
				if (!xatoi(&ptr, &p3)) p3 = 1;
				xprintf("rc=%u\n", (WORD)disk_write((BYTE)p1, Buff, p2, p3));
				break;

			case 'f' :	/* bf <val> - Fill working buffer */
				if (!xatoi(&ptr, &p1)) break;
				memset(Buff, (BYTE)p1, sizeof Buff);
				break;

			}
			break;

		case 'f' :	/* FatFS API controls */
			switch (*ptr++) {

			case 'i' :	/* fi <vol> - Force initialized the logical drive */
				if (!xatoi(&ptr, &p1)) break;
				put_rc(f_mount((BYTE)p1, &Fatfs[p1]));
				break;

			case 's' :	/* fs [<path>] - Show volume status */
				while (*ptr == ' ') ptr++;
				res = f_getfree(ptr, (DWORD*)&p2, &fs);
				if (res) { put_rc(res); break; }
				xprintf("FAT type = FAT%u\nBytes/Cluster = %lu\nNumber of FATs = %u\n"
						"Root DIR entries = %u\nSectors/FAT = %lu\nNumber of clusters = %lu\n"
						"FAT start (lba) = %lu\nDIR start (lba,clustor) = %lu\nData start (lba) = %lu\n\n...",
						ft[fs->fs_type & 3], (DWORD)fs->csize * 512, fs->n_fats,
						fs->n_rootdir, fs->fsize, (DWORD)fs->n_fatent - 2,
						fs->fatbase, fs->dirbase, fs->database
				);
				AccSize = AccFiles = AccDirs = 0;
				res = scan_files(ptr);
				if (res) { put_rc(res); break; }
				xprintf("\r%u files, %lu bytes.\n%u folders.\n"
						"%lu KB total disk space.\n%lu KB available.\n",
						AccFiles, AccSize, AccDirs,
						(fs->n_fatent - 2) * (fs->csize / 2), p2 * (fs->csize / 2)
				);
				break;

			case 'l' :	/* fl [<path>] - Directory listing */
				while (*ptr == ' ') ptr++;
				res = f_opendir(&Dir, ptr);
				if (res) { put_rc(res); break; }
				p1 = s1 = s2 = 0;
				for(;;) {
					res = f_readdir(&Dir, &Finfo);
					if ((res != FR_OK) || !Finfo.fname[0]) break;
					if (Finfo.fattrib & AM_DIR) {
						s2++;
					} else {
						s1++; p1 += Finfo.fsize;
					}
					xprintf("%c%c%c%c%c %u/%02u/%02u %02u:%02u %9lu  %-12s  %s\n",
							(Finfo.fattrib & AM_DIR) ? 'D' : '-',
							(Finfo.fattrib & AM_RDO) ? 'R' : '-',
							(Finfo.fattrib & AM_HID) ? 'H' : '-',
							(Finfo.fattrib & AM_SYS) ? 'S' : '-',
							(Finfo.fattrib & AM_ARC) ? 'A' : '-',
							(Finfo.fdate >> 9) + 1980, (Finfo.fdate >> 5) & 15, Finfo.fdate & 31,
							(Finfo.ftime >> 11), (Finfo.ftime >> 5) & 63,
							Finfo.fsize, Finfo.fname,
#if _USE_LFN
							Lfname);
#else
							"");
#endif
				}
				xprintf("%4u File(s),%10lu bytes total\n%4u Dir(s)", s1, p1, s2);
				res = f_getfree(ptr, (DWORD*)&p1, &fs);
				if (res == FR_OK)
					xprintf(", %10lu bytes free\n", p1 * fs->csize * 512);
				else
					put_rc(res);
				break;

			case 'o' :	/* fo <mode> <file> - Open a file */
				if (!xatoi(&ptr, &p1)) break;
				while (*ptr == ' ') ptr++;
				put_rc(f_open(&File[0], ptr, (BYTE)p1));
				break;

			case 'c' :	/* fc - Close a file */
				put_rc(f_close(&File[0]));
				break;

			case 'e' :	/* fe - Seek file pointer */
				if (!xatoi(&ptr, &p1)) break;
				res = f_lseek(&File[0], p1);
				put_rc(res);
				if (res == FR_OK)
					xprintf("fptr=%lu(0x%lX)\n", File[0].fptr, File[0].fptr);
				break;

			case 'd' :	/* fd <len> - read and dump file from current fp */
				if (!xatoi(&ptr, &p1)) break;
				ofs = File[0].fptr;
				while (p1) {
					if ((UINT)p1 >= 16) { cnt = 16; p1 -= 16; }
					else 				{ cnt = p1; p1 = 0; }
					res = f_read(&File[0], Buff, cnt, &cnt);
					if (res != FR_OK) { put_rc(res); break; }
					if (!cnt) break;
					put_dump(Buff, ofs, cnt, DW_CHAR);
					ofs += 16;
				}
				break;

			case 'r' :	/* fr <len> - read file */
				if (!xatoi(&ptr, &p1)) break;
				p2 = 0;
				Timer = 0;
				while (p1) {
					if ((UINT)p1 >= blen) {
						cnt = blen; p1 -= blen;
					} else {
						cnt = p1; p1 = 0;
					}
					res = f_read(&File[0], Buff, cnt, &s2);
					if (res != FR_OK) { put_rc(res); break; }
					p2 += s2;
					if (cnt != s2) break;
				}
				xprintf("%lu bytes read with %lu kB/sec.\n", p2, Timer ? (p2 / Timer) : 0);
				break;

			case 'w' :	/* fw <len> <val> - write file */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2)) break;
				memset(Buff, (BYTE)p2, blen);
				p2 = 0;
				Timer = 0;
				while (p1) {
					if ((UINT)p1 >= blen) {
						cnt = blen; p1 -= blen;
					} else {
						cnt = p1; p1 = 0;
					}
					res = f_write(&File[0], Buff, cnt, &s2);
					if (res != FR_OK) { put_rc(res); break; }
					p2 += s2;
					if (cnt != s2) break;
				}
				xprintf("%lu bytes written with %lu kB/sec.\n", p2, Timer ? (p2 / Timer) : 0);
				break;

			case 'n' :	/* fn <old_name> <new_name> - Change file/dir name */
				while (*ptr == ' ') ptr++;
				ptr2 = strchr(ptr, ' ');
				if (!ptr2) break;
				*ptr2++ = 0;
				while (*ptr2 == ' ') ptr2++;
				put_rc(f_rename(ptr, ptr2));
				break;

			case 'u' :	/* fu <name> - Unlink a file or dir */
				while (*ptr == ' ') ptr++;
				put_rc(f_unlink(ptr));
				break;

			case 'v' :	/* fv - Truncate file */
				put_rc(f_truncate(&File[0]));
				break;

			case 'k' :	/* fk <name> - Create a directory */
				while (*ptr == ' ') ptr++;
				put_rc(f_mkdir(ptr));
				break;

			case 'a' :	/* fa <atrr> <mask> <name> - Change file/dir attribute */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2)) break;
				while (*ptr == ' ') ptr++;
				put_rc(f_chmod(ptr, p1, p2));
				break;

			case 't' :	/* ft <year> <month> <day> <hour> <min> <sec> <name> - Change timestamp */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2) || !xatoi(&ptr, &p3)) break;
				Finfo.fdate = ((p1 - 1980) << 9) | ((p2 & 15) << 5) | (p3 & 31);
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2) || !xatoi(&ptr, &p3)) break;
				Finfo.ftime = ((p1 & 31) << 11) | ((p2 & 63) << 5) | ((p3 >> 1) & 31);
				put_rc(f_utime(ptr, &Finfo));
				break;

			case 'x' : /* fx <src_name> <dst_name> - Copy file */
				while (*ptr == ' ') ptr++;
				ptr2 = strchr(ptr, ' ');
				if (!ptr2) break;
				*ptr2++ = 0;
				while (*ptr2 == ' ') ptr2++;
				xprintf("Opening \"%s\"", ptr);
				res = f_open(&File[0], ptr, FA_OPEN_EXISTING | FA_READ);
				xputc('\n');
				if (res) {
					put_rc(res);
					break;
				}
				xprintf("Creating \"%s\"", ptr2);
				res = f_open(&File[1], ptr2, FA_CREATE_ALWAYS | FA_WRITE);
				xputc('\n');
				if (res) {
					put_rc(res);
					f_close(&File[0]);
					break;
				}
				xprintf("Copying file...");
				Timer = 0;
				p1 = 0;
				for (;;) {
					res = f_read(&File[0], Buff, blen, &s1);
					if (res || s1 == 0) break;   /* error or eof */
					res = f_write(&File[1], Buff, s1, &s2);
					p1 += s2;
					if (res || s2 < s1) break;   /* error or disk full */
				}
				xprintf("\n%lu bytes copied with %lu kB/sec.\n", p1, p1 / Timer);
				f_close(&File[0]);
				f_close(&File[1]);
				break;
#if _FS_RPATH
			case 'g' :	/* fg <path> - Change current directory */
				while (*ptr == ' ') ptr++;
				put_rc(f_chdir(ptr));
				break;

			case 'j' :	/* fj <drive#> - Change current drive */
				if (!xatoi(&ptr, &p1)) break;
				put_rc(f_chdrive((BYTE)p1));
				break;
#if _FS_RPATH >= 2
			case 'q' :	/* fq - Show current dir path */
				res = f_getcwd(Line, sizeof Line);
				if (res)
					put_rc(res);
				else
					xprintf("%s\n", Line);
				break;
#endif
#endif
#if _USE_MKFS
			case 'm' :	/* fm <vol> <partition rule> <cluster size> - Create file system */
				if (!xatoi(&ptr, &p1) || !xatoi(&ptr, &p2) || !xatoi(&ptr, &p3)) break;
				xprintf("The volume will be formatted. Are you sure? (Y/n)=");
				xgets(ptr, sizeof Line);
				if (*ptr == 'Y')
					put_rc(f_mkfs((BYTE)p1, (BYTE)p2, (WORD)p3));
				break;
#endif
			case 'z' :	/* fz [<rw size>] - Change R/W length for fr/fw/fx command */
				if (xatoi(&ptr, &p1) && p1 >= 1 && (size_t)p1 <= sizeof Buff)
					blen = p1;
				xprintf("blen=%u\n", blen);
				break;
			}
			break;

		case 't' :	/* t [<year> <mon> <mday> <hour> <min> <sec>] */
			if (xatoi(&ptr, &p1)) {
				rtc.year = (WORD)p1;
				xatoi(&ptr, &p1); rtc.month = (BYTE)p1;
				xatoi(&ptr, &p1); rtc.mday = (BYTE)p1;
				xatoi(&ptr, &p1); rtc.hour = (BYTE)p1;
				xatoi(&ptr, &p1); rtc.min = (BYTE)p1;
				if (!xatoi(&ptr, &p1)) break;
				rtc.sec = (BYTE)p1;
				rtc_settime(&rtc);
			}
			rtc_gettime(&rtc);
			xprintf("%u/%u/%u %02u:%02u:%02u\n", rtc.year, rtc.month, rtc.mday, rtc.hour, rtc.min, rtc.sec);
			break;

		case '?' :	/* Show Command List */
			xprintf(
				"[Disk contorls]\n"
				" di <pd#> - Initialize disk\n"
				" dd [<pd#> <sect>] - Dump a secrtor\n"
				" ds <pd#> - Show disk status\n"
				" dcs <drv> - Issure CTRL_SYNC command\n"
				" dce <drv> <start> <end> - Issue CTRL_ERASE_SECTOR command\n"
				" dcf <drv> - Issure NAND_FORMAT command\n"
				"[Buffer controls]\n"
				" bd <ofs> - Dump working buffer\n"
				" be <ofs> [<data>] ... - Edit working buffer\n"
				" br <pd#> <sect> [<num>] - Read disk into working buffer\n"
				" bw <pd#> <sect> [<num>] - Write working buffer into disk\n"
				" bf <val> - Fill working buffer\n"
				"[File system controls]\n"
				" fi <ld#> - Force initialized the volume\n"
				" fs [<path>] - Show volume status\n"
				" fl [<path>] - Show a directory\n"
				" fo <mode> <file> - Open a file\n"
				" fc - Close the file\n"
				" fe <ofs> - Move fp in normal seek\n"
				" fd <len> - Read and dump the file\n"
				" fr <len> - Read the file\n"
				" fw <len> <val> - Write to the file\n"
				" fn <org.name> <new.name> - Rename an object\n"
				" fu <name> - Unlink an object\n"
				" fv - Truncate the file at current fp\n"
				" fk <name> - Create a directory\n"
				" fa <atrr> <mask> <name> - Change object attribute\n"
				" ft <yyyy> <mm> <dd> <hh> <mm> <ss> <name> - Change timestamp of an object\n"
				" fx <src.file> <dst.file> - Copy a file\n"
				" fg <path> - Change current directory\n"
				" fj <ld#> - Change current drive\n"
				" fq - Show current directory\n"
				" fm <ld#> <rule> <cluster size> - Create file system\n"
				"[Graphic commands]\n"
				" gi - Initialize display\n"
				" gk <l> <r> <t> <b> - Set active area\n"
				" gf <l> <r> <t> <b> <col> - Draw solid rectangular\n"
				" gm <x> <y> - Move current position\n"
				" gl <x> <y> <col> - Draw line to\n"
				" gp <x> <y> <col> - Draw single pixel\n"
				" gb <x> <y> - Put built-in bitmap\n"
				"[Misc commands]\n"
				" FD - Launch filer\n"
				" FL <file> - Launch file loader (bmp jpg img txt)\n"
				" md[b|h|w] <addr> [<count>] - Dump memory\n"
				" mf <addr> <value> <count> - Fill memory\n"
				" me[b|h|w] <addr> [<value> ...] - Edit memory\n"
				"\n"
			);
			break;
		}
	}
}

