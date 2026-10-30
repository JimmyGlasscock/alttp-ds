#pragma once

#include <stdint.h>

/* Shared between the ARM9 publisher and the ARM7 voice service.
 * Both CPUs are 32-bit. Fields are ordered so the layout matches. */

#define ZELDA_APU_MAGIC 0x5550415Au
#define ZELDA_APU_VOICES 8
/* Intro bank decodes to about 350KB of PCM. A smaller pool keeps only
 * the first instruments, so later voices have nothing to play. */
#define ZELDA_APU_PCM_BYTES (384 * 1024)
/* Echo is mixed at 8 kHz. A 32 kHz software resample of all eight voices
 * overran the ARM7 and tore both the echo and the dry channels.
 * echo_delay is still the DSP count (32 kHz samples). EDL 2 is 1024,
 * which is 256 samples at this rate: 32 ms either way. */
#define ZELDA_APU_ECHO_RATE 8000
/* Power of two. The mixer loops this ring; the delay line is separate. */
#define ZELDA_APU_ECHO_RING 512
#define ZELDA_APU_ECHO_DSP_MAX (15 * 512)
#define ZELDA_APU_ECHO_DELAY_MAX (ZELDA_APU_ECHO_DSP_MAX / 4)
#define ZELDA_APU_NOISE_SAMPLES 256
/* One driver frame is 534 samples. The sequencer runs about every 64,
 * which is the sample at the start plus at most ten ticks. */
#define ZELDA_APU_SLICES 12
/* One video frame of the DSP, in samples. The driver clock uses this too. */
#define ZELDA_APU_FRAME 534
/* Cache-line padding so a flush of the frame cannot touch the next buffer. */
#define ZELDA_APU_FRAME_BYTES 544
/* Played back at the DSP rate. Two frames fit, so a late wake does not wrap. */
#define ZELDA_APU_BEND_RATE 32000
#define ZELDA_APU_BEND_RING 2048

enum {
	ZELDA_VOICE_NOISE = 1,
	ZELDA_VOICE_PMON = 2,
	ZELDA_VOICE_ECHO = 4,
	ZELDA_VOICE_RELEASE = 8,
	/* Channel is owned by a sound effect. */
	ZELDA_VOICE_SFX = 16,
	/* Channel is playing the outdoor or indoor rain. The ARM7 leaves
	 * its sample alone so the quieter volume from quiet_rain() holds. */
	ZELDA_VOICE_RAIN = 32
};

typedef struct ZeldaVoiceIpc {
	uint16_t pitch;
	int8_t volume_l;
	int8_t volume_r;
	uint8_t srcn;
	uint8_t epoch;
	uint8_t flags;
	uint8_t use_gain;
	uint8_t direct_gain;
	uint8_t gain_mode;
	uint16_t sustain;
	uint16_t gain_value;
	uint16_t adsr_rate[4];
	uint16_t pad;
	/* Pitch and key state at each sequencer step this frame.
	 * slice_at in the IPC is the sample time, shared by every voice. */
	uint16_t slice_pitch[ZELDA_APU_SLICES];
	uint8_t slice_epoch[ZELDA_APU_SLICES];
	uint8_t bend;
	uint8_t bend_pad;
	/* How many bytes of bend_pcm belong to this frame. */
	uint16_t bend_n;
} ZeldaVoiceIpc;

typedef struct ZeldaApuIpc {
	uint32_t magic;
	uint32_t seq;
	uint32_t generation;
	uint32_t dir_page;
	int8_t master_l;
	int8_t master_r;
	int8_t echo_l;
	int8_t echo_r;
	int8_t echo_fb;
	uint8_t mute;
	uint8_t reset;
	uint8_t echo_writes;
	int8_t fir[8];
	uint16_t echo_delay;
	uint16_t noise_rate;
	uint8_t *spc_ram;
	int8_t *pcm;
	/* Playback ring, PCM8, ZELDA_APU_ECHO_RING samples. */
	int8_t *echo_l_buf;
	int8_t *echo_r_buf;
	/* Delay line, ZELDA_APU_ECHO_DELAY_MAX samples. Not played directly. */
	int16_t *echo_delay_l;
	int16_t *echo_delay_r;
	int8_t *noise;
	ZeldaVoiceIpc voice[ZELDA_APU_VOICES];
	uint16_t slice_at[ZELDA_APU_SLICES];
	uint8_t slice_count;
	uint8_t slice_pad[3];
	/* Per-voice playback ring, PCM8, ZELDA_APU_BEND_RING samples.
	 * The ARM7 fills these from bend_pcm. */
	int8_t *bend_ring[ZELDA_APU_VOICES];
	/* One DSP frame for a pitch bend, PCM8, ZELDA_APU_FRAME samples.
	 * The ARM9 writes these. */
	int8_t *bend_pcm[ZELDA_APU_VOICES];
} ZeldaApuIpc;
