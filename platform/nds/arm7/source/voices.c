#include <calico.h>
#include <nds.h>
#include <calico/nds/arm7/sound.h>
#include <calico/system/tick.h>
#include <string.h>

#include "voices.h"

#define CACHE_SLOTS 128
#define BRR_BLOCKS_MAX 1024

typedef struct CachedSample {
	uint16_t src;
	uint32_t loop_byte;
	uint32_t bytes;
	int8_t *pcm;
} CachedSample;

typedef struct Voice {
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
	uint16_t gain;
	uint16_t rate_counter;
	uint8_t state;
	uint8_t playing;
	uint8_t restart;
	uint8_t ended;
	uint16_t last_timer;
	uint8_t last_vol;
	uint8_t last_pan;
	unsigned play_freq;
	unsigned glide_target;
	unsigned glide_step;
	uint32_t bend_phase;
	uint32_t bend_done;
	uint32_t bend_origin;
	u64 bend_t0;
	uint8_t bend_on;
	uint8_t bend_looped;
	uint8_t bend_sync;
	uint8_t play_epoch;
	uint32_t pos;
	uint32_t frac;
	uint32_t echo_pos;
	uint32_t echo_frac;
	uint32_t bytes;
	uint32_t loop_byte;
	int last_out;
	int8_t *pcm;
} Voice;

static ZeldaApuIpc *s_ipc;
static Voice s_voice[ZELDA_APU_VOICES];
static CachedSample s_cache[CACHE_SLOTS];
static unsigned s_cache_count;
static uint32_t s_pool_used;
static uint32_t s_generation;
static uint32_t s_dir_page;
static int s_have_cache;
static int s_muted;
static int s_echo_on;
static u64 s_echo_t0;
static u32 s_echo_done;
static unsigned s_echo_timer;
static unsigned s_delay_i;
static unsigned s_delay_len;
static unsigned s_fir_i;
static int16_t s_fir_l[8];
static int16_t s_fir_r[8];
static int s_sign_l;
static int s_sign_r;
/* 8 kHz samples kept ahead of the mixer. About three wakeups. */
#define ECHO_LEAD 24
static int s_noise_ready;
static unsigned s_bend_timer;
static uint32_t s_seen_seq;

static int clamp15(int s)
{
	if (s < -0x8000)
		s = -0x8000;
	if (s > 0x7fff)
		s = 0x7fff;
	return ((int16_t)((s & 0x7fff) << 1)) >> 1;
}

static int clamp16(int s)
{
	if (s < -0x8000)
		return -0x8000;
	if (s > 0x7fff)
		return 0x7fff;
	return s;
}

static int iabs8(int v)
{
	return v < 0 ? -v : v;
}

static uint16_t rd16(const uint8_t *ram, uint16_t addr)
{
	return (uint16_t)ram[addr] | ((uint16_t)ram[(uint16_t)(addr + 1)] << 8);
}

static void gain_add(uint16_t *gain, int add)
{
	int g = (int)*gain + add;

	if (g < 0)
		g = 0;
	if (g > 0x7ff)
		g = 0x7ff;
	*gain = (uint16_t)g;
}

static void exp_dec(uint16_t *gain)
{
	uint16_t g = *gain;
	uint16_t sub = (uint16_t)(((uint16_t)(g - 1) >> 8) + 1);

	*gain = sub > g ? 0 : (uint16_t)(g - sub);
}

static void env_apply(Voice *v)
{
	switch (v->state) {
	case 0:
		gain_add(&v->gain, v->adsr_rate[0] == 1 ? 1024 : 32);
		if (v->gain >= 0x7e0)
			v->state = 1;
		break;
	case 1:
		exp_dec(&v->gain);
		if (v->gain < v->sustain)
			v->state = 2;
		break;
	case 2:
		exp_dec(&v->gain);
		break;
	case 3:
		switch (v->gain_mode & 3) {
		case 0:
			gain_add(&v->gain, -32);
			break;
		case 1:
			exp_dec(&v->gain);
			break;
		case 2:
			gain_add(&v->gain, 32);
			break;
		default:
			gain_add(&v->gain, v->gain < 0x600 ? 32 : 8);
			break;
		}
		break;
	default:
		break;
	}
}

static void env_step(Voice *v, int samples)
{
	if (v->state != 4 && v->use_gain && v->direct_gain) {
		v->gain = v->gain_value;
		if (v->gain > 0x7ff)
			v->gain = 0x7ff;
		return;
	}

	while (samples > 0) {
		uint16_t rate;
		int need;

		if (v->state == 4) {
			int g = (int)v->gain - 8 * samples;
			v->gain = g > 0 ? (uint16_t)g : 0;
			return;
		}
		rate = v->adsr_rate[v->state];
		if (rate == 0)
			return;
		need = (int)rate - (int)v->rate_counter;
		if (need <= 0)
			need = (int)rate;
		if (need > samples) {
			v->rate_counter = (uint16_t)(v->rate_counter + samples);
			return;
		}
		samples -= need;
		v->rate_counter = 0;
		env_apply(v);
	}
}

static void stop_channel(unsigned ch)
{
	soundChStop(ch);
}

/* One BRR sample per 4096 pitch counts, at the 32 kHz DSP clock.
 * Pitch 0x1000 is 32 kHz. Pitch 0x3FFF is just under 128 kHz.
 * The timer itself cannot go below about 256 Hz. */
static unsigned freq_from_pitch(unsigned pitch)
{
	if (!pitch)
		return 0;
	if (pitch > 0x3fff)
		pitch = 0x3fff;
	return (32000u * pitch) >> 12;
}

static unsigned limit_freq(unsigned freq)
{
	if (!freq)
		return 0;
	if (freq < 256)
		freq = 256;
	if (freq > 128000u)
		freq = 128000u;
	return freq;
}

static unsigned timer_from_freq(unsigned freq)
{
	unsigned timer;

	freq = limit_freq(freq);
	if (!freq)
		return 0;
	timer = soundTimerFromHz(freq);
	if (timer < 1)
		timer = 1;
	if (timer > 65535)
		timer = 65535;
	return timer;
}

static unsigned side_level(int gain, int vol, int master)
{
	/* gain, voice, and master are 11, 7, and 7 bits. >> 18 maps a fully
	 * open DSP onto one channel. The songs leave the master at 0x60 and
	 * the voice volumes far below 127, so that mapping only fills about
	 * a quarter of the register. >> 16 puts a normal note on the
	 * hardware maximum. */
	int amp = (gain * iabs8(vol) * iabs8(master)) >> 16;

	if (amp < 0)
		amp = 0;
	if (amp > 127)
		amp = 127;
	return (unsigned)amp;
}

/* Hardware pan splits one volume across both speakers. The channel
 * volume has to carry both sides or a centered voice is half as loud.
 * Pan 0 is the left speaker and 127 is the right. */
static void voice_level(const ZeldaApuIpc *ipc, const Voice *v,
			unsigned *vol, unsigned *pan)
{
	unsigned l = side_level(v->gain, v->volume_l, ipc->master_l);
	unsigned r = side_level(v->gain, v->volume_r, ipc->master_r);
	unsigned sum = l + r;

	if (!sum) {
		*vol = 0;
		*pan = 64;
		return;
	}
	*pan = (r * 127u) / sum;
	if (*pan > 127)
		*pan = 127;
	if (sum > 126)
		*vol = 127;
	else
		*vol = (sum * 128u) / 127u;
}

static const CachedSample *cache_find(uint16_t src)
{
	unsigned i;

	for (i = 0; i < s_cache_count; i++) {
		if (s_cache[i].src == src)
			return &s_cache[i];
	}
	return NULL;
}

static int decode_sample(const uint8_t *ram, uint16_t src, uint16_t loop_addr)
{
	CachedSample *slot;
	int8_t *dst;
	int old = 0;
	int older = 0;
	uint32_t pos = 0;
	uint32_t loop_byte = 0xffffffffu;
	uint16_t addr = src;
	int blocks = 0;

	if (s_cache_count >= CACHE_SLOTS)
		return 0;
	s_pool_used = (s_pool_used + 3u) & ~3u;
	if (s_pool_used >= ZELDA_APU_PCM_BYTES)
		return 0;
	dst = s_ipc->pcm + s_pool_used;

	while (blocks < BRR_BLOCKS_MAX) {
		uint8_t header, filter, flags, shift, cur = 0;
		int i;

		if (addr == loop_addr)
			loop_byte = pos;
		if (s_pool_used + pos + 16 > ZELDA_APU_PCM_BYTES)
			break;
		header = ram[addr++];
		shift = header >> 4;
		filter = (header >> 2) & 3;
		flags = header & 3;
		for (i = 0; i < 16; i++) {
			int s;

			if (i & 1) {
				s = cur & 0xf;
			} else {
				cur = ram[addr++];
				s = cur >> 4;
			}
			if (s > 7)
				s -= 16;
			if (shift <= 0xc)
				s = (s << shift) >> 1;
			else
				s = (s >> 3) << 12;
			switch (filter) {
			case 1:
				s += old + (-old >> 4);
				break;
			case 2:
				s += 2 * old + ((3 * -old) >> 5) - older + (older >> 4);
				break;
			case 3:
				s += 2 * old + ((13 * -old) >> 6) - older + ((3 * older) >> 4);
				break;
			default:
				break;
			}
			s = clamp15(s);
			older = old;
			old = s;
			/* 15-bit BRR fits in a byte at >> 8, and that leaves every
			 * voice at half of what the mixer can play. >> 7 fills the
			 * byte, so the whole mix is twice as loud. Pitch mod and
			 * the echo sum undo this extra bit. */
			dst[pos++] = (int8_t)(s >> 7);
		}
		blocks++;
		if (flags & 1) {
			if (!(flags & 2))
				loop_byte = 0xffffffffu;
			else if (loop_byte == 0xffffffffu)
				loop_byte = 0;
			break;
		}
		if ((blocks & 7) == 0)
			threadYield();
	}

	if (pos == 0)
		return 0;
	while (pos & 3) {
		if (s_pool_used + pos >= ZELDA_APU_PCM_BYTES)
			break;
		dst[pos++] = 0;
	}
	if (loop_byte != 0xffffffffu && loop_byte >= pos)
		loop_byte = 0xffffffffu;

	slot = &s_cache[s_cache_count++];
	slot->src = src;
	slot->loop_byte = loop_byte;
	slot->bytes = pos;
	slot->pcm = dst;
	s_pool_used += pos;
	return 1;
}

static void rebuild_cache(const ZeldaApuIpc *ipc)
{
	const uint8_t *ram = ipc->spc_ram;
	unsigned srcn;

	s_cache_count = 0;
	s_pool_used = 0;
	s_have_cache = 1;
	s_generation = ipc->generation;
	s_dir_page = ipc->dir_page;
	if (!ram || ipc->generation == 0 || !ipc->pcm)
		return;

	/* The directory is 32 entries. Past that the bytes are the
	 * instrument table, and decoding them filled the pool. */
	for (srcn = 0; srcn < 32; srcn++) {
		uint16_t entry = (uint16_t)(ipc->dir_page + srcn * 4);
		uint16_t src = rd16(ram, entry);
		uint16_t loop = rd16(ram, (uint16_t)(entry + 2));

		if (src < 0x4000 || src >= 0xc000)
			continue;
		if (cache_find(src))
			continue;
		if (!decode_sample(ram, src, loop))
			break;
	}
}

static void fill_noise(int8_t *dst)
{
	int noise = -0x4000;
	int i;

	for (i = 0; i < ZELDA_APU_NOISE_SAMPLES; i++) {
		int bit = (noise & 1) ^ ((noise >> 1) & 1);

		noise = ((noise >> 1) & 0x3fff) | (bit << 14);
		noise = (int16_t)(((noise & 0x7fff) << 1)) >> 1;
		dst[i] = (int8_t)(noise >> 7);
	}
	s_noise_ready = 1;
}

static int snapshot(ZeldaApuIpc *dst)
{
	ZeldaApuIpc *ipc = s_ipc;
	u32 s1, s2;

	if (!ipc)
		return 0;
	s1 = *(volatile u32 *)&ipc->seq;
	if (s1 & 1)
		return 0;
	__asm__ volatile("" ::: "memory");
	memcpy(dst, ipc, sizeof *dst);
	__asm__ volatile("" ::: "memory");
	s2 = *(volatile u32 *)&ipc->seq;
	if (s1 != s2 || dst->magic != ZELDA_APU_MAGIC)
		return 0;
	return 1;
}

/* Grunts and whooshes hold a full envelope, so the channel volume is
 * already maxed and the sample itself is what sounds quiet. One extra
 * bit, clamped, brings them up to the punch effects. The cached bytes
 * stay put because music uses the same BRR. */
#define SFX_BOOST_MAX 4096
static int8_t s_sfx_boost[ZELDA_APU_VOICES][SFX_BOOST_MAX];

static const int8_t *sfx_loud_pcm(unsigned ch, const Voice *v)
{
	const int8_t *src;
	uint32_t n, i;

	if (!(v->flags & ZELDA_VOICE_SFX) || (v->flags & ZELDA_VOICE_NOISE))
		return v->pcm;
	/* Rain was turned down 8 dB on the ARM9. Doubling its sample would
	 * give most of that back. */
	if (v->flags & ZELDA_VOICE_RAIN)
		return v->pcm;
	if (v->use_gain || v->adsr_rate[2] != 0 || v->sustain < 0x800)
		return v->pcm;
	if (!v->pcm || v->bytes < 4 || v->bytes > SFX_BOOST_MAX)
		return v->pcm;
	src = v->pcm;
	n = v->bytes;
	for (i = 0; i < n; i++) {
		int s = (int)src[i] << 1;

		if (s > 127)
			s = 127;
		if (s < -128)
			s = -128;
		s_sfx_boost[ch][i] = (int8_t)s;
	}
	return s_sfx_boost[ch];
}

static void start_pcm(unsigned ch, unsigned vol, unsigned pan, unsigned freq,
		      const int8_t *pcm, uint32_t bytes, uint32_t loop_byte)
{
	SoundMode mode;
	unsigned pnt, len, timer;

	if (!pcm || bytes < 4)
		return;
	if (loop_byte == 0xffffffffu || loop_byte >= bytes) {
		mode = SoundMode_OneShot;
		pnt = 0;
		len = bytes / 4;
	} else {
		mode = SoundMode_Repeat;
		pnt = loop_byte / 4;
		len = (bytes - loop_byte) / 4;
	}
	if (len < 1)
		len = 1;
	timer = timer_from_freq(freq);
	soundChStop(ch);
	soundChPreparePcm(ch, vol, SoundVolDiv_1, pan, timer, mode,
			  SoundFmt_Pcm8, pcm, pnt, len);
	soundChStart(ch);
}

static void silence_all(void)
{
	int i;

	for (i = 0; i < 10; i++)
		stop_channel(i);
	for (i = 0; i < ZELDA_APU_VOICES; i++) {
		s_voice[i].playing = 0;
		s_voice[i].bend_on = 0;
	}
	s_echo_on = 0;
}

static unsigned voice_freq(const ZeldaApuIpc *ipc, const Voice *v, const Voice *prev)
{
	unsigned pitch = v->pitch;
	unsigned freq;

	if (v->flags & ZELDA_VOICE_NOISE) {
		unsigned rate = ipc->noise_rate;
		unsigned freq = rate ? 32000u / rate : 0;

		if (freq > 32000)
			freq = 32000;
		return limit_freq(freq);
	}
	if ((v->flags & ZELDA_VOICE_PMON) && prev) {
		int factor = (prev->last_out >> 4) + 0x400;

		pitch = (unsigned)(((int)pitch * factor) >> 10);
		if (pitch > 0x3fff)
			pitch = 0x3fff;
	}
	freq = freq_from_pitch(pitch);
	return limit_freq(freq);
}

/* The driver publishes one pitch per frame. Vibrato is a fraction of a
 * semitone, and chasing it by rewriting the hardware timer every wake
 * buzzes the title theme, so those changes land immediately. A music
 * slide moves more than a semitone between frames. Take a fixed step of
 * that gap so the sweep arrives within the frame. A new note snaps, and
 * so does a jump of two octaves or more. Sound-effect bends do not use
 * this; the DSP frame in bend_pcm is what plays. */
static unsigned glide_freq(Voice *v, unsigned freq, int snap)
{
	unsigned cur, hi, lo;

	if (snap || !freq || !v->play_freq) {
		v->play_freq = freq;
		v->glide_target = freq;
		v->glide_step = 0;
		return freq;
	}
	if (freq != v->glide_target) {
		cur = v->play_freq;
		hi = freq > cur ? freq : cur;
		lo = freq > cur ? cur : freq;
		v->glide_target = freq;
		if (hi <= lo + lo / 16u || hi >= lo * 4u) {
			v->play_freq = freq;
			v->glide_step = 0;
			return freq;
		}
		v->glide_step = (hi - lo) / 16u;
		if (!v->glide_step)
			v->glide_step = 1;
	}
	if (!v->glide_step || v->play_freq == v->glide_target)
		return v->play_freq;
	cur = v->play_freq;
	if (v->glide_target > cur) {
		cur += v->glide_step;
		if (cur > v->glide_target)
			cur = v->glide_target;
	} else if (cur - v->glide_target <= v->glide_step) {
		cur = v->glide_target;
	} else {
		cur -= v->glide_step;
	}
	v->play_freq = cur;
	return cur;
}

static void attach_sample(Voice *v, const ZeldaApuIpc *ipc)
{
	const CachedSample *sample = NULL;

	v->pcm = NULL;
	v->bytes = 0;
	v->loop_byte = 0xffffffffu;
	v->ended = 0;
	if (v->flags & ZELDA_VOICE_NOISE) {
		if (!ipc->noise)
			return;
		v->pcm = ipc->noise;
		v->bytes = ZELDA_APU_NOISE_SAMPLES;
		v->loop_byte = 0;
		return;
	}
	if (ipc->spc_ram) {
		uint16_t entry = (uint16_t)(ipc->dir_page + v->srcn * 4);
		uint16_t src = rd16(ipc->spc_ram, entry);

		sample = cache_find(src);
	}
	if (!sample)
		return;
	v->pcm = sample->pcm;
	v->bytes = sample->bytes;
	v->loop_byte = sample->loop_byte;
}

static int8_t sample_at(Voice *v)
{
	uint32_t pos;

	if (v->ended || !v->pcm || v->bytes == 0)
		return 0;
	pos = v->pos;
	if (v->loop_byte != 0xffffffffu) {
		if (pos >= v->bytes) {
			uint32_t span = v->bytes - v->loop_byte;

			if (span == 0)
				pos = v->loop_byte;
			else
				pos = v->loop_byte + (pos - v->loop_byte) % span;
			v->pos = pos;
		}
	} else if (pos >= v->bytes) {
		v->ended = 1;
		return 0;
	}
	return v->pcm[pos];
}

/* Pitch-mod source only. Echo has its own cursor, so a delay line cannot
 * drag the dry voices off the samples the mixer is playing. */
static void advance_voice(Voice *v, unsigned freq)
{
	int8_t s;

	v->frac += freq;
	while (v->frac >= 8000) {
		v->frac -= 8000;
		v->pos++;
	}
	s = sample_at(v);
	/* sampleOut is (BRR >> 1) * gain >> 11. The byte is one bit hotter
	 * than that, so this extra shift puts the modulator back. */
	v->last_out = ((int)s * (int)v->gain) >> 5;
}

static void program_voice(const ZeldaApuIpc *ipc, unsigned ch, Voice *v, unsigned freq)
{
	unsigned vol, pan, timer;

	voice_level(ipc, v, &vol, &pan);
	if (!freq) {
		if (v->playing)
			stop_channel(ch);
		v->playing = 0;
		v->restart = 0;
		return;
	}
	if (v->state == 4 && v->gain == 0) {
		if (v->playing)
			stop_channel(ch);
		v->playing = 0;
		v->restart = 0;
		return;
	}
	if (!v->pcm) {
		if (v->playing)
			stop_channel(ch);
		v->playing = 0;
		v->restart = 0;
		return;
	}
	timer = timer_from_freq(freq);
	if (v->restart || !v->playing) {
		const int8_t *pcm = sfx_loud_pcm(ch, v);

		start_pcm(ch, vol, pan, freq, pcm, v->bytes, v->loop_byte);
		v->playing = 1;
		v->restart = 0;
		v->last_vol = (uint8_t)vol;
		v->last_pan = (uint8_t)pan;
		v->last_timer = (uint16_t)timer;
		return;
	}
	if (vol != v->last_vol) {
		soundChSetVolume(ch, vol, SoundVolDiv_1);
		v->last_vol = (uint8_t)vol;
	}
	if (pan != v->last_pan) {
		soundChSetPan(ch, pan);
		v->last_pan = (uint8_t)pan;
	}
	if (timer != v->last_timer) {
		soundChSetTimer(ch, timer);
		v->last_timer = (uint16_t)timer;
	}
}

/* A pitch bend is already rendered by the DSP on the ARM9: one frame of
 * PCM, with the gaussian filter and the envelope in the samples. Playing
 * that back at a fixed rate keeps the sweep. Rewriting the channel timer
 * to chase the pitch buzzes, and guessing the curve between frames drops
 * the turns. Steady effects and the music stay on the hardware channels. */

static void bend_stop(unsigned ch, Voice *v)
{
	if (!v->bend_on)
		return;
	soundChStop(ch);
	v->bend_on = 0;
	v->playing = 0;
}

static u32 bend_hw(const Voice *v)
{
	u64 ticks;

	if (!s_bend_timer || !v->playing)
		return 0;
	ticks = tickGetCount() - v->bend_t0;
	return (u32)((ticks * (u64)SOUND_CLOCK) /
		     ((u64)TICK_FREQ * s_bend_timer));
}

static void bend_begin(unsigned ch, Voice *v, int8_t *ring)
{
	memset(ring, 0, ZELDA_APU_BEND_RING);
	soundChStop(ch);
	v->bend_on = 1;
	v->bend_done = 0;
	v->playing = 0;
}

static void bend_level(const ZeldaApuIpc *ipc, Voice *v, unsigned *vol, unsigned *pan)
{
	uint16_t gain = v->gain;

	/* The samples already include the envelope. The channel volume is
	 * the full-open level, which is what a steady note uses at peak. */
	v->gain = 0x7ff;
	voice_level(ipc, v, vol, pan);
	v->gain = gain;
}

/* Returns 1 while this channel is playing the rendered frames. */
static int bend_service(ZeldaApuIpc *ipc, unsigned ch, Voice *v,
			int seq_changed, int play)
{
	int8_t *ring = ipc->bend_ring[ch];
	const int8_t *pcm = ipc->bend_pcm[ch];
	unsigned vol, pan, i;
	u32 hw;

	if (!ring)
		return 0;
	if (!v->bend_on) {
		if (!play || !pcm)
			return 0;
		bend_begin(ch, v, ring);
	}
	hw = bend_hw(v);
	if (v->playing && hw >= v->bend_done) {
		if (!play) {
			bend_stop(ch, v);
			return 0;
		}
		/* The queue ran out as this frame arrived. Start it at the play head. */
		v->bend_done = hw;
	}
	if (play && seq_changed && pcm) {
		u32 queued = v->bend_done - hw;
		unsigned n = ipc->voice[ch].bend_n;

		if (n > ZELDA_APU_FRAME)
			n = ZELDA_APU_FRAME;
		/* ARM7 has no data cache. The ARM9 flushed these bytes. */
		if (n && queued + n < ZELDA_APU_BEND_RING) {
			for (i = 0; i < n; i++)
				ring[(v->bend_done + i) & (ZELDA_APU_BEND_RING - 1)] = pcm[i];
			v->bend_done += n;
		}
	} else if (v->playing && v->bend_done >= hw &&
		   v->bend_done - hw < ZELDA_APU_FRAME) {
		/* Past the queued audio the ring would repeat the swoon. */
		u32 ahead = v->bend_done - hw;

		for (i = ahead; i < ZELDA_APU_FRAME; i++)
			ring[(hw + i) & (ZELDA_APU_BEND_RING - 1)] = 0;
	}

	bend_level(ipc, v, &vol, &pan);
	if (!v->playing) {
		if (!s_bend_timer)
			s_bend_timer = timer_from_freq(ZELDA_APU_BEND_RATE);
		soundChPreparePcm(ch, vol, SoundVolDiv_1, pan, s_bend_timer,
				  SoundMode_Repeat, SoundFmt_Pcm8, ring, 0,
				  ZELDA_APU_BEND_RING / 4);
		soundChStart(ch);
		v->bend_t0 = tickGetCount();
		v->playing = 1;
		v->last_vol = (uint8_t)vol;
		v->last_pan = (uint8_t)pan;
		v->last_timer = (uint16_t)s_bend_timer;
		return 1;
	}
	if (vol != v->last_vol) {
		soundChSetVolume(ch, vol, SoundVolDiv_1);
		v->last_vol = (uint8_t)vol;
	}
	if (pan != v->last_pan) {
		soundChSetPan(ch, pan);
		v->last_pan = (uint8_t)pan;
	}
	return 1;
}

/* Same sums as dsp_handleEcho, at 8 kHz. The delay count is the DSP's
 * 32 kHz length divided by 4, so the title's EDL 2 stays 32 ms. FIR preset
 * 2 is 12, 33, 43, 43, 19, -2, -13, -7. */

static int echo_wanted(const ZeldaApuIpc *ipc)
{
	return ipc->echo_writes || ipc->echo_l != 0 || ipc->echo_r != 0;
}

static unsigned echo_vol(int v)
{
	if (v < 0)
		v = -v;
	/* *2 matches the extra bit on the dry samples. *4 matches the
	 * channel boost in side_level, so the wash stays in balance. */
	v *= 8;
	if (v > 127)
		v = 127;
	return (unsigned)v;
}

static u32 echo_hw_pos(void)
{
	u64 ticks = tickGetCount() - s_echo_t0;

	if (!s_echo_timer)
		return 0;
	return (u32)((ticks * (u64)SOUND_CLOCK) /
		     ((u64)TICK_FREQ * s_echo_timer));
}

static void echo_reset_line(ZeldaApuIpc *ipc, unsigned len)
{
	memset(ipc->echo_delay_l, 0, ZELDA_APU_ECHO_DELAY_MAX * sizeof(int16_t));
	memset(ipc->echo_delay_r, 0, ZELDA_APU_ECHO_DELAY_MAX * sizeof(int16_t));
	memset(s_fir_l, 0, sizeof s_fir_l);
	memset(s_fir_r, 0, sizeof s_fir_r);
	s_delay_i = 0;
	s_fir_i = 0;
	s_delay_len = len;
}

static void echo_set_delay(ZeldaApuIpc *ipc)
{
	unsigned len = ipc->echo_delay / 4;

	if (len < 1)
		len = 1;
	if (len > ZELDA_APU_ECHO_DELAY_MAX)
		len = ZELDA_APU_ECHO_DELAY_MAX;
	if (len != s_delay_len)
		echo_reset_line(ipc, len);
}

static int8_t echo_pcm8(int s, int negative)
{
	s >>= 8;
	if (negative)
		s = -s;
	if (s < -128)
		s = -128;
	if (s > 127)
		s = 127;
	return (int8_t)s;
}

static int8_t echo_at(Voice *v)
{
	uint32_t pos;

	if (!v->pcm || v->bytes == 0)
		return 0;
	pos = v->echo_pos;
	if (v->loop_byte != 0xffffffffu) {
		if (pos >= v->bytes) {
			uint32_t span = v->bytes - v->loop_byte;

			if (span == 0)
				pos = v->loop_byte;
			else
				pos = v->loop_byte + (pos - v->loop_byte) % span;
			v->echo_pos = pos;
		}
	} else if (pos >= v->bytes) {
		return 0;
	}
	return v->pcm[pos];
}

static int echo_tap(Voice *v, unsigned freq)
{
	int8_t s;

	v->echo_frac += freq;
	while (v->echo_frac >= ZELDA_APU_ECHO_RATE) {
		v->echo_frac -= ZELDA_APU_ECHO_RATE;
		v->echo_pos++;
	}
	s = echo_at(v);
	/* Undo the extra bit on the dry samples. Echo volume puts it back. */
	return ((int)s * (int)v->gain) >> 5;
}

static void echo_sample(ZeldaApuIpc *ipc, const unsigned *freq, uint32_t pos)
{
	int in_l = 0;
	int in_r = 0;
	int sum_l = 0;
	int sum_r = 0;
	int i;
	unsigned ring = pos & (ZELDA_APU_ECHO_RING - 1);
	int fb = ipc->echo_fb;

	for (i = 0; i < ZELDA_APU_VOICES; i++) {
		Voice *v = &s_voice[i];
		int tap;

		if (!(v->flags & ZELDA_VOICE_ECHO))
			continue;
		tap = echo_tap(v, freq[i]);
		in_l = clamp16(in_l + ((tap * v->volume_l) >> 6));
		in_r = clamp16(in_r + ((tap * v->volume_r) >> 6));
	}

	s_fir_l[s_fir_i] = (int16_t)(ipc->echo_delay_l[s_delay_i] >> 1);
	s_fir_r[s_fir_i] = (int16_t)(ipc->echo_delay_r[s_delay_i] >> 1);
	for (i = 0; i < 7; i++) {
		unsigned idx = (s_fir_i + (unsigned)i + 1) & 7;
		int c = ipc->fir[i];

		sum_l += (s_fir_l[idx] * c) >> 6;
		sum_r += (s_fir_r[idx] * c) >> 6;
	}
	/* Hardware wraps the accumulator to 16 bits before the last tap. */
	sum_l = (int16_t)sum_l;
	sum_r = (int16_t)sum_r;
	sum_l += (s_fir_l[s_fir_i] * ipc->fir[7]) >> 6;
	sum_r += (s_fir_r[s_fir_i] * ipc->fir[7]) >> 6;
	sum_l = clamp16(sum_l);
	sum_r = clamp16(sum_r);

	ipc->echo_l_buf[ring] = echo_pcm8(sum_l, s_sign_l < 0);
	ipc->echo_r_buf[ring] = echo_pcm8(sum_r, s_sign_r < 0);

	if (ipc->echo_writes) {
		int w_l = clamp16(in_l + ((sum_l * fb) >> 7)) & 0xfffe;
		int w_r = clamp16(in_r + ((sum_r * fb) >> 7)) & 0xfffe;

		ipc->echo_delay_l[s_delay_i] = (int16_t)w_l;
		ipc->echo_delay_r[s_delay_i] = (int16_t)w_r;
	}
	s_fir_i = (s_fir_i + 1) & 7;
	if (++s_delay_i >= s_delay_len)
		s_delay_i = 0;
}

static void echo_stop(void)
{
	if (!s_echo_on)
		return;
	stop_channel(8);
	stop_channel(9);
	s_echo_on = 0;
}

static void echo_start(ZeldaApuIpc *ipc)
{
	unsigned words = ZELDA_APU_ECHO_RING / 4;

	s_echo_timer = timer_from_freq(ZELDA_APU_ECHO_RATE);
	s_sign_l = ipc->echo_l < 0 ? -1 : 1;
	s_sign_r = ipc->echo_r < 0 ? -1 : 1;
	memset(ipc->echo_l_buf, 0, ZELDA_APU_ECHO_RING);
	memset(ipc->echo_r_buf, 0, ZELDA_APU_ECHO_RING);
	s_delay_len = 0;
	echo_set_delay(ipc);
	s_echo_done = ECHO_LEAD;
	s_echo_t0 = tickGetCount();
	soundChPreparePcm(8, echo_vol(ipc->echo_l), SoundVolDiv_1, 0,
			  s_echo_timer, SoundMode_Repeat, SoundFmt_Pcm8,
			  ipc->echo_l_buf, 0, words);
	soundChPreparePcm(9, echo_vol(ipc->echo_r), SoundVolDiv_1, 127,
			  s_echo_timer, SoundMode_Repeat, SoundFmt_Pcm8,
			  ipc->echo_r_buf, 0, words);
	soundChStart(8);
	soundChStart(9);
	s_echo_on = 1;
}

static void echo_flip_unplayed(int8_t *buf, u32 from, u32 to)
{
	u32 i;

	for (i = from; i != to; i++) {
		unsigned k = i & (ZELDA_APU_ECHO_RING - 1);
		int s = buf[k];

		if (s == -128)
			s = 127;
		else
			s = -s;
		buf[k] = (int8_t)s;
	}
}

static void echo_sync_sign(ZeldaApuIpc *ipc)
{
	int sl = ipc->echo_l < 0 ? -1 : 1;
	int sr = ipc->echo_r < 0 ? -1 : 1;
	u32 hw;

	if (sl == s_sign_l && sr == s_sign_r)
		return;
	hw = echo_hw_pos();
	if (hw < s_echo_done) {
		if (sl != s_sign_l)
			echo_flip_unplayed(ipc->echo_l_buf, hw, s_echo_done);
		if (sr != s_sign_r)
			echo_flip_unplayed(ipc->echo_r_buf, hw, s_echo_done);
	}
	s_sign_l = sl;
	s_sign_r = sr;
}

static int echo_due(void)
{
	u32 hw = echo_hw_pos();
	u32 target;
	int steps;

	if (hw > s_echo_done && hw - s_echo_done > ZELDA_APU_ECHO_RING / 2)
		s_echo_done = hw;
	target = hw + ECHO_LEAD;
	if (s_echo_done >= target)
		return 0;
	steps = (int)(target - s_echo_done);
	if (steps > 32)
		steps = 32;
	return steps;
}

static void service_echo(ZeldaApuIpc *ipc, const unsigned *freq, int steps)
{
	int i;

	if (!s_echo_on)
		return;
	echo_set_delay(ipc);
	soundChSetVolume(8, echo_vol(ipc->echo_l), SoundVolDiv_1);
	soundChSetVolume(9, echo_vol(ipc->echo_r), SoundVolDiv_1);
	echo_sync_sign(ipc);
	for (i = 0; i < steps; i++) {
		echo_sample(ipc, freq, s_echo_done);
		s_echo_done++;
	}
}

void ZeldaVoicesInit(void)
{
	int i;

	pmPowerOn(POWCNT_SOUND);
	pmSoundSetAmpPower(true);
	REG_SOUNDBIAS = 0x200;
	REG_SOUNDCNT = SOUNDCNT_ENABLE | SOUNDCNT_VOL(127);
	soundSetMixerConfig(SoundOutSrc_Mixer, SoundOutSrc_Mixer, false, false);
	for (i = 0; i < 10; i++)
		soundChStop(i);
}

void ZeldaVoicesBind(ZeldaApuIpc *ipc)
{
	s_ipc = ipc;
}

void ZeldaVoicesService(void)
{
	ZeldaApuIpc ipc;
	int i, seq_changed;
	unsigned freq[ZELDA_APU_VOICES];

	if (!snapshot(&ipc))
		return;
	if (!s_noise_ready && ipc.noise)
		fill_noise(ipc.noise);
	if (!s_have_cache || ipc.generation != s_generation || ipc.dir_page != s_dir_page) {
		/* A song-bank upload replaces the samples under any voice that
		 * is still looping. Stop those before the pool is rebuilt. */
		if (s_have_cache && ipc.generation != s_generation) {
			int n;

			silence_all();
			for (n = 0; n < ZELDA_APU_VOICES; n++) {
				s_voice[n].playing = 0;
				s_voice[n].gain = 0;
				s_voice[n].state = 4;
			}
		}
		rebuild_cache(&ipc);
	}

	if (ipc.mute || ipc.reset) {
		if (!s_muted)
			silence_all();
		s_muted = 1;
		for (i = 0; i < ZELDA_APU_VOICES; i++)
			s_voice[i].epoch = ipc.voice[i].epoch;
		return;
	}
	s_muted = 0;
	seq_changed = ipc.seq != s_seen_seq;
	if (seq_changed)
		s_seen_seq = ipc.seq;

	for (i = 0; i < ZELDA_APU_VOICES; i++) {
		Voice *v = &s_voice[i];
		const ZeldaVoiceIpc *src = &ipc.voice[i];
		int slide;

		v->pitch = src->pitch;
		v->volume_l = src->volume_l;
		v->volume_r = src->volume_r;
		v->srcn = src->srcn;
		v->flags = src->flags;
		v->use_gain = src->use_gain;
		v->direct_gain = src->direct_gain;
		v->gain_mode = src->gain_mode;
		v->sustain = src->sustain;
		v->gain_value = src->gain_value;
		v->adsr_rate[0] = src->adsr_rate[0];
		v->adsr_rate[1] = src->adsr_rate[1];
		v->adsr_rate[2] = src->adsr_rate[2];
		v->adsr_rate[3] = src->adsr_rate[3];
		slide = (src->flags & ZELDA_VOICE_SFX) && src->bend &&
			!(src->flags & (ZELDA_VOICE_NOISE | ZELDA_VOICE_PMON));
		if (v->epoch != src->epoch) {
			v->epoch = src->epoch;
			v->pos = 0;
			v->frac = 0;
			v->echo_pos = 0;
			v->echo_frac = 0;
			v->last_out = 0;
			v->ended = 0;
			v->restart = 1;
			attach_sample(v, &ipc);
			/* A bend restarts the sample at the sequencer step, so the
			 * envelope stays put until that step is rendered. */
			if (!slide || !v->pcm || !ipc.bend_ring[i]) {
				v->state = v->use_gain ? 3 : 0;
				v->gain = 0;
				v->rate_counter = 0;
			}
		}
		if ((v->flags & ZELDA_VOICE_RELEASE) && v->state != 4)
			v->state = 4;
		freq[i] = voice_freq(&ipc, v, i ? &s_voice[i - 1] : NULL);
		if (!(v->flags & (ZELDA_VOICE_NOISE | ZELDA_VOICE_PMON)) &&
		    (src->bend || v->bend_on)) {
			/* A new note that does not bend takes the hardware channel. */
			if (v->bend_on && !src->bend && v->restart)
				bend_stop((unsigned)i, v);
			else if (bend_service(&ipc, (unsigned)i, v, seq_changed, src->bend)) {
				v->restart = 0;
				continue;
			}
		}
		if (v->bend_on)
			bend_stop((unsigned)i, v);
		env_step(v, 32);
		freq[i] = glide_freq(v, freq[i], v->restart);
		program_voice(&ipc, (unsigned)i, v, freq[i]);
	}

	/* Eight steps at 8 kHz is one millisecond, matching the thread wake.
	 * This cursor only feeds pitch modulation. */
	for (i = 0; i < 8; i++) {
		int n;

		for (n = 0; n < ZELDA_APU_VOICES; n++)
			advance_voice(&s_voice[n], freq[n]);
	}

	if (echo_wanted(&ipc) && ipc.echo_l_buf && ipc.echo_r_buf &&
	    ipc.echo_delay_l && ipc.echo_delay_r) {
		if (!s_echo_on)
			echo_start(&ipc);
		service_echo(&ipc, freq, echo_due());
	} else {
		echo_stop();
	}
}
