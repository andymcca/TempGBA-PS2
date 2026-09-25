/* Per-platform settings and headers - gpSP on DSTwo
 *
 * Copyright (C) 2013 GBATemp user Nebuleon
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licens e as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef __GPSP_PORT_H__
#define __GPSP_PORT_H__

#define MAX_PATH 256
#define MAX_FILE 256

typedef int FILE_TAG_TYPE;

typedef struct timespec timespec;

int errno;

#include <sys/types.h>
#include <osd_config.h>
#include <debug.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <libpad.h>
#include <fileXio_rpc.h>
#include <fileXio.h>
#include <fileio.h>
#include <libps2time.h>

#include "ps2.h"

struct timespec {
    time_t   tv_sec;        /* seconds */
    long     tv_nsec;       /* nanoseconds */
};

/* Tuning parameters for the PlayStation 2 Emotion Engine (32 MiB RDRAM).
 * Match the PSP split: 4 MiB of ROM/BIOS translations and 2 MiB of RAM code.
 * Large (16–32 MiB) ROMs that fail LOAD_ALL_ROM still fall back to paging. */
#define READONLY_CODE_CACHE_SIZE          (4 * 1024 * 1024)
#define WRITABLE_CODE_CACHE_SIZE          (2 * 1024 * 1024)
/* The following parameter needs to be at least enough bytes to hold
 * the generated code for the largest instruction on your platform.
 * In most cases, that will be the ARM instruction
 * STMDB R0!, {R0,R1,R2,R3,R4,R5,R6,R7,R8,R9,R10,R11,R12,R13,R14,R15} */
#define TRANSLATION_CACHE_LIMIT_THRESHOLD (1024)

#define MAX_AUTO_FRAMESKIP 4

#define FILE_OPEN_APPEND (O_CREAT | O_APPEND | O_TRUNC)

#define FILE_OPEN_READ (O_RDONLY)

#define FILE_OPEN_WRITE (O_CREAT | O_WRONLY | O_TRUNC)

#ifdef HOST

#define FILE_OPEN(filename_tag, filename, mode)                             \
  filename_tag = fioOpen(filename, FILE_OPEN_##mode)                        \

#define FILE_CHECK_VALID(filename_tag)                                      \
  (filename_tag >= 0)                                       				\

#define FILE_CLOSE(filename_tag)                                            \
  (ps2fgets_invalidate(filename_tag), fioClose(filename_tag))               \

#define FILE_DELETE(filename)                                               \
  fioRemove(filename) && fioRmdir(filename)                                 \

#define FILE_READ(filename_tag, buffer, size)                               \
  fioRead(filename_tag, buffer, size)                                       \

#define FILE_WRITE(filename_tag, buffer, size)                              \
  fioWrite(filename_tag, buffer, size)                                      \

#define FILE_SEEK(filename_tag, offset, type)                               \
  (ps2fgets_invalidate(filename_tag), fioLseek(filename_tag, offset, type)) \

#else

#define FILE_OPEN(filename_tag, filename, mode)                             \
  filename_tag = fileXioOpen(filename, FILE_OPEN_##mode, fileXio_mode)      \

#define FILE_CHECK_VALID(filename_tag)                                      \
  (filename_tag >= 0)                                       				\

#define FILE_CLOSE(filename_tag)                                            \
  (ps2fgets_invalidate(filename_tag), fileXioClose(filename_tag))           \

#define FILE_DELETE(filename)                                               \
  fileXioRemove(filename)                                                   \

#define FILE_READ(filename_tag, buffer, size)                               \
  fileXioRead(filename_tag, buffer, size)                                   \

#define FILE_WRITE(filename_tag, buffer, size)                              \
  fileXioWrite(filename_tag, buffer, size)                                  \

#define FILE_SEEK(filename_tag, offset, type)                               \
  (ps2fgets_invalidate(filename_tag), fileXioLseek(filename_tag, offset, type)) \

#endif

#define FILE_TAG_INVALID                                                    \
  (-1)                                                                      \

#define FILE_GETS(str, size, filename_tag)                                  \
  ps2fgets(str, size, filename_tag)                                         \

#include "ps2input.h"
#include "draw.h"
#include "gui.h"
#include "settings.h"
#include "main.h"
#include "ps2sound.h"
#include "unifont.h"
#include "scalers/scaler.h"

extern uint32_t PerGameBootFromBIOS;
extern uint32_t BootFromBIOS;
extern uint32_t PerGameShowFPS;
extern uint32_t ShowFPS;
extern uint32_t PerGameUserFrameskip;
extern uint32_t UserFrameskip;
extern uint32_t PerGameHblankIrqMode;
extern uint32_t HblankIrqMode;

enum {
	HBLANK_IRQ_TEMPGBA = 0, /* no scanline window (current TempGBA behaviour) */
	HBLANK_IRQ_GPSP_KAI = 1, /* scanlines 0-160 */
	HBLANK_IRQ_OFF = 2 /* scanline 0 only */
};

extern struct timespec TimeDifference(struct timespec Past, struct timespec Present);
extern void GetFileNameNoExtension(char* Result, const char* Path);

#endif
