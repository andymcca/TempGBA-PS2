/* Per-platform code - ReGBA on GCW Zero
 *
 * Copyright (C) 2013 Dingoonity user Nebuleon
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

#include "common.h"
#include <malloc.h>

#if defined USE_MMAP
static FILE_TAG_TYPE MappedFile = FILE_TAG_INVALID;
static size_t MappedFileSize;
#endif

uint8_t* ReGBA_MapEntireROM(FILE_TAG_TYPE File, size_t Size)
{
#if defined USE_MMAP
	uint8_t* Result = mmap(NULL /* kernel chooses address */,
		Size,
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE,
		fileno(File),
		0 /* offset into file */);
	if (Result != NULL)
	{
		MappedFile = File;
#  if TRACE_MEMORY
		ReGBA_Trace("I: Mapped a ROM to memory via the operating system");
#  endif
	}
	return Result;
#elif defined LOAD_ALL_ROM
	// The file is kept open for us. But we close it.
	uint8_t* Result = malloc(Size);
	if (Result != NULL)
	{
		ReGBA_ProgressInitialise(FILE_ACTION_LOAD_ROM_FROM_FILE);
		uint8_t* Ptr = Result;
		size_t Read, Next, Done = 0;
		Next = Size - Done < 65536 ? Size - Done : 65536;
		while ((Read = FILE_READ(File, Ptr, Next)) > 0)
		{
			Ptr += Read;
			Done += Read;
			ReGBA_ProgressUpdate(Done, Size);
			Next = Size - Done < 65536 ? Size - Done : 65536;
		}
		ReGBA_ProgressFinalise();
		printf("ROM fully loaded into EE RAM (%u KiB)\r\n", (unsigned)(Size / 1024));
		FILE_CLOSE(File);
	}
	else
	{
		printf("ROM malloc failed for %u KiB — falling back to fileXio paging\r\n",
			(unsigned)(Size / 1024));
	}

	return Result;
#else
	return NULL;
#endif
}

void ReGBA_UnmapEntireROM(void* Mapping)
{
#if defined USE_MMAP
	munmap(Mapping, MappedFileSize);
	FILE_CLOSE(MappedFile);
#  if TRACE_MEMORY
	ReGBA_Trace("I: Unmapped the previous ROM from memory");
#  endif
	MappedFile = NULL;
	MappedFileSize = 0;
#elif defined LOAD_ALL_ROM
	free(Mapping);
#  if TRACE_MEMORY
	ReGBA_Trace("I: Unloaded the previous ROM from memory");
#  endif
#endif
}

uint8_t* ReGBA_AllocateROM(size_t Size)
{
	uint8_t* Result = malloc(Size);
#if TRACE_MEMORY
	if (Result != NULL)
		ReGBA_Trace("I: Allocated space for a %u-byte ROM buffer", Size);
	else
		ReGBA_Trace("I: Failed to allocate space for a %u-byte ROM buffer", Size);
#endif
	return Result;
}

size_t ReGBA_AllocateOnDemandBuffer(void** Buffer, size_t rom_bytes)
{
	/* Take as much leftover EE heap as we can for 32 KiB ROM pages.
	 * A 32 MiB ROM cannot fit next to the JIT caches. Step by 128 KiB
	 * so a 15.x MiB hole is not reported as 14 MiB. A GUI reserve is
	 * kept only when the whole image already fits. */
	const size_t reserve = 256 * 1024;
	const size_t step = 128 * 1024;
	size_t Size = 28 * 1024 * 1024;
	void* Result = NULL;

	while (Size >= (512 * 1024))
	{
		Result = memalign(64, Size);
		if (Result != NULL)
			break;
		Size -= step;
	}

	if (Result != NULL && rom_bytes != 0 && Size >= rom_bytes &&
	    Size > reserve + (512 * 1024) && (Size - reserve) >= rom_bytes)
	{
		size_t shrunk = Size - reserve;
		free(Result);
		Result = memalign(64, shrunk);
		if (Result != NULL)
			Size = shrunk;
		else
			Result = memalign(64, Size);
	}

	*Buffer = Result;
	if (Result != NULL)
		printf("On-demand ROM buffer: %u KiB (%u pages), ROM %u KiB\r\n",
			(unsigned)(Size / 1024),
			(unsigned)(Size / ROM_PAGE_BYTES),
			(unsigned)(rom_bytes / 1024));
	else
		printf("On-demand ROM buffer allocation failed\r\n");
	return Result != NULL ? Size : 0;
}

void ReGBA_DeallocateROM(void* Buffer)
{
	free(Buffer);
#if TRACE_MEMORY
	ReGBA_Trace("I: Deallocated space for the previous buffer");
#endif
}
