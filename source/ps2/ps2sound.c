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
/* Stereo S16: 1476 frames @ 44100 = 33 ms. The earlier * 2 was only
 * 16.7 ms and could not queue a second chunk ahead of the DAC. */
#define AUDIO_CHUNK_BYTES     (AUDIO_OUTPUT_BUFFER_SIZE * 2 * (int)sizeof(s16))

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
			if (!feed_buffer(ps2_sound_buffer, AUDIO_CHUNK_BYTES))
			{
				/* Silence, not the last chunk (held note) and not Sleep
				 * (that stops the stream until the next emu wakeup). */
				out = (char *)ps2_silence;
			}
#ifndef HOST
			/* wait_audio is a sync IOP RPC. On PCSX2 HOST it can stall the
			 * whole EE for seconds; on hardware it is the DAC clock. */
			audsrv_wait_audio(AUDIO_CHUNK_BYTES);
#endif
			if (!audio_paused && audio_running)
				audsrv_play_audio(out, AUDIO_CHUNK_BYTES);
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
	printf("HOST audio: main thread (no worker)\n");
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

signed int ReGBA_AudioUpdate()
{
	if (audio_thread_id >= 0)
	{
		if (!audio_paused)
			wakeup_audio_thread();
		return 0;
	}

	/* Fallback if the worker thread could not be created. */
	if (feed_buffer(ps2_sound_buffer, AUDIO_CHUNK_BYTES))
		audsrv_play_audio((char*)ps2_sound_buffer, AUDIO_CHUNK_BYTES);

	return 0;
}

void pause_audio()
{
	audio_pause_count++;
	audio_paused = 1;
	audsrv_stop_audio();
}

void resume_audio()
{
	if (audio_pause_count > 0)
		audio_pause_count--;
	if (audio_pause_count == 0)
	{
		audio_paused = 0;
		wakeup_audio_thread();
	}
}
