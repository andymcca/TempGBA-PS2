#include "common.h"
#include "../sound.h"

#include <audsrv.h>
#include <kernel.h>
#include <string.h>

volatile unsigned int AudioFastForwarded;

#define DAMPEN_SAMPLE_COUNT (AUDIO_OUTPUT_BUFFER_SIZE / 32)
#define AUDIO_THREAD_STACK_SIZE (8 * 1024)
/* Lower number = higher priority on the EE. Main is demoted below this. */
#define AUDIO_THREAD_PRIORITY 0x30
#define EMU_THREAD_PRIORITY   0x40
/* One GBA frame at 44100 Hz: 44100 / 59.7275 ≈ 738.35. Hardware wait_audio
 * uses the integer part; HOST uses a Bresenham remainder so rate does not
 * walk. The ring is still 1476 frames so a 739-sample frame fits. */
#define AUDIO_DAC_FRAMES      738
#define AUDIO_DAC_BYTES       (AUDIO_DAC_FRAMES * 2 * (int)sizeof(s16))
#define AUDIO_CHUNK_BYTES     (AUDIO_OUTPUT_BUFFER_SIZE * 2 * (int)sizeof(s16))
#define GBA_RATE_NUM          597275u
#define GBA_RATE_DEN          10000u

#ifdef SOUND_TO_FILE
FILE* WaveFile;
#endif

audsrv_fmt_t sound_settings;

extern void *_gp;

static unsigned char ps2_sound_buffer[AUDIO_CHUNK_BYTES] __attribute__((aligned(64)));
static unsigned char ps2_silence[AUDIO_CHUNK_BYTES] __attribute__((aligned(64)));
static u8 audio_thread_stack[AUDIO_THREAD_STACK_SIZE] __attribute__((aligned(16)));
static s32 audio_thread_id = -1;
static volatile int audio_running = 0;
static volatile int audio_paused = 1;
static volatile int audio_pause_count = 0;

static inline void RenderSample(int16_t* Left, int16_t* Right)
{
	int16_t LeftPart, RightPart;
	uint32_t j;
	for (j = 0; j < OUTPUT_FREQUENCY_DIVISOR; j++) {
		ReGBA_LoadNextAudioSample(&LeftPart, &RightPart);

		/* The GBA outputs in 12-bit sound. Make it louder. */
		if      (LeftPart >  2047) LeftPart =  2047;
		else if (LeftPart < -2048) LeftPart = -2048;
		*Left += LeftPart / OUTPUT_FREQUENCY_DIVISOR;

		if      (RightPart >  2047) RightPart =  2047;
		else if (RightPart < -2048) RightPart = -2048;
		*Right += RightPart / OUTPUT_FREQUENCY_DIVISOR;
	}
}

/* Returns 1 if the output buffer was filled, 0 on underrun (buffer untouched). */
static int feed_buffer(unsigned char *buffer, int len)
{
	s16* stream = (s16*) buffer;
	u32 Samples = ReGBA_GetAudioSamplesAvailable() / OUTPUT_FREQUENCY_DIVISOR;
	u32 Requested = len / (2 * sizeof(s16));

	u8 WasInUnderrun = Stats.InSoundBufferUnderrun;
	Stats.InSoundBufferUnderrun = Samples < Requested * 2;
	if (Stats.InSoundBufferUnderrun && !WasInUnderrun)
	{
		Stats.SoundBufferUnderrunCount++;
		if ((Stats.SoundBufferUnderrunCount % 30) == 1)
			printf("Audio underrun x%u — skipping stale buffer\r\n",
				(unsigned)Stats.SoundBufferUnderrunCount);
	}

	/* There must be AUDIO_OUTPUT_BUFFER_SIZE * 2 samples generated in order
	 * for the first AUDIO_OUTPUT_BUFFER_SIZE to be valid. Some sound is
	 * generated in the past from the future, and if the first
	 * AUDIO_OUTPUT_BUFFER_SIZE is grabbed before the core has had time to
	 * generate all of it (at AUDIO_OUTPUT_BUFFER_SIZE * 2), the end may
	 * still be silence, causing crackling. */
	if (Samples < Requested * 2)
		return 0;

	Stats.InSoundBufferUnderrun = 0;

	s16* Next = stream;

	// Take the first half of the sound.
	uint32_t i;
	for (i = 0; i < Requested / 2; i++)
	{
		s16 Left = 0, Right = 0;
		RenderSample(&Left, &Right);

		*Next++ = Left  << 4;
		*Next++ = Right << 4;
	}
	Samples -= Requested / 2;

	// Discard as many samples as are generated in 1 frame, if fast-forwarding.
	bool Skipped = false;
	unsigned int VideoFastForwardedCopy = VideoFastForwarded;
	if (VideoFastForwardedCopy != AudioFastForwarded)
	{
		unsigned int FramesToSkip = (VideoFastForwardedCopy > AudioFastForwarded)
			? /* no overflow */ VideoFastForwardedCopy - AudioFastForwarded
			: /* overflow */    0x100 - (AudioFastForwarded - VideoFastForwardedCopy);
		uint32_t SamplesToSkip = (uint32_t) (FramesToSkip * (OUTPUT_SOUND_FREQUENCY / 59.73f));
		if (SamplesToSkip > Samples - (Requested * 3 - Requested / 2))
			SamplesToSkip = Samples - (Requested * 3 - Requested / 2);
		ReGBA_DiscardAudioSamples(SamplesToSkip * OUTPUT_FREQUENCY_DIVISOR);
		Samples -= SamplesToSkip;
		AudioFastForwarded = VideoFastForwardedCopy;
		Skipped = true;
	}

	// Take the second half of the sound now.
	for (i = 0; i < Requested - Requested / 2; i++)
	{
		s16 Left = 0, Right = 0;
		RenderSample(&Left, &Right);

		*Next++ = Left  << 4;
		*Next++ = Right << 4;
	}
	Samples -= Requested - Requested / 2;

	// If we skipped sound, dampen the transition between the two halves.
	if (Skipped)
	{
		for (i = 0; i < DAMPEN_SAMPLE_COUNT; i++)
		{
			uint_fast8_t j;
			for (j = 0; j < 2; j++)
			{
				stream[Requested / 2 + i * 2 + j] = (int16_t) (
					  (int32_t) stream[Requested / 2 - i * 2 - 2 + j] * (DAMPEN_SAMPLE_COUNT - (int32_t) i) / (DAMPEN_SAMPLE_COUNT + 1)
					+ (int32_t) stream[Requested / 2 + i * 2 + j] * ((int32_t) i + 1) / (DAMPEN_SAMPLE_COUNT + 1)
					);
			}
		}
	}

	return 1;
}

static void audio_thread(void *arg)
{
	(void)arg;

	while (audio_running)
	{
		if (audio_paused)
		{
			SleepThread();
			continue;
		}

		{
			char *out = ps2_sound_buffer;
			if (!feed_buffer(ps2_sound_buffer, AUDIO_DAC_BYTES))
			{
				/* Silence, not the last chunk (held note) and not Sleep
				 * (that stops the stream until the next emu wakeup). */
				out = (char *)ps2_silence;
			}
#ifndef HOST
			/* wait_audio is a sync IOP RPC. On PCSX2 HOST it can stall the
			 * whole EE for seconds; on hardware it is the DAC clock. */
			audsrv_wait_audio(AUDIO_DAC_BYTES);
#endif
			if (!audio_paused && audio_running)
				audsrv_play_audio(out, AUDIO_DAC_BYTES);
		}
	}

	SleepThread();
}

static void wakeup_audio_thread(void)
{
	if (audio_thread_id >= 0)
		WakeupThread(audio_thread_id);
}

void init_audio()
{
	int ret;

	ret = audsrv_init();
	if (ret != 0)
	{
		printf("Audsrv returned error: %s\n", audsrv_get_error_string());
		return;
	}

	sound_settings.freq = OUTPUT_SOUND_FREQUENCY;
	sound_settings.bits = 16;
	sound_settings.channels = 2;

	ret = audsrv_set_format(&sound_settings);
	if(ret != AUDSRV_ERR_NOERROR)
	{
		printf("Audsrv returned error: %s\n", audsrv_get_error_string());
		return;
	}

	audsrv_set_volume(MAX_VOLUME);
	memset(ps2_silence, 0, sizeof(ps2_silence));

	audio_running = 1;
	audio_paused = 0;
	audio_pause_count = 0;

#ifdef HOST
	/* No worker on PCSX2. A high-priority Count spin starved the menu
	 * down to ~2 fps. Play from ReGBA_AudioUpdate on the EE main thread. */
	audio_thread_id = -1;
	ps2_audio_resync();
	printf("HOST audio: one GBA frame per emulated frame\n");
	return;
#endif

	/* Let the audio worker preempt the emu thread only while it is awake. */
	ChangeThreadPriority(GetThreadId(), EMU_THREAD_PRIORITY);

	{
		ee_thread_t thread;
		memset(&thread, 0, sizeof(thread));
		thread.func = audio_thread;
		thread.stack = audio_thread_stack;
		thread.stack_size = sizeof(audio_thread_stack);
		thread.gp_reg = &_gp;
		thread.initial_priority = AUDIO_THREAD_PRIORITY;
		thread.option = 0;
		audio_thread_id = CreateThread(&thread);
		if (audio_thread_id < 0)
		{
			printf("Failed to create audio thread (%d); audio stays on the EE main thread\n",
				(int)audio_thread_id);
			audio_thread_id = -1;
			audio_running = 0;
		}
		else
		{
			StartThread(audio_thread_id, NULL);
			printf("Audio thread started (id %d)\n", (int)audio_thread_id);
		}
	}
}

#ifdef HOST
/* 2018 audsrv has no available/queued. Submit exactly one GBA frame of
 * samples per emulated frame (same 59.7275 clock as video). A variable
 * "catch up to the display" dump made pitch walk; leading silence before
 * the first real buffer made the stream start late vs graphics. */
static unsigned int host_audio_v0;
static u32 host_audio_played;
static u32 host_gba_frac;
static int host_pending_frames;
static int host_stream_started;

void ps2_audio_resync(void)
{
	host_audio_played = 0;
	host_gba_frac = 0;
	host_pending_frames = 0;
	host_stream_started = 0;
}

void ps2_audio_begin_frame(void)
{
	if (host_pending_frames < 8)
		host_pending_frames++;
}

static u32 host_samples_for_gba_frame(void)
{
	host_gba_frac += (u32)OUTPUT_SOUND_FREQUENCY * GBA_RATE_DEN;
	{
		u32 n = host_gba_frac / GBA_RATE_NUM;
		host_gba_frac -= n * GBA_RATE_NUM;
		return n;
	}
}

static void host_audio_pump(void)
{
	if (audio_paused)
		return;

	while (host_pending_frames > 0)
	{
		u32 field_num, field_den, allowed, n, bytes;
		unsigned int elapsed;
		char *out;

		{
			u32 saved_frac = host_gba_frac;

			n = host_samples_for_gba_frame();
			if (n < 32u || n > (u32)AUDIO_OUTPUT_BUFFER_SIZE)
			{
				host_gba_frac = saved_frac;
				host_pending_frames--;
				continue;
			}

			if (host_stream_started)
			{
				ps2_display_field_rate(&field_num, &field_den);
				elapsed = vblank_ticks - host_audio_v0;
				allowed = (u32)(((u64)elapsed * (u64)OUTPUT_SOUND_FREQUENCY * (u64)field_den)
					/ (u64)field_num);
				/* Already one frame ahead of the display clock — retry after
				 * pace waits, do not dump a second frame this field. */
				if (host_audio_played >= allowed + n)
				{
					host_gba_frac = saved_frac;
					return;
				}
			}

			bytes = n * 2u * (u32)sizeof(s16);
			out = (char *)ps2_sound_buffer;
			if (!feed_buffer(ps2_sound_buffer, (int)bytes))
			{
				/* Do not enqueue silence before the first real buffer: that
				 * spends DAC time on nothing and leaves game audio late. */
				if (!host_stream_started)
				{
					host_gba_frac = saved_frac;
					return;
				}
				out = (char *)ps2_silence;
			}
		}

		if (!host_stream_started)
		{
			host_audio_v0 = vblank_ticks;
			host_audio_played = 0;
			host_stream_started = 1;
		}

		audsrv_play_audio(out, (int)bytes);
		host_audio_played += n;
		host_pending_frames--;
	}
}
#else
void ps2_audio_resync(void)
{
}

void ps2_audio_begin_frame(void)
{
}
#endif

signed int ReGBA_AudioUpdate()
{
	if (audio_thread_id >= 0)
	{
		if (!audio_paused)
			wakeup_audio_thread();
		return 0;
	}

#ifdef HOST
	host_audio_pump();
#else
	/* Fallback if the worker thread could not be created. */
	if (feed_buffer(ps2_sound_buffer, AUDIO_DAC_BYTES))
		audsrv_play_audio((char*)ps2_sound_buffer, AUDIO_DAC_BYTES);
#endif

	return 0;
}

void pause_audio()
{
	audio_pause_count++;
	audio_paused = 1;
	audsrv_stop_audio();
	ps2_audio_resync();
}

void resume_audio()
{
	if (audio_pause_count > 0)
		audio_pause_count--;
	if (audio_pause_count == 0)
	{
		audio_paused = 0;
		ps2_audio_resync();
		wakeup_audio_thread();
	}
}
