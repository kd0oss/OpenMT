/*
 *  loader.h - tiny serial loader for the STM32F105 that speaks the ST
 *  USART boot loader protocol (AN3155), so stm32flash works unchanged.
 *
 *  Flash layout (F105, 2 KB pages):
 *
 *      0x08000000  loader code            (pages 0 and 1, 4 KB)
 *      0x08001000  "application valid" marker page (page 2)
 *      0x08001800  application            (page 3 .. end of flash)
 *
 *  The loader never writes below LD_APP_START.
 */

#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>

#ifndef LD_FLASH_KB
#define LD_FLASH_KB         128
#endif
#ifndef LD_RAM_KB
#define LD_RAM_KB           32
#endif

#define LD_FLASH_BASE       0x08000000UL
#define LD_PAGE_SIZE        2048UL
#define LD_FLASH_END        (LD_FLASH_BASE + (uint32_t)LD_FLASH_KB * 1024UL)
#define LD_TOTAL_PAGES      ((uint32_t)LD_FLASH_KB * 1024UL / LD_PAGE_SIZE)

#define LD_MARKER_ADDR      0x08001000UL            /* page 2            */
#define LD_APP_START        0x08001800UL            /* page 3            */
#define LD_APP_FIRST_PAGE   ((LD_APP_START - LD_FLASH_BASE) / LD_PAGE_SIZE)

#define LD_RAM_BASE         0x20000000UL
#define LD_RAM_SIZE         ((uint32_t)LD_RAM_KB * 1024UL)

/* 16 bytes at the very end of RAM survive a software reset: the
 * application writes LD_BOOT_MAGIC there to ask for the loader. */
#define LD_BOOT_FLAG_ADDR   (LD_RAM_BASE + LD_RAM_SIZE - 16UL)
#define LD_BOOT_MAGIC       0xB007B007UL

#define LD_PID              0x0418U                 /* reported chip ID  */
#define LD_VERSION          0x31U                   /* bootloader v3.1   */

#ifndef LDR_DEBUG
#define LDR_DEBUG       0
#endif

#define LD_ACK              0x79U
#define LD_NACK             0x1FU

/* ---- provided by the hardware layer (loader_hw.c / the PC test) ------ */

/* Wait for the 0x7F start-up byte, measure the baud rate, set up the
 * UART (8 bits, even parity, 1 stop).  The ACK is sent by the caller. */
void     ld_autobaud(void);

/* Next received byte, or -1 if that byte arrived with a line error (parity,
 * framing, overrun).  Blocks; keeps the watchdog fed. */
int      ld_getc(void);
void     ld_putc(uint8_t c);
void     ld_flush(void);                    /* wait until all is sent    */
void     ld_kick(void);                     /* feed the watchdog         */

int      ld_flash_erase_page(uint32_t page);                       /* 0 = ok */
int      ld_flash_program(uint32_t addr, const uint8_t *d, uint32_t len);
void     ld_flash_read(uint32_t addr, uint8_t *d, uint32_t len);   /* flash or RAM */

int      ld_marker_valid(void);             /* 1 = application complete  */
int      ld_marker_invalidate(void);        /* 0 = ok                    */
int      ld_marker_commit(void);            /* 0 = ok                    */

void     ld_launch(void);                   /* reset into the application */

/* ---- portable part (loader_core.c) ----------------------------------- */
int      ld_app_valid(void);                /* marker + vector table check */
void     ld_run(void);                      /* never returns             */

#endif /* LOADER_H */
