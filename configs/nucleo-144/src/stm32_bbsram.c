/************************************************************************************
 * configs/nucleo-144/src/stm32_bbsram.c
 *
 *   Copyright (C) 2016 Gregory Nutt. All rights reserved.
 *   Author: David Sidrane <david_s5@nscdg.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ************************************************************************************/

/************************************************************************************
 * Included Files
 ************************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <debug.h>
#include <syslog.h>

#include <up_internal.h>
#include <stm32_bbsram.h>

#include "nucleo-144.h"

#ifdef CONFIG_STM32F7_BBSRAM

/************************************************************************************
 * Pre-processor Definitions
 ************************************************************************************/

/* Configuration ********************************************************************/
/* The path to the Battery Backed up SRAM */

#define BBSRAM_PATH "/bbr"
#define HARDFAULT_FILENO 3
#define HARDFAULT_PATH "/bbr3"

/* The sizes of the files to create (-1) use rest of BBSRAM memory */

#define BSRAM_FILE_SIZES \
  { \
    256, \
    256, \
    1024, \
    -1, \
    0 \
  }

#define MAX_FILE_PATH_LENGTH 40
#define CONFIG_ISTACK_SIZE 800
#define CONFIG_USTACK_SIZE 800

#define ARRAYSIZE(a) (sizeof((a))/sizeof(a[0]))

/************************************************************************************
 * Private Data
 ************************************************************************************/

/* Used for stack frame storage */

typedef uint32_t stack_word_t;

/* Stack related data */

typedef struct
{
  uint32_t sp;
  uint32_t top;
  uint32_t size;

} _stack_t;

typedef struct
{
  _stack_t user;
#if CONFIG_ARCH_INTERRUPTSTACK > 3
  _stack_t interrupt;
#endif
} stack_t;

/* Not Used for reference only */

typedef struct
{
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r4;
  uint32_t r5;
  uint32_t r6;
  uint32_t r7;
  uint32_t r8;
  uint32_t r9;
  uint32_t r10;
  uint32_t r11;
  uint32_t r12;
  uint32_t sp;
  uint32_t lr;
  uint32_t pc;
  uint32_t xpsr;
  uint32_t d0;
  uint32_t d1;
  uint32_t d2;
  uint32_t d3;
  uint32_t d4;
  uint32_t d5;
  uint32_t d6;
  uint32_t d7;
  uint32_t d8;
  uint32_t d9;
  uint32_t d10;
  uint32_t d11;
  uint32_t d12;
  uint32_t d13;
  uint32_t d14;
  uint32_t d15;
  uint32_t fpscr;
  uint32_t sp_main;
  uint32_t sp_process;
  uint32_t apsr;
  uint32_t ipsr;
  uint32_t epsr;
  uint32_t primask;
  uint32_t basepri;
  uint32_t faultmask;
  uint32_t control;
  uint32_t s0;
  uint32_t s1;
  uint32_t s2;
  uint32_t s3;
  uint32_t s4;
  uint32_t s5;
  uint32_t s6;
  uint32_t s7;
  uint32_t s8;
  uint32_t s9;
  uint32_t s10;
  uint32_t s11;
  uint32_t s12;
  uint32_t s13;
  uint32_t s14;
  uint32_t s15;
  uint32_t s16;
  uint32_t s17;
  uint32_t s18;
  uint32_t s19;
  uint32_t s20;
  uint32_t s21;
  uint32_t s22;
  uint32_t s23;
  uint32_t s24;
  uint32_t s25;
  uint32_t s26;
  uint32_t s27;
  uint32_t s28;
  uint32_t s29;
  uint32_t s30;
  uint32_t s31;
} proc_regs_t;

/* Flags to identify what is in the dump */

typedef enum
{
  REGS_PRESENT          = 0x01,
  USERSTACK_PRESENT     = 0x02,
  INTSTACK_PRESENT      = 0x04,
  INVALID_USERSTACK_PTR = 0x20,
  INVALID_INTSTACK_PTR  = 0x40,
} fault_flags_t;

typedef struct
{
  fault_flags_t flags;                  /* What is in the dump */
  uintptr_t     current_regs;           /* Used to validate the dump */
  int           lineno;                 /* __LINE__ to up_assert */
  int           pid;                    /* Process ID */
  uint32_t      regs[XCPTCONTEXT_REGS]; /* Interrupt register save area */
  stack_t       stacks;                 /* Stack info */
#if CONFIG_TASK_NAME_SIZE > 0
  char          name[CONFIG_TASK_NAME_SIZE + 1]; /* Task name (with NULL
                                                  * terminator) */
#endif
  char          filename[MAX_FILE_PATH_LENGTH];  /* the Last of chars in
                                                  * __FILE__ to up_assert */
} info_t;

typedef struct
{
  info_t    info;                  /* The info */
#if CONFIG_ARCH_INTERRUPTSTACK > 3 /* The amount of stack data is compile time
                                    * sized backed on what is left after the
                                    * other BBSRAM files are defined
                                    * The order is such that only the
                                    * ustack should be truncated
                                    */
  stack_word_t istack[CONFIG_USTACK_SIZE];
#endif
  stack_word_t ustack[CONFIG_ISTACK_SIZE];
} fullcontext_t;

/************************************************************************************
 * Private Data
 ************************************************************************************/

static uint8_t g_sdata[STM32F7_BBSRAM_SIZE];

/************************************************************************************
 * Private Functions
 ************************************************************************************/

/************************************************************************************
 * Name: hardfault_get_desc
 ************************************************************************************/

static int hardfault_get_desc(struct bbsramd_s *desc)
{
  int ret = -ENOENT;
  int fd = open(HARDFAULT_PATH, O_RDONLY);
  int rv;

  if (fd < 0)
    {
      syslog(LOG_INFO, "stm32 bbsram: Failed to open Fault Log file [%s] (%d)\n",
             HARDFAULT_PATH, fd);
    }
  else
    {
      ret = -EIO;
      rv  = ioctl(fd, STM32F7_BBSRAM_GETDESC_IOCTL,
                 (unsigned long)((uintptr_t)desc));

      if (rv >= 0)
        {
          ret = fd;
        }
      else
        {
          syslog(LOG_INFO, "stm32 bbsram: Failed to get Fault Log descriptor (%d)\n",
                 rv);
        }
    }

  return ret;
}

/************************************************************************************
 * Name: copy_reverse
 ************************************************************************************/

#if defined(CONFIG_STM32F7_SAVE_CRASHDUMP)
static void copy_reverse(stack_word_t *dest, stack_word_t *src, int size)
{
  while (size--)
    {
      *dest++ = *src--;
    }
}
#endif /* CONFIG_STM32F7_SAVE_CRASHDUMP */

/************************************************************************************
 * Public Functions
 ************************************************************************************/

/************************************************************************************
 * Name: stm32_bbsram_int
 ************************************************************************************/

int stm32_bbsram_int(void)
{
  int filesizes[CONFIG_STM32F7_BBSRAM_FILES + 1] = BSRAM_FILE_SIZES;
  struct bbsramd_s desc;
  int rv;

  /* Using Battery Backed Up SRAM */

  stm32_bbsraminitialize(BBSRAM_PATH, filesizes);

#if defined(CONFIG_STM32F7_SAVE_CRASHDUMP)
  /* Panic Logging in Battery Backed Up Files */
  /* Do we have an hard fault in BBSRAM? */

  rv = hardfault_get_desc(&desc);
  if (rv >= OK)
    {
      printf("There is a hard fault logged.\n");

      rv = unlink(HARDFAULT_PATH);
      if (rv < 0)
        {
          syslog(LOG_INFO, "stm32 bbsram: Failed to unlink Fault Log file [%s] (%d)\n", HARDFAULT_PATH, rv);
        }
    }
#endif /* CONFIG_STM32F7_SAVE_CRASHDUMP */

  return rv;
}

/************************************************************************************
 * Name: board_crashdump
 ************************************************************************************/

#if defined(CONFIG_STM32F7_SAVE_CRASHDUMP)
void board_crashdump(uintptr_t currentsp, FAR void *tcb,
                     FAR const uint8_t *filename, int lineno)
{
  fullcontext_t *pdump = (fullcontext_t *)&g_sdata;
  FAR struct tcb_s *rtcb;
  int rv;

  (void)enter_critical_section();

  rtcb = (FAR struct tcb_s *)tcb;

  /* Zero out everything */

  memset(pdump, 0, sizeof(fullcontext_t));

  /* Save Info */

  pdump->info.lineno = lineno;

  if (filename)
    {
      int offset = 0;
      unsigned int len = strlen((char *)filename) + 1;

      if (len > sizeof(pdump->info.filename))
        {
          offset = len - sizeof(pdump->info.filename);
        }

      strncpy(pdump->info.filename, (char *)&filename[offset],
              sizeof(pdump->info.filename));
    }

  /* Save the value of the pointer for current_regs as debugging info.
   * It should be NULL in case of an ASSERT and will aid in cross
   * checking the validity of system memory at the time of the
   * fault.
   */

  pdump->info.current_regs = (uintptr_t) CURRENT_REGS;

  /* Save Context */

#if CONFIG_TASK_NAME_SIZE > 0
  strncpy(pdump->info.name, rtcb->name, CONFIG_TASK_NAME_SIZE);
#endif

  pdump->info.pid = rtcb->pid;

  /* If  current_regs is not NULL then we are in an interrupt context
   * and the user context is in current_regs else we are running in
   * the users context
   */

  if (CURRENT_REGS)
    {
      pdump->info.stacks.interrupt.sp = currentsp;
      pdump->info.flags |= (REGS_PRESENT | USERSTACK_PRESENT | INTSTACK_PRESENT);
      memcpy(pdump->info.regs, (void *)CURRENT_REGS, sizeof(pdump->info.regs));
      pdump->info.stacks.user.sp = pdump->info.regs[REG_R13];
    }
  else
    {
      /* users context */

      pdump->info.flags |= USERSTACK_PRESENT;
      pdump->info.stacks.user.sp = currentsp;
    }

  if (pdump->info.pid == 0)
    {
      pdump->info.stacks.user.top = g_idle_topstack - 4;
      pdump->info.stacks.user.size = CONFIG_IDLETHREAD_STACKSIZE;
    }
  else
    {
      pdump->info.stacks.user.top = (uint32_t) rtcb->adj_stack_ptr;
      pdump->info.stacks.user.size = (uint32_t) rtcb->adj_stack_size;;
    }

#if CONFIG_ARCH_INTERRUPTSTACK > 3
  /* Get the limits on the interrupt stack memory */

  pdump->info.stacks.interrupt.top = (uint32_t)&g_intstackbase;
  pdump->info.stacks.interrupt.size  = (CONFIG_ARCH_INTERRUPTSTACK & ~3);

  /* If In interrupt Context save the interrupt stack data centered
   * about the interrupt stack pointer
   */

  if ((pdump->info.flags & INTSTACK_PRESENT) != 0)
    {
      stack_word_t *ps = (stack_word_t *) pdump->info.stacks.interrupt.sp;
      copy_reverse(pdump->istack, &ps[ARRAYSIZE(pdump->istack) / 2],
                   ARRAYSIZE(pdump->istack));
    }

  /* Is it Invalid? */

  if (!(pdump->info.stacks.interrupt.sp <= pdump->info.stacks.interrupt.top &&
        pdump->info.stacks.interrupt.sp > pdump->info.stacks.interrupt.top -
          pdump->info.stacks.interrupt.size))
    {
      pdump->info.flags |= INVALID_INTSTACK_PTR;
    }

#endif
  /* If In interrupt context or User save the user stack data centered
   * about the user stack pointer
   */

  if ((pdump->info.flags & USERSTACK_PRESENT) != 0)
    {
      stack_word_t *ps = (stack_word_t *) pdump->info.stacks.user.sp;
      copy_reverse(pdump->ustack, &ps[ARRAYSIZE(pdump->ustack) / 2],
                   ARRAYSIZE(pdump->ustack));
    }

  /* Is it Invalid? */

  if (!(pdump->info.stacks.user.sp <= pdump->info.stacks.user.top &&
        pdump->info.stacks.user.sp > pdump->info.stacks.user.top -
          pdump->info.stacks.user.size))
    {
      pdump->info.flags |= INVALID_USERSTACK_PTR;
    }

  rv = stm32_bbsram_savepanic(HARDFAULT_FILENO, (uint8_t *)pdump,
                              sizeof(fullcontext_t));

  /* Test if memory got wiped because of using _sdata */

  if (rv == -ENXIO)
    {
      char *dead = "Memory wiped - dump not saved!";

      while (*dead)
        {
          up_lowputc(*dead++);
        }
    }
  else if (rv == -ENOSPC)
    {
      /* hard fault again */

      up_lowputc('!');
    }

#if defined(CONFIG_BOARD_RESET_ON_CRASH)
  up_systemreset();
#endif
}
#endif /* CONFIG_STM32F7_SAVE_CRASHDUMP */

#endif /* CONFIG_STM32_BBSRAM */
