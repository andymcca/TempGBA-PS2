#include "common.h"
#include "../sound.h"

#include <audsrv.h>
#include <kernel.h>
#include <string.h>

volatile unsigned int AudioFastForwarded;

#define DAMPEN_SAMPLE_COUNT (AUDIO_OUTPUT_BUFFER_SIZE / 32)
#define AUDIO_THREAD_STACK_SIZE (8 * 1024)
/* One GBA frame at 44100 Hz: 44100 / 59.7275 ≈ 738.35. The ring is
 * still 1476 frames so a 739-sample frame fits. */
#define AUDIO_DAC_FRAMES      738
#define AUDIO_DAC_BYTES       (AUDIO_DAC_FRAMES * 2 * (int)sizeof(s16))
#define AUDIO_CHUNK_BYTES     (AUDIO_OUTPUT_BUFFER_SIZE * 2 * (int)sizeof(s16))
/* Same Count scale as the earlier HOST spin (~147.456 MHz). ~16.7 ms. */
#define AUDIO_COUNTS_PER_CHUNK 2466611u

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
static u32 audio_last_count;

static inline u32 ee_count(void)
{
	u32 c;
	__asm__ volatile("mfc0 %0, $9" : "=r"(c));
	return c;
}

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

	audio_running = 0;
	audio_paused = 0;
	audio_pause_count = 0;
	audio_thread_id = -1;
	ps2_audio_resync();
#ifdef HOST
	printf("HOST audio: one display field per enqueue (no worker)\n");
#else
	printf("Hardware audio: time-based backpressure (no worker)\n");
#endif
}

#ifdef HOST
/* 2018 audsrv has no available/queued. Submit at the display field rate
 * (44100 / field_hz samples per vblank). That is realtime on PAL and NTSC.
 * Tying enqueue to GBA frames and blocking until 59.73 chunks/sec on a
 * 50 Hz display starved the ring; the quota then discarded the backlog
 * and the stream went silent for long stretches. */
static unsigned int host_audio_v0;
static u32 host_audio_played;
static unsigned int host_audio_last_field;
static int host_audio_ready;

void ps2_audio_resync(void)
{
	host_audio_ready = 0;
}

void ps2_audio_begin_frame(void)
{
}

static void host_audio_pump(void)
{
	u32 field_num, field_den, allowed, want, bytes;
	unsigned int elapsed;
	char *out;

	if (audio_paused)
		return;

	if (!host_audio_ready)
	{
		host_audio_v0 = vblank_ticks;
		host_audio_played = 0;
		host_audio_last_field = (unsigned int)(vblank_ticks - 1);
		host_audio_ready = 1;
	}

	if (vblank_ticks == host_audio_last_field)
		return;
	host_audio_last_field = vblank_ticks;

	ps2_display_field_rate(&field_num, &field_den);
	elapsed = vblank_ticks - host_audio_v0;
	allowed = (u32)(((u64)elapsed * (u64)OUTPUT_SOUND_FREQUENCY * (u64)field_den)
		/ (u64)field_num);

	if (host_audio_played + 32u > allowed)
		return;

	want = allowed - host_audio_played;
	/* One field only — do not dump a backlog in one go (that walks pitch). */
	{
		u32 field_frames = (u32)(((u64)OUTPUT_SOUND_FREQUENCY * (u64)field_den)
			/ (u64)field_num) + 1u;
		if (want > field_frames)
			want = field_frames;
	}
	if (want > (u32)AUDIO_OUTPUT_BUFFER_SIZE)
		want = (u32)AUDIO_OUTPUT_BUFFER_SIZE;
	want &= ~1u;
	if (want < 32u)
		return;

	bytes = want * 2u * (u32)sizeof(s16);
	out = (char *)ps2_sound_buffer;
	if (!feed_buffer(ps2_sound_buffer, (int)bytes))
		out = (char *)ps2_silence;

	audsrv_play_audio(out, (int)bytes);
	host_audio_played += want;
}
#else
void ps2_audio_resync(void)
{
	audio_last_count = 0;
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
	{
		u32 now = ee_count();
		if (audio_last_count != 0 && (u32)(now - audio_last_count) < AUDIO_COUNTS_PER_CHUNK)
			return 0;
	}
	if (feed_buffer(ps2_sound_buffer, AUDIO_DAC_BYTES))
	{
		audsrv_play_audio((char *)ps2_sound_buffer, AUDIO_DAC_BYTES);
		audio_last_count = ee_count();
	}
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
