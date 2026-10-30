#include <nds.h>
#include <malloc.h>
#include <stddef.h>
#include <string.h>

#include "zelda_audio.h"
#include "zelda_apu_ipc.h"
#include "zelda_rtl.h"
#include "audio.h"
#include "spc_player.h"
#include "snes/dsp.h"

typedef char zelda_slice_count_ok[(SPC_TIMELINE == ZELDA_APU_SLICES) ? 1 : -1];

extern void Die(const char *error);

static ZeldaApuIpc *ipc;
static uint32_t flushed_generation = 0xffffffffu;

static void *alloc_buf(size_t bytes)
{
	void *p = memalign(32, bytes);

	if (!p)
		Die("Out of audio RAM");
	memset(p, 0, bytes);
	DC_FlushRange(p, bytes);
	return p;
}

void ZeldaAudioInit(void)
{
	int i;

	ipc = alloc_buf(sizeof *ipc);
	ipc->magic = ZELDA_APU_MAGIC;
	ipc->pcm = alloc_buf(ZELDA_APU_PCM_BYTES);
	ipc->echo_l_buf = alloc_buf(ZELDA_APU_ECHO_RING);
	ipc->echo_r_buf = alloc_buf(ZELDA_APU_ECHO_RING);
	ipc->echo_delay_l = alloc_buf(ZELDA_APU_ECHO_DELAY_MAX * sizeof(int16_t));
	ipc->echo_delay_r = alloc_buf(ZELDA_APU_ECHO_DELAY_MAX * sizeof(int16_t));
	/* ARM7 and the mixer own these. Drop our cache lines so a later
	 * writeback cannot wipe the delay line. */
	DC_InvalidateRange(ipc->echo_l_buf, ZELDA_APU_ECHO_RING);
	DC_InvalidateRange(ipc->echo_r_buf, ZELDA_APU_ECHO_RING);
	DC_InvalidateRange(ipc->echo_delay_l, ZELDA_APU_ECHO_DELAY_MAX * sizeof(int16_t));
	DC_InvalidateRange(ipc->echo_delay_r, ZELDA_APU_ECHO_DELAY_MAX * sizeof(int16_t));
	ipc->noise = alloc_buf(ZELDA_APU_NOISE_SAMPLES);
	for (i = 0; i < ZELDA_APU_VOICES; i++) {
		ipc->bend_ring[i] = alloc_buf(ZELDA_APU_BEND_RING);
		/* ARM7 fills the ring. Drop the cache line so a later writeback
		 * cannot wipe samples the mixer is playing. */
		DC_InvalidateRange(ipc->bend_ring[i], ZELDA_APU_BEND_RING);
		ipc->bend_pcm[i] = alloc_buf(ZELDA_APU_FRAME_BYTES);
	}
	DC_FlushRange(ipc, sizeof *ipc);

	pxiWaitRemote(PxiChannel_User0);
	pxiSend(PxiChannel_User0, (u32)ipc);
}

static void publish(SpcPlayer *p)
{
	Dsp *dsp = p->dsp;
	const SpcTimeline *tl = SpcPlayer_Timeline();
	uint32_t generation = SpcPlayer_SongGeneration();
	unsigned slices = tl->count;
	int i;

	if (slices > ZELDA_APU_SLICES)
		slices = ZELDA_APU_SLICES;

	if (generation != flushed_generation) {
		DC_FlushRange(p->ram, 65536);
		flushed_generation = generation;
	}

	ipc->seq++;
	DC_FlushRange(&ipc->seq, sizeof ipc->seq);

	ipc->generation = generation;
	ipc->dir_page = dsp->dirPage;
	ipc->master_l = dsp->masterVolumeL;
	ipc->master_r = dsp->masterVolumeR;
	ipc->echo_l = dsp->echoVolumeL;
	ipc->echo_r = dsp->echoVolumeR;
	ipc->echo_fb = dsp->feedbackVolume;
	ipc->echo_writes = dsp->echoWrites ? 1 : 0;
	ipc->echo_delay = dsp->echoDelay ? dsp->echoDelay : 1;
	if (ipc->echo_delay > ZELDA_APU_ECHO_DSP_MAX)
		ipc->echo_delay = ZELDA_APU_ECHO_DSP_MAX;
	memcpy(ipc->fir, dsp->firValues, sizeof ipc->fir);
	ipc->mute = dsp->mute ? 1 : 0;
	ipc->reset = dsp->reset ? 1 : 0;
	ipc->noise_rate = dsp->noiseRate;
	ipc->spc_ram = p->ram;
	ipc->slice_count = (uint8_t)slices;
	if (slices)
		memcpy(ipc->slice_at, tl->at, slices * sizeof ipc->slice_at[0]);

	for (i = 0; i < ZELDA_APU_VOICES; i++) {
		DspChannel *c = &dsp->channel[i];
		ZeldaVoiceIpc *v = &ipc->voice[i];
		uint8_t flags = 0;

		if (c->useNoise)
			flags |= ZELDA_VOICE_NOISE;
		if (c->pitchModulation)
			flags |= ZELDA_VOICE_PMON;
		if (c->echoEnable)
			flags |= ZELDA_VOICE_ECHO;
		if (c->adsrState == 4)
			flags |= ZELDA_VOICE_RELEASE;
		if (p->is_chan_on & (1u << i))
			flags |= ZELDA_VOICE_SFX;
		/* Port 1 sounds 1 and 2 are the outdoor rain pair, 3 and 4
		 * the indoor pair. The Zora waterfall plays the first pair. */
		if ((p->port1_active & (1u << i)) && (p->is_chan_on & (1u << i)) &&
		    p->channel[i].sfx_which_sound >= 1 &&
		    p->channel[i].sfx_which_sound <= 4)
			flags |= ZELDA_VOICE_RAIN;

		v->pitch = c->pitch;
		v->volume_l = c->volumeL;
		v->volume_r = c->volumeR;
		v->srcn = c->srcn;
		v->epoch = dsp_voice_epoch[i];
		v->flags = flags;
		v->use_gain = c->useGain ? 1 : 0;
		v->direct_gain = c->directGain ? 1 : 0;
		v->gain_mode = c->gainMode;
		v->sustain = c->sustainLevel;
		v->gain_value = c->gainValue;
		v->adsr_rate[0] = c->adsrRates[0];
		v->adsr_rate[1] = c->adsrRates[1];
		v->adsr_rate[2] = c->adsrRates[2];
		v->adsr_rate[3] = c->adsrRates[3];
		if (slices) {
			memcpy(v->slice_pitch, tl->pitch[i], slices * sizeof v->slice_pitch[0]);
			memcpy(v->slice_epoch, tl->epoch[i], slices);
		}
		v->bend = SpcPlayer_BendPlay(i) ? 1 : 0;
		v->bend_n = 0;
		if (v->bend && ipc->bend_pcm[i]) {
			unsigned n = (unsigned)SpcPlayer_BendSamples(i);

			if (n > ZELDA_APU_FRAME)
				n = ZELDA_APU_FRAME;
			v->bend_n = (uint16_t)n;
			if (n)
				memcpy(ipc->bend_pcm[i], SpcPlayer_BendPcm(i), n);
			DC_FlushRange(ipc->bend_pcm[i], ZELDA_APU_FRAME_BYTES);
		}
	}

	DC_FlushRange(ipc, sizeof *ipc);
	ipc->seq++;
	DC_FlushRange(&ipc->seq, sizeof ipc->seq);
}

/* Half a second of title music, remembered so a skip can play it before the sword. */
enum { kLeadFrames = 30 };

typedef struct LeadSnap {
	uint8 prefix[offsetof(SpcPlayer, ram)];
	Dsp dsp;
	uint8 epoch[8];
} LeadSnap;

static LeadSnap lead_ring[kLeadFrames];
static int lead_count;
static int lead_pos;

static void lead_save(const SpcPlayer *p)
{
	LeadSnap *s = &lead_ring[lead_pos];

	memcpy(s->prefix, p, sizeof s->prefix);
	s->dsp = *p->dsp;
	memcpy(s->epoch, dsp_voice_epoch, sizeof s->epoch);
	lead_pos = (lead_pos + 1) % kLeadFrames;
	if (lead_count < kLeadFrames)
		lead_count++;
}

static void lead_load(const LeadSnap *s, SpcPlayer *p)
{
	Dsp *dsp = p->dsp;

	memcpy(p, s->prefix, sizeof s->prefix);
	p->dsp = dsp;
	*dsp = s->dsp;
	dsp->apu_ram = p->ram;
	memcpy(dsp_voice_epoch, s->epoch, sizeof s->epoch);
}

void ZeldaAudioDropLeadIn(void)
{
	lead_count = 0;
	lead_pos = 0;
}

void ZeldaAudioCatchupFrame(void)
{
	SpcPlayer *p = g_zenv.player;

	if (!p || !p->dsp)
		return;
	ZeldaPopApuState();
	lead_save(p);
	SpcPlayer_Advance(p);
}

void ZeldaAudioPlayLeadIn(void)
{
	SpcPlayer *p = g_zenv.player;
	int n, i, oldest;

	if (!ipc || !p || !p->dsp || lead_count <= 0)
		return;
	n = lead_count;
	oldest = (lead_pos - n + kLeadFrames) % kLeadFrames;
	for (i = 0; i < n; i++) {
		lead_load(&lead_ring[(oldest + i) % kLeadFrames], p);
		SpcPlayer_Advance(p);
		publish(p);
		swiWaitForVBlank();
	}
	ZeldaAudioDropLeadIn();
}

void ZeldaAudioFrame(void)
{
	SpcPlayer *p = g_zenv.player;

	if (!ipc || !p || !p->dsp)
		return;
	/* The game posts one APU command per frame. Nothing else drains it. */
	ZeldaPopApuState();
	SpcPlayer_Advance(p);
	publish(p);
}

void ZeldaAudioHold(void)
{
	SpcPlayer *p = g_zenv.player;
	int was;

	if (!ipc || !p || !p->dsp)
		return;
	/* The menu frame still posts a command. Drop it so the song stays put. */
	ZeldaPopApuState();
	was = p->dsp->mute ? 1 : 0;
	p->dsp->mute = 1;
	publish(p);
	p->dsp->mute = was;
}
