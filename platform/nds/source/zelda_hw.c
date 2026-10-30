#include <nds.h>
#include <string.h>

#include "zelda_rtl.h"
#include "variables.h"
#include "snes/ppu.h"
#include "snes/snes_regs.h"
#include "zelda_save.h"
#include "zelda_bottom_map.h"

const uint16 *SrcPtr(uint16 src);

uint8_t dma_read(struct Dma *dma, uint16_t adr);

/*
 * Mode 1 picture on the DS 2D engine.
 *
 * SNES mode 1 is three tiled layers plus sprites. The DS main engine has
 * four tiled backgrounds and 128 hardware sprites, so the frame is tile
 * uploads, map entries, palettes, and OAM — not a software scanline plot.
 *
 * Back to front, matching PpuDrawBackgrounds in mode 1 with BG3 priority
 * (the only mode-1 setting this game writes, BGMODE 9):
 *   BG3 priority 1        HUD, in front of every sprite
 *   sprites priority 3
 *   BG1 priority 1        overhangs, and the file-select wallpaper
 *   BG2 priority 1        doorways and other tiles that cover Link
 *   sprites priority 2    Link
 *   BG1 priority 0
 *   BG2 priority 0        ground, in front of sprites 0 and 1
 *   sprites priority 1
 *   sprites priority 0
 *   BG3 priority 0        name letters. Behind the wallpaper.
 *   backdrop
 *
 * The DS has four backgrounds, so BG2 priority 1 stays on the ground layer
 * (it has its own scroll and cannot move onto BG1) and BG3 priority 0 shares
 * that hardware layer: the letters use it while BG2 is off. A subscreen-only
 * picture with nothing behind it (the title art) stays at priority 3. A
 * subscreen BG1 over an opaque BG2 is the rain and the other overworld
 * overlays: it is drawn in front of every sprite and alpha-blended, which
 * is what SNES half-add color math does. That only happens when color math
 * targets the ground. Hyrule Castle puts BG1 on the subscreen with CGADSUB
 * 0x20, backdrop only, so the layer stays behind the floor and is not
 * blended: the stone and Link stay solid. The stone bridge where the
 * bottle is given keeps that subscreen 256 pixels below the floor and
 * half-adds it. The shadow has to be drawn on the floor, in front of
 * it, or the bank covers it and the tint is gone. It stays behind
 * Link, so he and the man under the bridge stay solid. Rain is a
 * different subscreen, so it still draws in front. Walking the castle stairs
 * drops him to sprite priority 1, which exists to slip behind a
 * main-screen BG1 lip. These rooms have no such lip, so for that climb
 * only he stays in front of the stone. The arch and the wall around the
 * doorway are drawn over him for that walk. A lower stair also sets the
 * priority bit on the floor pattern beside the opening. That pattern
 * stays on the ground: lifting it reprints the floor above the door.
 * Priority 1 everywhere else stays behind the floor, which is how he
 * walks under a bridge.
 * The same climb
 * can take every sprite off the main screen and leave it on the
 * subscreen; those are still drawn. When that climb forces every
 * sprite to priority 3, a lower-floor monster stays behind the floor
 * so the bridge still covers him. A monster on the floor Link is
 * walking onto stays in front of that floor for the whole walk.
 * Link's own sprites stay in front of it. A ledge jump
 * raises Link to sprite priority 3 (sideways, south, and diagonal), and
 * that still has to sit
 * under the rain. A fixed color
 * add or subtract is applied to the palettes, which is the blue wash in
 * the opening, a dark room with no lamp, and a room lit by torches. The
 * lamp is BG1 on the subscreen, CGADSUB 0xB3: subtract, no half, on the
 * room and sprites. A transparent subscreen pixel subtracts the fixed
 * color (31, so the room goes black). An opaque cone pixel subtracts its
 * own color. The DS blender cannot subtract a layer, so the cone is
 * redrawn as light added onto a room already darkened by the cone's
 * brightest subtract, and a window blacks out everything the cone does
 * not cover. The status bar is outside that window, so it stays drawn
 * over the darkness. Master brightness is INIDISP: one
 * register, latched in vblank, including while a load is still running,
 * so the whole picture fades or cuts to black in one frame. Coming back
 * from black waits until that frame's tiles are uploaded, which is what
 * keeps the file select hidden until it is drawn. Mosaic uses
 * the DS mosaic register, so
 * the pixelated fade when leaving the house is visible. The door iris is
 * the SNES window HDMA table: outside the circle the picture is black, and
 * the circle closes on Link, then opens again outside. A shut line is not
 * an empty X range (the DS ignores that and shows the picture). The window
 * is latched before the room upload, including when that upload has
 * already reached the picture, so the shut frame stays black. Tree edges
 * are the backdrop showing through the tiles. The backdrop itself stays
 * black so the outside of the circle is black; the real color is a sprite
 * behind the backgrounds, inside the window only. Opening a floodgate and
 * draining the swamp use that same table with W12SEL 3: subscreen BG1
 * shows only inside the span, so the water line moves. Outside it the
 * floor, doors, sprites, and status bar stay. The world map is mode 7.
 * That is one 1024x1024 affine background: the same 8-bit tilemap and 8bpp
 * tiles the SNES stores. M7A and M7D are stored from an HBlank interrupt
 * before each scanline. Pixels past the 1024x1024 field are transparent,
 * so the map edge does not need a window.
 *
 * Naming a file scrolls the letter grid without moving the name. An IRQ at
 * SNES line 128 switches BG3 from the name scroll to selectfile_var8. That
 * scanline split is done here with an HBlank interrupt. The magic mirror is
 * the other one: HDMA replaces BG1 and BG2 horizontal scroll on every line
 * from the wave table, so the ground stretches into bands that travel down
 * the screen while the palette fades through white. Sprites are not in that
 * table, so Link stays put, and the status bar is BG3, so it stays too.
 *
 * Bank A (128KB), 16KB tile bases and 2KB map bases:
 *   0x00000 maps: HUD, BG1 high, BG1 low, BG2  (8KB each)
 *   0x08000 BG1 tiles
 *   0x10000 BG2 tiles
 *   0x18000 BG3 tiles
 * Bank E holds sprite characters, 128 bytes per OAM slot.
 */

enum {
	kCropY = 16,
	kBlankNone = -1,
	/* Status bar, below the cropped top of the picture. */
	kHudDrop = 3
};

/* The overworld camera (vendor overworld.c) may travel this many rows past the
 * top and bottom of an area so the cropped rows are never where Link walks. */
typedef char OwCameraPadMatchesCrop[(CAMERA_PAD_Y == kCropY) ? 1 : -1];

static int bg_id[4];
static uint8 bg_size[4];
static uint8 bg_tilebase[4];
static uint32 tile_hash[3][1024];
static uint32 map_sig[3];
static int map_valid[3];
static uint16 last_adr[3];
static uint8 used_bg1[1024];
static int blank_bg1 = kBlankNone;

/* 2bpp tiles are compacted. Slot 0 is a transparent tile.
 * bg3_lo is the priority-0 map (name letters), behind the wallpaper. */
static uint16 bg3_slot[8 * 1024];
/* The (tile, palette) key each slot was handed out for, so the per-frame
 * check walks the slots in use instead of all 8192 possible keys. */
static uint16 bg3_key[1024];
static int bg3_next;
static u16 bg3_lo[4096] __attribute__((aligned(4)));
static int back_mode;
static uint32 spr_hash[128];
/* Built in main RAM, then copied in one DMA. Writing each tile into sprite
 * VRAM while the screen is scanning tears the picture, and the hash would
 * keep that torn copy. A room of guards is enough to spill past vblank. */
static u16 spr_gfx[128 * 64] __attribute__((aligned(4)));
/* Slots whose tiles changed since the last copy. Only those are flushed and
 * sent: Link and a few effects change most frames, the rest do not. */
static uint8 spr_dirty_slot[128];
static int spr_dirty;

static u16 map_buf[4][4096] __attribute__((aligned(4)));
/* BG2 tiles with the priority bit. Doorways live here, in front of Link. */
static u16 bg2_hi[4096] __attribute__((aligned(4)));
static int bg2_blank;
static int bg2_lift_mode = -1;
static int bg2_front_on_slot1;
/* Doorway map built for the current BG1 and BG2 tilemaps. A stair climb
 * scrolls without changing those maps; rebuilding it every frame is what
 * stalls the walk. */
static int bake_valid;
/* Slot 1 is showing BG2 priority tiles copied onto BG1. Those tiles share
 * BG1's scroll, so they have to come back off when that scroll leaves BG2. */
static int bg1_baked;
/* Set while the vblank present may write tiles that are already showing.
 * The map itself is built earlier, into RAM. */
static int vram_commit;

/* What the next present will show. Filled before vblank, while the
 * previous picture is still up. */
static struct {
	int ready;
	int main_en, sub_en, cover;
	int bg1_main, bg1_sub, overlay, back_add, bridge;
	int bg2_on, lift_bg2, stair_arch, show_bg3_lo;
	int lamp, door_iris;
	int bar_under_overlay;
} pic;
/* Names in the room's floor pattern. A stair lip ORs priority onto that
 * pattern; lifting those cells paints the floor over the doorway. */
static uint8 floor_pattern[1024];
/* Names used widely with the priority bit clear. The floor is in this
 * set. So are the wall tiles, which the doorway then marks with priority. */
static uint8 common_floor[1024];
/* Cells of the doorway during a climb. The wall above the frame has no
 * priority bit, so it is marked here and drawn over Link. */
static uint8 stair_cover[64 * 64];
static uint8 stair_cap[64 * 64];

/* The VRAM words are summed up 32 bits at a time. A tilemap is up to 4096
 * words and the overworld hashes two of them every frame, so a halfword
 * loop with a mask on each load was the bulk of a still frame. The rotate
 * keeps two flips of the same bit in different words from cancelling. */
_Static_assert(offsetof(Ppu, vram) % 4 == 0, "VRAM must be word aligned");

static uint32 hash_words(const uint16 *vram, uint16 addr, int n) {
	const uint16 *p = vram + addr;
	uint32 h = 2166136261u;
	int i;

	if (!(addr & 1) && !(n & 1) && (unsigned)addr + (unsigned)n <= 0x8000u &&
	    !((uintptr_t)p & 3)) {
		const uint32 *q = (const uint32 *)p;
		int m = n >> 1;

		for (; m >= 4; m -= 4, q += 4) {
			h = ((h << 7 | h >> 25) ^ q[0]) * 16777619u;
			h = ((h << 7 | h >> 25) ^ q[1]) * 16777619u;
			h = ((h << 7 | h >> 25) ^ q[2]) * 16777619u;
			h = ((h << 7 | h >> 25) ^ q[3]) * 16777619u;
		}
		for (; m > 0; m--, q++)
			h = ((h << 7 | h >> 25) ^ *q) * 16777619u;
		return h ? h : 1u;
	}
	for (i = 0; i < n; i++)
		h = (h ^ vram[(addr + i) & 0x7fff]) * 16777619u;
	return h ? h : 1u;
}

/* The words of a tilemap, in order. VRAM is read in place unless the map
 * wraps past the end, which only the copy handles. */
static const uint16 *map_src(const Ppu *ppu, uint16 adr, int words) {
	static uint16 wrapped[4096];
	int i;

	if ((unsigned)adr + (unsigned)words <= 0x8000u)
		return ppu->vram + adr;
	for (i = 0; i < words; i++)
		wrapped[i] = ppu->vram[(adr + i) & 0x7fff];
	return wrapped;
}

/* One bitplane byte spread to eight nibbles, leftmost pixel in the lowest
 * one. A row of 4bpp is the four planes ORed in at bit 0..3 of each nibble:
 * four loads instead of sixty-four bit extractions. */
static uint32 plane_spread[256];
static int plane_spread_ready;

static void build_plane_spread(void) {
	int b, px;

	for (b = 0; b < 256; b++) {
		uint32 w = 0;

		for (px = 0; px < 8; px++)
			w |= (uint32)((b >> (7 - px)) & 1) << (px * 4);
		plane_spread[b] = w;
	}
	plane_spread_ready = 1;
}

/* A room's worth of sprites arrives on one frame when Link steps off a
 * stair. Every slot misses the hash and is converted before that blank,
 * so the cost of one tile is the cost of the hitch. */
static void conv4(const uint16 *src, u16 *dst, int pal_off) {
	int row;

	if (!pal_off) {
		if (!plane_spread_ready)
			build_plane_spread();
		for (row = 0; row < 8; row++) {
			uint32 lo = src[row];
			uint32 hi = src[row + 8];
			uint32 w = plane_spread[lo & 0xff] |
				(plane_spread[lo >> 8 & 0xff] << 1) |
				(plane_spread[hi & 0xff] << 2) |
				(plane_spread[hi >> 8 & 0xff] << 3);

			dst[row * 2] = (u16)w;
			dst[row * 2 + 1] = (u16)(w >> 16);
		}
		return;
	}
	for (row = 0; row < 8; row++) {
		uint8 plane0 = (uint8)src[row];
		uint8 plane1 = (uint8)(src[row] >> 8);
		uint8 plane2 = (uint8)src[row + 8];
		uint8 plane3 = (uint8)(src[row + 8] >> 8);
		uint8 bytes[4];
		int col;

		for (col = 0; col < 4; col++) {
			int bit0 = 7 - col * 2;
			int bit1 = bit0 - 1;
			uint32 px0 = ((plane0 >> bit0) & 1) |
				(((plane1 >> bit0) & 1) << 1) |
				(((plane2 >> bit0) & 1) << 2) |
				(((plane3 >> bit0) & 1) << 3);
			uint32 px1 = ((plane0 >> bit1) & 1) |
				(((plane1 >> bit1) & 1) << 1) |
				(((plane2 >> bit1) & 1) << 2) |
				(((plane3 >> bit1) & 1) << 3);
			if (pal_off) {
				if (px0)
					px0 += (uint32)pal_off;
				if (px1)
					px1 += (uint32)pal_off;
			}
			bytes[col] = (uint8)(px0 | (px1 << 4));
		}
		dst[row * 2] = (u16)(bytes[0] | (bytes[1] << 8));
		dst[row * 2 + 1] = (u16)(bytes[2] | (bytes[3] << 8));
	}
}

static void conv2(const uint16 *src, u16 *dst, int pal_off) {
	uint16 wide[16];
	int i;

	for (i = 0; i < 8; i++) {
		wide[i] = src[i];
		wide[i + 8] = 0;
	}
	conv4(wide, dst, pal_off);
}

static void clear_tile(u16 *gfx, int tile) {
	int i;
	u16 *dst = gfx + tile * 16;

	if (!vram_commit)
		return;
	for (i = 0; i < 16; i++)
		dst[i] = 0;
}

static void upload4_tile(const Ppu *ppu, int bank, u16 *gfx, uint16 tile_adr, int tile) {
	uint16 addr = (uint16)((tile_adr + tile * 16) & 0x7fff);
	uint32 h = hash_words(ppu->vram, addr, 16);
	uint16 tmp[16];
	int i;

	if (tile_hash[bank][tile] == h)
		return;
	/* Map entries are built ahead of the blank. The bytes they name are
	 * written in the blank, after the map copy, so a scan does not tear
	 * a tile that is already on screen. */
	if (!vram_commit)
		return;
	for (i = 0; i < 16; i++)
		tmp[i] = ppu->vram[(addr + i) & 0x7fff];
	conv4(tmp, gfx + tile * 16, 0);
	tile_hash[bank][tile] = h;
}

static inline u16 swizzle4(uint16 entry) {
	return (u16)((entry & 0x3ff) |
		((entry & 0x4000) >> 4) |
		((entry & 0x8000) >> 4) |
		(((entry >> 10) & 7) << 12));
}

/* Cone pixels keep their indices and move to palettes 8-15, which the
 * room does not use. Those colors are the light added back on top. */
static u16 swizzle_lamp(uint16 entry) {
	return (u16)((entry & 0x3ff) |
		((entry & 0x4000) >> 4) |
		((entry & 0x8000) >> 4) |
		((8 + ((entry >> 10) & 7)) << 12));
}

static int map_words(int wider, int higher) {
	return 1024 << ((wider ? 1 : 0) + (higher ? 1 : 0));
}

static BgSize map_bg_size(int wider, int higher) {
	if (wider && higher)
		return BgSize_T_512x512;
	if (wider)
		return BgSize_T_512x256;
	if (higher)
		return BgSize_T_256x512;
	return BgSize_T_256x256;
}

static uint32 sig_map(const Ppu *ppu, uint16 adr, int words) {
	return hash_words(ppu->vram, adr, words);
}

/* Text BG offsets take effect on the next scanline. They are committed at
 * the start of vblank. bgInit writes 0 into the layer it rebuilds, so the
 * committed offsets are written back before that 0 can cover a line. */
static int hw_scroll_x[4];
static int hw_scroll_y[4];
static int hw_scroll_valid;

static void hw_scroll_apply(void) {
	int i;

	if (!hw_scroll_valid)
		return;
	for (i = 0; i < 4; i++)
		bgSetScroll(bg_id[i], hw_scroll_x[i], hw_scroll_y[i]);
	bgUpdate();
}

static void ensure_size(int slot, int wider, int higher, int map_base, int tile_base) {
	uint8 sid = (uint8)((wider ? 1 : 0) | (higher ? 2 : 0));
	int ime;

	int show;
	unsigned pri;

	if (bg_size[slot] == sid && bg_tilebase[slot] == (uint8)tile_base)
		return;
	/* bgInit enables the layer and clears its priority. Put both back so a
	 * vblank commit does not leave the floor on top for the next picture. */
	show = (REG_DISPCNT & (1u << (8 + slot))) != 0;
	pri = (unsigned)(*bgControl[bg_id[slot]] & 3);
	ime = enterCriticalSection();
	bg_id[slot] = bgInit(slot, BgType_Text4bpp, map_bg_size(wider, higher), map_base, tile_base);
	bg_size[slot] = sid;
	bg_tilebase[slot] = (uint8)tile_base;
	bgSetPriority(bg_id[slot], pri);
	hw_scroll_apply();
	if (!show)
		bgHide(bg_id[slot]);
	leaveCriticalSection(ime);
}

/* Map copies wait until vblank. A stair step rebuilds the front layer and
 * the floor map, and copying either into VRAM while the picture is scanning
 * snows the whole frame: it is the same bank the display is reading. */
static struct {
	const u16 *src;
	int words;
	int wider;
	int higher;
	int map_base;
	int tile_base;
	int on;
} map_job[4];

/* What the doorway layer's map in VRAM was last filled from. The overworld
 * puts the same bg2_hi there every frame; copying 8KB each time it is
 * unchanged cost a flush and a DMA that held the CPU for nothing. */
static uint32 bg2_hi_ver;
static struct {
	const u16 *src;
	uint32 ver;
	int words;
	int wider;
	int higher;
} slot1_up;

static int vblank_margin(void) {
	unsigned line = REG_VCOUNT;

	return line >= 192 && line < 250;
}

static void flush_map_jobs(void) {
	int slot;

	if (!vblank_margin())
		return;
	for (slot = 0; slot < 4; slot++) {
		int bytes;

		if (!map_job[slot].on)
			continue;
		ensure_size(slot, map_job[slot].wider, map_job[slot].higher,
			map_job[slot].map_base, map_job[slot].tile_base);
		bytes = map_job[slot].words * 2;
		DC_FlushRange((void *)map_job[slot].src, (uint32)bytes);
		dmaCopyWords(3, (void *)map_job[slot].src, bgGetMapPtr(bg_id[slot]),
			(uint32)bytes);
		if (slot == 1) {
			slot1_up.src = map_job[1].src;
			slot1_up.ver = bg2_hi_ver;
			slot1_up.words = map_job[1].words;
			slot1_up.wider = map_job[1].wider;
			slot1_up.higher = map_job[1].higher;
		}
		map_job[slot].on = 0;
	}
}

static void queue_map(int slot, const u16 *src, int words,
	int wider, int higher, int map_base, int tile_base) {
	map_job[slot].src = src;
	map_job[slot].words = words;
	map_job[slot].wider = wider;
	map_job[slot].higher = higher;
	map_job[slot].map_base = map_base;
	map_job[slot].tile_base = tile_base;
	map_job[slot].on = 1;
}

static int bg1_split_mode = -1;

static void present_bg1(Ppu *ppu, int split, int lamp) {
	BgLayer *layer = &ppu->bgLayer[0];
	int words = map_words(layer->tilemapWider, layer->tilemapHigher);
	uint32 sig = sig_map(ppu, layer->tilemapAdr, words);
	u16 *gfx;
	int tile;

	/* Slot 1 stays on BG2 graphics while it is showing doorways. BG1 tiles
	 * always live at tile base 2, with or without the hardware layer
	 * pointing there yet. */
	gfx = BG_GFX + 2 * 8192;

	if (last_adr[0] != layer->tileAdr) {
		memset(tile_hash[0], 0, sizeof tile_hash[0]);
		last_adr[0] = layer->tileAdr;
		map_valid[0] = 0;
	}
	if (bg1_split_mode != split + lamp * 2) {
		bg1_split_mode = split + lamp * 2;
		map_valid[0] = 0;
	}

	/* The tiles a map uses only change when the map does, so they are
	 * counted when it is rebuilt, not on every frame it is reused. */
	if (!map_valid[0] || map_sig[0] != sig) {
		const uint16 *src = map_src(ppu, layer->tilemapAdr, words);
		u16 blank;
		int i;

		memset(used_bg1, 0, sizeof used_bg1);
		for (i = 0; i < words; i++)
			used_bg1[src[i] & 0x3ff] = 1;
		bake_valid = 0;
		blank_bg1 = kBlankNone;
		for (i = 0; i < 1024; i++) {
			if (!used_bg1[i]) {
				blank_bg1 = i;
				break;
			}
		}
		if (!split)
			blank_bg1 = kBlankNone;
		blank = (u16)(blank_bg1 < 0 ? 0 : blank_bg1);
		if (!split) {
			memset(map_buf[1], 0, (size_t)words * sizeof(u16));
			for (i = 0; i < words; i++)
				map_buf[2][i] = lamp ? swizzle_lamp(src[i]) : swizzle4(src[i]);
		} else {
			for (i = 0; i < words; i++) {
				uint16 entry = src[i];

				if (entry & 0x2000) {
					map_buf[1][i] = swizzle4(entry);
					map_buf[2][i] = blank;
				} else {
					map_buf[1][i] = blank;
					map_buf[2][i] = swizzle4(entry);
				}
			}
		}
		queue_map(2, map_buf[2], words, layer->tilemapWider, layer->tilemapHigher, 8, 2);
		if (split)
			queue_map(1, map_buf[1], words, layer->tilemapWider, layer->tilemapHigher, 4, 2);
		map_sig[0] = sig;
		map_valid[0] = 1;
	}

	/* The unused index fills the other priority layer. It has to stay
	 * transparent even when a previous upload left pixels there and the
	 * hash was cleared, or those pixels paint over the logo and sprites. */
	if (blank_bg1 >= 0 && !used_bg1[blank_bg1]) {
		clear_tile(gfx, blank_bg1);
		if (vram_commit)
			tile_hash[0][blank_bg1] = 0;
	}

	for (tile = 0; tile < 1024; tile++) {
		if (!used_bg1[tile] || tile == blank_bg1)
			continue;
		upload4_tile(ppu, 0, gfx, layer->tileAdr, tile);
	}
}

/* The floor filler is eight tiles, drawn on both layers. Anything else
 * that carries the priority bit is a wall or a cap. */
static void mark_floor_pattern(void) {
	uint16 offs[2];
	int n, i;

	memset(floor_pattern, 0, sizeof floor_pattern);
	offs[0] = dung_floor_1_filler_tiles;
	offs[1] = dung_floor_2_filler_tiles;
	for (n = 0; n < 2; n++) {
		const uint16 *src;

		if ((offs[n] & 0xf) || offs[n] > 0xf0)
			continue;
		src = SrcPtr(offs[n]);
		for (i = 0; i < 8; i++)
			floor_pattern[src[i] & 0x3ff] = 1;
	}
}

/* SNES tilemaps store a wide or tall room as 32x32 quadrants. */
static void map_xy(int i, int wider, int higher, int *x, int *y) {
	int sub;

	if (wider && higher) {
		int quad = i >> 10;

		sub = i & 0x3ff;
		*x = (sub & 31) + ((quad & 1) ? 32 : 0);
		*y = ((sub >> 5) & 31) + ((quad & 2) ? 32 : 0);
		return;
	}
	sub = i & 0x3ff;
	if (wider) {
		*x = (sub & 31) + ((i & 0x400) ? 32 : 0);
		*y = (sub >> 5) & 31;
		return;
	}
	if (higher) {
		*x = sub & 31;
		*y = ((sub >> 5) & 31) + ((i & 0x400) ? 32 : 0);
		return;
	}
	*x = i & 31;
	*y = (i >> 5) & 31;
}

/* Stair and door caps. A vflip cap is the south edge of the opening. */
static int arch_cap(int name) {
	switch (name) {
	case 0x02c: case 0x02d:
	case 0x088: case 0x089:
	case 0x09d:
	case 0x0b5: case 0x0b6: case 0x0b7: case 0x0b8: case 0x0b9:
		return 1;
	}
	return 0;
}

/* The blocks of the doorway. Both columns of a north wall are here.
 * The step (0x0ac), the open hole, and the fill inside a straight
 * flight (0x007, 0x016, 0x017) are not. Link walks on that fill. */
static int doorway_wall(int name) {
	switch (name) {
	case 0x008: case 0x00d: case 0x010: case 0x018: case 0x078:
	case 0x050: case 0x051: case 0x060: case 0x061: case 0x070:
	case 0x090: case 0x091: case 0x096: case 0x097: case 0x0a2: case 0x0a3:
		return 1;
	}
	return 0;
}

/* Set once the climb masks are built during this lay_maps. The doorway
 * bake used to scan the tilemaps again on the frame a stair loads a room. */
static int climb_marked;

static int keep_under_link(int name) {
	if (name == 0 || name == 0x1ef || name == 0x0ac)
		return 1;
	if (name == 0x007 || name == 0x016 || name == 0x017)
		return 1;
	if (floor_pattern[name])
		return 1;
	if (common_floor[name])
		return 1;
	return 0;
}

static int stair_wall_cell(int x, int y) {
	if ((unsigned)x >= 64 || (unsigned)y >= 64)
		return 0;
	return stair_cover[y * 64 + x];
}

/* 1 = the opening's own columns. 2 = the wall column on either side.
 * The step tile is the same name as the wall base, so only a side
 * column brings that name forward. */
static void mark_cover_cell(int x, int y, int kind) {
	int i;

	if ((unsigned)x >= 64 || (unsigned)y >= 64)
		return;
	i = y * 64 + x;
	if (kind == 2 && stair_cover[i] == 1)
		return;
	stair_cover[i] = (uint8)kind;
}

static void mark_cover_column(int x, int y, int vflip, int kind) {
	int dy;

	mark_cover_cell(x, y, kind);
	for (dy = 1; dy <= 4; dy++)
		mark_cover_cell(x, y - dy, kind);
	/* The jamb is two rows under a north cap. A south cap is vflipped
	 * and the floor below it stays down. */
	if (!vflip) {
		mark_cover_cell(x, y + 1, kind);
		mark_cover_cell(x, y + 2, kind);
	}
}

/* Floor count and the doorway band share one read of each tilemap.
 * A second pass over VRAM here is what hitched the room a stair enters. */
static void mark_climb(const Ppu *ppu) {
	static uint8 count[1024];
	int layer, i;

	if (climb_marked)
		return;
	climb_marked = 1;
	memset(count, 0, sizeof count);
	memset(common_floor, 0, sizeof common_floor);
	memset(stair_cover, 0, sizeof stair_cover);
	memset(stair_cap, 0, sizeof stair_cap);
	for (layer = 0; layer < 2; layer++) {
		const BgLayer *bg = &ppu->bgLayer[layer];
		int words = map_words(bg->tilemapWider, bg->tilemapHigher);
		const uint16 *src = map_src(ppu, bg->tilemapAdr, words);

		for (i = 0; i < words; i++) {
			uint16 entry = src[i];
			int name = entry & 0x3ff;
			int x, y;

			if (!(entry & 0x2000)) {
				if (count[name] < 255)
					count[name]++;
				continue;
			}
			if (!arch_cap(name))
				continue;
			map_xy(i, bg->tilemapWider, bg->tilemapHigher, &x, &y);
			if ((unsigned)x >= 64 || (unsigned)y >= 64)
				continue;
			/* 1 = north cap, 2 = south cap stored vflipped. */
			stair_cap[y * 64 + x] = (entry & 0x8000) ? 2 : 1;
			mark_cover_column(x, y, entry & 0x8000, 1);
		}
	}
	for (i = 0; i < 1024; i++)
		if (count[i] >= 8)
			common_floor[i] = 1;
	for (i = 0; i < 64 * 64; i++) {
		int cap = stair_cap[i];
		int x, y;

		if (!cap)
			continue;
		x = i & 63;
		y = i >> 6;
		if (x > 0 && !stair_cap[i - 1])
			mark_cover_column(x - 1, y, cap == 2, 2);
		if (x + 1 < 64 && !stair_cap[i + 1])
			mark_cover_column(x + 1, y, cap == 2, 2);
	}
}

static void present_bg2(Ppu *ppu, int lift, int keep_floor) {
	BgLayer *layer = &ppu->bgLayer[1];
	int words = map_words(layer->tilemapWider, layer->tilemapHigher);
	uint32 sig = sig_map(ppu, layer->tilemapAdr, words);
	/* Counted when the map is rebuilt and kept for the frames that reuse it. */
	static uint8 used[1024];
	u16 *gfx;
	int tile;

	gfx = BG_GFX + 4 * 8192;

	if (last_adr[1] != layer->tileAdr) {
		memset(tile_hash[1], 0, sizeof tile_hash[1]);
		last_adr[1] = layer->tileAdr;
		map_valid[1] = 0;
	}
	if (bg2_lift_mode != lift + keep_floor * 2) {
		bg2_lift_mode = lift + keep_floor * 2;
		map_valid[1] = 0;
	}

	if (!map_valid[1] || map_sig[1] != sig) {
		const uint16 *src = map_src(ppu, layer->tilemapAdr, words);
		u16 blank;
		int i;

		/* The floor set is the room's filler, not every repeated name.
		 * It does not change on the frames where this map is reused. */
		bake_valid = 0;
		if (keep_floor) {
			mark_floor_pattern();
			mark_climb(ppu);
		} else {
			memset(floor_pattern, 0, sizeof floor_pattern);
			memset(common_floor, 0, sizeof common_floor);
			memset(stair_cover, 0, sizeof stair_cover);
		}
		memset(used, 0, sizeof used);
		for (i = 0; i < words; i++)
			used[src[i] & 0x3ff] = 1;
		bg2_blank = 0;
		for (i = 1; i < 1024; i++) {
			if (!used[i]) {
				bg2_blank = i;
				break;
			}
		}
		blank = (u16)bg2_blank;
		/* Same split as PpuDrawBackground: 0xb100 is in front of Link,
		 * 0x7100 stays on the ground behind him. During a stair climb the
		 * jamb and the wall across the top of the opening come forward
		 * with the cap. The floor pattern stays on the ground. */
		if (!lift) {
			for (i = 0; i < words; i++) {
				bg2_hi[i] = blank;
				map_buf[3][i] = swizzle4(src[i]);
			}
		} else {
			for (i = 0; i < words; i++) {
				uint16 entry = src[i];
				int name = entry & 0x3ff;
				int x, y, cover = 0;

				if (keep_floor && (doorway_wall(name) || name == 0x0ac)) {
					int cell;

					map_xy(i, layer->tilemapWider,
						layer->tilemapHigher, &x, &y);
					cell = stair_wall_cell(x, y);
					/* The step shares its name with the wall base.
					 * Beside the opening that base covers him. Under
					 * the opening it is the stair, and he stands on it. */
					if (cell && doorway_wall(name))
						cover = 1;
					else if (cell == 2 && name == 0x0ac)
						cover = 1;
				}
				/* Priority floor stays on the ground, under the
				 * sprites. The doorway wall comes forward. */
				if (((entry & 0x2000) &&
				     !(keep_floor && keep_under_link(name))) || cover) {
					bg2_hi[i] = swizzle4(entry);
					map_buf[3][i] = blank;
				} else {
					bg2_hi[i] = blank;
					map_buf[3][i] = swizzle4(entry);
				}
			}
		}
		bg2_hi_ver++;
		queue_map(3, map_buf[3], words, layer->tilemapWider, layer->tilemapHigher, 12, 4);
		map_sig[1] = sig;
		map_valid[1] = 1;
	}

	if (!used[bg2_blank]) {
		clear_tile(gfx, bg2_blank);
		if (vram_commit)
			tile_hash[1][bg2_blank] = 0;
	}

	for (tile = 0; tile < 1024; tile++) {
		if (used[tile])
			upload4_tile(ppu, 1, gfx, layer->tileAdr, tile);
	}
}

static int bg3_lookup(int key) {
	int slot = bg3_slot[key];

	if (slot)
		return slot - 1;
	if (bg3_next >= 1024)
		return -1;
	slot = bg3_next++;
	bg3_slot[key] = (uint16)(slot + 1);
	bg3_key[slot] = (uint16)key;
	return slot;
}

/* Hearts, magic, and the item box. Text boxes and the inventory grid
 * are other BG3 tiles, so this strip is the status bar alone. */
static int hud_strip_hidden(void)
{
	int mod;

	if (ZeldaHudOnTop())
		return 0;
	mod = main_module_index;
	if (mod == 14)
		mod = saved_module_for_menu;
	/* 5 is loading a save. Save and Continue, and Don't Save and
	 * Continue, set this a frame before the load blanks the picture,
	 * and that frame is still the game over screen. 7 indoors, 9
	 * outdoors, 11 the special overworld (lost woods). 15 and 16 are
	 * the door iris: that circle leaves play while it is still open
	 * across the status bar. 17 is a fall down a hole, 18 is death,
	 * 19 a pendant and 22 a crystal. 21 is Agahnim sending Link to
	 * the dark world, through Sahasrahla's telepathy. The bottom
	 * screen is the idle picture for that, so the bar stays off this
	 * screen too. 23 is save and quit: the playfield stays up while
	 * the screen fades. 24 is Agahnim's defeat: the flute bird, then
	 * the bat at the pyramid. The bottom screen is the idle picture
	 * for that, so the bar stays off this screen too. 25 is the triforce
	 * chamber: the speech is on this layer, and the bar stays on the
	 * bottom screen. */
	switch (mod) {
	case 5:
	case 7:
	case 9:
	case 11:
	case 15:
	case 16:
	case 17:
	case 18:
	case 19:
	case 21:
	case 22:
	case 23:
	case 24:
	case 25:
		return 1;
	default:
		return 0;
	}
}

/* With the HUD on the bottom screen, the item menu is drawn there. The top
 * screen keeps the frozen game and none of the menu's BG3. */
static int item_menu_hidden(void)
{
	return !ZeldaHudOnTop() && main_module_index == 14 && submodule_index == 1;
}

static void present_bg3(Ppu *ppu, int show_low) {
	BgLayer *layer = &ppu->bgLayer[2];
	int words = map_words(layer->tilemapWider, layer->tilemapHigher);
	uint32 sig = sig_map(ppu, layer->tilemapAdr, words);
	u16 *gfx;
	int i;
	int hide;
	int hide_all;
	unsigned hud_at;
	static int hud_hide_applied = -1;
	static int menu_hide_applied = -1;

	gfx = BG_GFX + 6 * 8192;
	hide = hud_strip_hidden();
	hide_all = item_menu_hidden();
	if (hide != hud_hide_applied || hide_all != menu_hide_applied) {
		hud_hide_applied = hide;
		menu_hide_applied = hide_all;
		map_valid[2] = 0;
	}

	if (last_adr[2] != layer->tileAdr) {
		memset(tile_hash[2], 0, sizeof tile_hash[2]);
		memset(bg3_slot, 0, sizeof bg3_slot);
		bg3_next = 1;
		last_adr[2] = layer->tileAdr;
		map_valid[2] = 0;
	}

	if (!map_valid[2] || map_sig[2] != sig) {
		hud_at = word_7E0219 ? word_7E0219 : 0x6040;
		for (i = 0; i < words; i++) {
			unsigned at = (layer->tilemapAdr + i) & 0x7fff;
			uint16 entry;
			int tile;
			int pal;
			int slot;
			u16 out;

			if (hide_all || (hide && at >= hud_at && at < hud_at + 165)) {
				map_buf[0][i] = 0;
				bg3_lo[i] = 0;
				continue;
			}
			entry = ppu->vram[at];
			tile = entry & 0x3ff;
			pal = (entry >> 10) & 7;
			slot = bg3_lookup((tile << 3) | pal);
			if (slot < 0) {
				map_buf[0][i] = 0;
				bg3_lo[i] = 0;
				continue;
			}
			out = (u16)slot;
			if (entry & 0x4000)
				out |= 1 << 10;
			if (entry & 0x8000)
				out |= 1 << 11;
			out |= (u16)((pal >> 2) << 12);
			/* Priority 1 is the HUD, in front of sprites. Priority 0 is
			 * the name text, which the wallpaper has to cover. */
			if (entry & 0x2000) {
				map_buf[0][i] = out;
				bg3_lo[i] = 0;
			} else {
				map_buf[0][i] = 0;
				bg3_lo[i] = out;
			}
		}
		queue_map(0, map_buf[0], words, layer->tilemapWider, layer->tilemapHigher, 0, 6);
		if (show_low)
			queue_map(3, bg3_lo, words, layer->tilemapWider, layer->tilemapHigher, 12, 6);
		map_sig[2] = sig;
		map_valid[2] = 1;
	}

	for (i = 1; i < bg3_next; i++) {
		int slot = i;
		int key = bg3_key[i];
		int tile;
		int pal;
		uint16 addr;
		uint32 h;
		uint16 tmp[8];
		int w;

		tile = key >> 3;
		pal = key & 7;
		addr = (uint16)((layer->tileAdr + tile * 8) & 0x7fff);
		h = hash_words(ppu->vram, addr, 8) ^ (uint32)(pal + 1);
		if (tile_hash[2][slot] == h)
			continue;
		if (!vram_commit)
			continue;
		for (w = 0; w < 8; w++)
			tmp[w] = ppu->vram[(addr + w) & 0x7fff];
		conv2(tmp, gfx + slot * 16, (pal & 3) * 4);
		tile_hash[2][slot] = h;
	}
}

/* Palettes stay at full intensity. INIDISP is REG_MASTER_BRIGHT, which
 * scales the finished picture the way the SNES brightness register does.
 * 0..15 maps onto the DS fade factor 16..0. */
/* Extra darkening over the whole top screen, 0 none .. 16 black. The
 * bottom screen's item menu dims the frozen game behind it with this. It
 * stacks on whatever the game or a fade is already doing. */
static volatile int menu_dim;
/* Screen-change fade (title to file select to spawn select). Stacks the
 * same way, and the larger of the two wins. */
static volatile int trans_dim;
/* While set, the vblank handler keeps the brightness of the last picture
 * that was shown instead of reading INIDISP, which already belongs to a
 * frame that has not been presented yet. */
static volatile int bright_hold;
static volatile unsigned last_inidisp = 0x80;

void ZeldaHwSetDim(int level)
{
	if (level < 0)
		level = 0;
	if (level > 16)
		level = 16;
	menu_dim = level;
}

void ZeldaHwSetTransition(int level)
{
	if (level < 0)
		level = 0;
	if (level > 16)
		level = 16;
	trans_dim = level;
}

void ZeldaHwHoldBrightness(int on)
{
	bright_hold = on;
}

/* The last presented picture was already black (forced blank or brightness 0). */
int ZeldaHwShownBlack(void)
{
	unsigned ini = last_inidisp;

	return (ini & 0x80) || (ini & 0xf) == 0;
}

/* dark is the DS fade factor, 0 clear .. 16 black. */
static u16 bright_reg(unsigned dark)
{
	unsigned dim = (unsigned)(menu_dim > trans_dim ? menu_dim : trans_dim);
	unsigned d = dark + ((16u - dark) * dim + 8u) / 16u;

	if (d > 16)
		d = 16;
	return d ? (u16)((1u << 15) | d) : 0;
}

static u16 inidisp_to_bright(unsigned inidisp) {
	unsigned b;

	if (inidisp & 0x80)
		return bright_reg(16);
	b = inidisp & 0xf;
	if (b >= 15)
		return bright_reg(0);
	if (b == 0)
		return bright_reg(16);
	return bright_reg((16u * (15u - b) + 7u) / 15u);
}

/* The load that follows EnableForceBlank runs for several vblanks. The
 * SNES writes INIDISP in NMI anyway, so the picture is already black
 * while that load is still going. picture_covered stays set until the
 * present that leaves that blank has finished copying tiles. */
static volatile int use_master_bright = 1;
static volatile int picture_covered = 1;
/* 0 visible .. 16 black. Negative leaves brightness to the game. */
static volatile int fade_level = -1;

static void apply_forced_fade(void)
{
	int level = fade_level;

	if (level < 0)
		return;
	if (level > 16)
		level = 16;
	REG_MASTER_BRIGHT = bright_reg((unsigned)level);
}

void ZeldaHwSetFade(int level)
{
	fade_level = level;
	if (level >= 0)
		apply_forced_fade();
}

static unsigned shown_inidisp(unsigned inidisp) {
	if (picture_covered && !(inidisp & 0x80))
		return 0x80;
	return inidisp;
}

static void vblank_bright(void) {
	ZeldaMapRainVblank();
	if (fade_level >= 0) {
		apply_forced_fade();
		return;
	}
	if (bright_hold) {
		if (use_master_bright)
			REG_MASTER_BRIGHT = inidisp_to_bright(last_inidisp);
		return;
	}
	if (use_master_bright)
		REG_MASTER_BRIGHT = inidisp_to_bright(shown_inidisp(INIDISP_copy));
}

void ZeldaHwUseMasterBright(int on) {
	use_master_bright = on;
	if (fade_level >= 0) {
		apply_forced_fade();
		return;
	}
	if (on)
		REG_MASTER_BRIGHT = inidisp_to_bright(shown_inidisp(INIDISP_copy));
	else
		REG_MASTER_BRIGHT = 0;
}

/* FileSelect_TriggerNameStripesAndAdvance clears the force blank in the
 * same frame it queues the background. The save names are copied on the
 * next frame, inside FileSelect_Main. Hold this frame black so the first
 * picture is the one with those names, not the empty background. */
static int file_select_primed;

static int file_select_not_ready(const Ppu *ppu) {
	if (main_module_index != 1 || ppu->forcedBlank) {
		file_select_primed = 0;
		return 0;
	}
	if (file_select_primed)
		return 0;
	file_select_primed = 1;
	return 1;
}

static int clamp5(int v) {
	if (v < 0)
		return 0;
	if (v > 31)
		return 31;
	return v;
}

/* Fixed-color math. A transparent subscreen keeps the fixed color and
 * skips the half; half only applies when the subscreen itself is off. */
static u16 math_color(u16 color, const Ppu *ppu) {
	int r = color & 31;
	int g = (color >> 5) & 31;
	int b = (color >> 10) & 31;

	if (ppu->subtractColor) {
		r -= ppu->fixedColorR;
		g -= ppu->fixedColorG;
		b -= ppu->fixedColorB;
	} else {
		r += ppu->fixedColorR;
		g += ppu->fixedColorG;
		b += ppu->fixedColorB;
	}
	if (ppu->halfColor && !ppu->addSubscreen) {
		r >>= 1;
		g >>= 1;
		b >>= 1;
	}
	return (u16)(clamp5(r) | (clamp5(g) << 5) | (clamp5(b) << 10));
}

static int iris_wanted(void);

/* Set by lamp_prepare. The cone is up, so palettes are the darkened
 * base and (when the subtract stays in range) the light layer is on. */
static int lamp_dim;
static int lamp_show_light;
static uint8 lamp_cmax_r, lamp_cmax_g, lamp_cmax_b;

static void lamp_prepare(const Ppu *ppu);
static void lamp_write_colors(void);

/* BG1 subscreen, subtract, fixed color 31. CGADSUB 0xB3 has no half
 * bit: transparent pixels subtract 31 (black), opaque cone pixels
 * subtract their own color. Torches and the opening tint are the same
 * subtract with the subscreen off, so they take the flat path. The
 * door iris owns the window while it is running. */
static int lamp_cone(const Ppu *ppu) {
	if (iris_wanted() || ppu->forcedBlank || ppu->preventMathMode == 3)
		return 0;
	if (!player_is_indoors)
		return 0;
	if (!ppu->subtractColor || ppu->halfColor || !ppu->addSubscreen)
		return 0;
	if ((ppu->screenEnabled[1] & 0x01) == 0 || (ppu->screenEnabled[0] & 0x01))
		return 0;
	if ((ppu->mathEnabled & 0x13) == 0)
		return 0;
	return ppu->fixedColorR == 31 && ppu->fixedColorG == 31 &&
		ppu->fixedColorB == 31;
}

static int math_active(const Ppu *ppu) {
	if (lamp_cone(ppu))
		return 0;
	if (ppu->preventMathMode == 3 || (ppu->mathEnabled & 0x3f) == 0)
		return 0;
	if (ppu->fixedColorR | ppu->fixedColorG | ppu->fixedColorB)
		return 1;
	return ppu->halfColor && !ppu->addSubscreen;
}

/* main - sub, clamped. The lantern does not halve. */
static int sub_sat(int main_c, int sub_c) {
	if (main_c <= sub_c)
		return 0;
	return main_c - sub_c;
}

static u16 shade_base(u16 color) {
	return (u16)(sub_sat(color & 31, lamp_cmax_r) |
		(sub_sat((color >> 5) & 31, lamp_cmax_g) << 5) |
		(sub_sat((color >> 10) & 31, lamp_cmax_b) << 10));
}

/* BG3 is colors 0-31 (the status bar). BG1 and BG2 are the room, loaded
 * from color 32 up. Indoor subtract must not take the bar with the room. */
static int bake_bg_color(const Ppu *ppu, int i) {
	if (i < 32)
		return (ppu->mathEnabled & 0x04) != 0 ||
			!(player_is_indoors && ppu->subtractColor);
	return (ppu->mathEnabled & 0x03) != 0;
}

static void present_palettes(const Ppu *ppu) {
	int i;
	int on;
	int bg;
	int obj;
	int back;
	u16 color;

	if (lamp_cone(ppu)) {
		int sprites = (ppu->mathEnabled & 0x10) != 0;

		lamp_prepare(ppu);
		BG_PALETTE[0] = 0;
		for (i = 1; i < 128; i++) {
			color = ppu->cgram[i];
			if (lamp_dim && i >= 32 && (ppu->mathEnabled & 0x03))
				color = shade_base(color);
			BG_PALETTE[i] = (u16)(color & 0x7fff);
		}
		for (i = 0; i < 128; i++) {
			color = ppu->cgram[128 + i];
			if (lamp_dim && sprites)
				color = shade_base(color);
			SPRITE_PALETTE[i] = (u16)(color & 0x7fff);
		}
		lamp_write_colors();
		return;
	}

	on = math_active(ppu);
	bg = on && (ppu->mathEnabled & 0x07);
	obj = on && (ppu->mathEnabled & 0x10);
	back = on && (ppu->mathEnabled & 0x20);
	color = back ? math_color(ppu->cgram[0], ppu) : ppu->cgram[0];
	/* The backdrop cannot be windowed. While the iris is up, or the SNES
	 * picture is forced blank, it has to stay black. */
	BG_PALETTE[0] = (iris_wanted() || ppu->forcedBlank) ? 0 :
		(u16)(color & 0x7fff);
	for (i = 1; i < 128; i++) {
		int bake = bg && bake_bg_color(ppu, i);

		color = bake ? math_color(ppu->cgram[i], ppu) : ppu->cgram[i];
		BG_PALETTE[i] = (u16)(color & 0x7fff);
	}
	for (i = 0; i < 128; i++) {
		color = obj ? math_color(ppu->cgram[128 + i], ppu) : ppu->cgram[128 + i];
		SPRITE_PALETTE[i] = (u16)(color & 0x7fff);
	}
}

/* SNES mosaic size 1 is one pixel. The DS register is that size minus one. */
static void present_mosaic(const Ppu *ppu) {
	int i;
	int size;

	if (ppu->forcedBlank || ppu->mosaicSize <= 1 || (ppu->mosaicEnabled & 7) == 0) {
		for (i = 0; i < 4; i++)
			bgMosaicDisable(bg_id[i]);
		bgSetMosaic(0, 0);
		return;
	}
	size = ppu->mosaicSize - 1;
	if (size > 15)
		size = 15;
	bgSetMosaic((unsigned)size, (unsigned)size);
	for (i = 0; i < 4; i++)
		bgMosaicEnable(bg_id[i]);
}

/* Half-add is (front + back) / 2. A full add clamps the sum.
 * overlay: the subscreen is in front of every sprite (rain).
 * back_add: Translucent and Addition rooms. The subscreen sits on the
 * floor, in front of it and behind Link, and only its opaque pixels are
 * mixed in. A transparent subscreen pixel leaves the tile alone. Mixing
 * that tile with the backdrop instead is what halved every color.
 * Sprites are not a destination, so Link stays solid.
 * bridge: the subscreen is in front of the floor and behind the sprites.
 * Only the floor is a destination, so the shadow stays on the bank and
 * Link stays solid. */
static void present_overlay_blend(const Ppu *ppu, int overlay, int back_add,
		int bridge) {
	int coef;

	/* Cone light is hardware BG2. Adding it (coeff 16+16) onto the room
	 * and sprites is (base + light), clipped. The status bar is not a
	 * destination, and it sits in front of this layer. */
	if (lamp_show_light) {
		REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 |
			BLEND_DST_BG1 | BLEND_DST_BG3 | BLEND_DST_SPRITE |
			BLEND_DST_BACKDROP;
		REG_BLDALPHA = 16 | (16 << 8);
		return;
	}
	if (ppu->subtractColor || (!overlay && !back_add && !bridge)) {
		REG_BLDCNT = 0;
		return;
	}
	coef = ppu->halfColor ? 8 : 16;
	if (bridge) {
		REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 |
			BLEND_DST_BG3 | BLEND_DST_BACKDROP;
	} else if (back_add) {
		/* Hardware BG2 is the subscreen. It is the source, so a clear
		 * pixel does not blend and the floor under it stays full bright.
		 * Doorways are not a source: they sit in front of this and stay
		 * solid. */
		REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 |
			BLEND_DST_BG3 | BLEND_DST_BACKDROP;
	} else {
		REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 |
			BLEND_DST_BG0 | BLEND_DST_BG1 | BLEND_DST_BG3 |
			BLEND_DST_SPRITE | BLEND_DST_BACKDROP;
	}
	REG_BLDALPHA = (u16)(coef | (coef << 8));
}

static int obj_tile(int name, int tx, int ty) {
	return (((name >> 4) + ty) << 4) | (((name & 0xf) + tx) & 0xf);
}

/* Dungeon modules that are a stair climb. Priority 1 during these is Link
 * passing the lip. It is not the under-a-bridge or lower-floor priority. */
static int climbing_stairs(void) {
	if (main_module_index != 7)
		return 0;
	switch (submodule_index) {
	case 6:  /* fat inter-room */
	case 8:  /* north intra-room */
	case 14: /* spiral */
	case 16: /* south intra-room */
	case 17: /* straight inter-room */
	case 18:
	case 19:
		return 1;
	default:
		return 0;
	}
}

/* LinkOam_Main writes his body, sword, shield, and shadow into this block.
 * The regions monsters allocate from stop on either side of it. */
static int link_oam_slot(int sprite) {
	int base = (int)sort_sprites_offset_into_oam_buffer >> 2;

	return sprite >= base && sprite < base + 12;
}

/* Which floor owns each OAM slot. 0xff is not one of those monsters. */
extern uint8 sprite_oam_floor[128];

static int ds_sprite_priority(int snes_priority, int bg1_main, int sprite) {
	/* Same interleave as PpuDrawBackgrounds: 3 above BG1-high, 2 above
	 * BG1-low, 0 and 1 above only the back layer. Priority 1 sits under a
	 * main-screen BG1 lip, and under a bridge. Hyrule Castle has no
	 * main-screen BG1, so that priority would bury Link under the only
	 * floor. Raise his block for the climb. A monster on the lower floor
	 * uses the same priority to stay under a bridge, and stays there.
	 * A spiral climb off the lower floor forces every sprite to priority 3.
	 * On the main screen that draws a lower-floor monster over a bridge,
	 * so he stays behind the floor. A monster standing on the floor Link
	 * is walking onto keeps that floor's priority and is visible while
	 * the walk is still going. */
	int link = link_oam_slot(sprite);

	if (snes_priority >= 3) {
		if (link_is_on_lower_level == 3 && !link) {
			if (sprite_oam_floor[sprite] == 1)
				return 3;
			if (sprite_oam_floor[sprite] == 0)
				return 2;
		}
		return 1;
	}
	if (snes_priority == 2 ||
	    (snes_priority == 1 && !bg1_main && climbing_stairs() && link))
		return 2;
	return 3;
}

/* Fills shadow OAM and spr_gfx. Safe while the picture is on screen:
 * nothing here touches sprite VRAM. ZeldaHwCommitSprites copies both
 * during vblank, before the background upload can run the blank out. */
static void compose_sprites(Ppu *ppu, int sprites_on) {
	int sprite;
	int bg1_main = (ppu->screenEnabled[0] & 0x01) != 0;

	if (!sprites_on || ppu->forcedBlank) {
		oamClear(&oamMain, 0, 128);
		return;
	}

	for (sprite = 0; sprite < 128; sprite++) {
		int index = sprite * 2;
		int y = ppu->oam[index] >> 8;
		int high;
		int oam1;
		int x;
		int size;
		int name;
		int base;
		u16 *gfx;
		uint32 h;
		int tx, ty, n;

		if (y == 0xf0) {
			oamSet(&oamMain, sprite, 0, 0, 0, 0, SpriteSize_8x8,
				SpriteColorFormat_16Color, SPRITE_GFX, -1, false, true,
				false, false, false);
			continue;
		}

		high = ppu->oam[0x100 + (index >> 4)] >> (index & 15);
		oam1 = ppu->oam[index + 1];
		x = (ppu->oam[index] & 0xff) + ((high & 1) ? 256 : 0);
		if (x >= 256)
			x -= 512;
		y -= kCropY;
		if (x < 0)
			x += 512;
		if (y < 0)
			y += 256;
		size = (high & 2) ? 16 : 8;
		name = oam1 & 0xff;
		base = (oam1 & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
		gfx = spr_gfx + sprite * 64;

		h = 2166136261u;
		for (ty = 0; ty < size; ty += 8) {
			for (tx = 0; tx < size; tx += 8)
				h = hash_words(ppu->vram,
					(uint16)((base + obj_tile(name, tx / 8, ty / 8) * 16) & 0x7fff),
					16) ^ (h * 16777619u);
		}
		if (h == 0)
			h = 1;
		if (spr_hash[sprite] != h) {
			n = 0;
			for (ty = 0; ty < size; ty += 8) {
				for (tx = 0; tx < size; tx += 8, n++) {
					uint16 addr = (uint16)((base + obj_tile(name, tx / 8, ty / 8) * 16) & 0x7fff);
					uint16 tmp[16];
					int w;

					for (w = 0; w < 16; w++)
						tmp[w] = ppu->vram[(addr + w) & 0x7fff];
					conv4(tmp, gfx + n * 16, 0);
				}
			}
			spr_hash[sprite] = h;
			spr_dirty_slot[sprite] = 1;
			spr_dirty = 1;
		}

		oamSet(&oamMain, sprite, x, y,
			ds_sprite_priority((oam1 >> 12) & 3, bg1_main, sprite),
			(oam1 >> 9) & 7,
			size == 16 ? SpriteSize_16x16 : SpriteSize_8x8,
			SpriteColorFormat_16Color, SPRITE_GFX + sprite * 64, -1, false, false,
			(oam1 & 0x4000) != 0, (oam1 & 0x8000) != 0, false);
	}
}

void ZeldaHwComposeSprites(void) {
	if (!g_zenv.ppu)
		return;
	/* Spiral stairs from a lower floor take OBJ off the main screen and
	 * leave it on the subscreen. The OAM is the same either way. */
	compose_sprites(g_zenv.ppu,
		((g_zenv.ppu->screenEnabled[0] | g_zenv.ppu->screenEnabled[1]) & 0x10) != 0);
}

void ZeldaHwCommitSprites(int sprites_on) {
	if (!sprites_on) {
		oamClear(&oamMain, 0, 128);
		oamUpdate(&oamMain);
		return;
	}
	if (spr_dirty) {
		int s = 0;

		/* One copy per run of neighbouring slots. */
		while (s < 128) {
			int e;

			if (!spr_dirty_slot[s]) {
				s++;
				continue;
			}
			for (e = s; e < 128 && spr_dirty_slot[e]; e++)
				spr_dirty_slot[e] = 0;
			DC_FlushRange(spr_gfx + s * 64, (uint32)((e - s) * 64 * sizeof(u16)));
			dmaCopyWords(3, spr_gfx + s * 64, SPRITE_GFX + s * 64,
				(uint32)((e - s) * 64 * sizeof(u16)));
			s = e;
		}
		spr_dirty = 0;
	}
	oamUpdate(&oamMain);
}

/* First DS line of the letter grid. SNES row 127 is the first row drawn
 * with the post-IRQ scroll, and the top 16 SNES rows are cropped. */
enum { kBg3SplitLine = 128 - 1 - kCropY };

/* Inside the window: every layer, sprites, and the blender. Outside: nothing,
 * except the lamp, which leaves hardware BG0 (the status bar) visible.
 * The water window leaves the floor (BG3), doors (BG1), sprites, and the
 * status bar, and drops the subscreen (BG2) and the blend. */
enum { kWinInside = 0x3f, kWinHud = 0x01, kWinNoWater = 0x1b };

static int bg3_split_on;
static int scanline_handler;
static void m7_hblank(void);
static void scanline_irq_sync(void);

/* Hardware register block at 0x04000020. The HBlank interrupt stores one
 * before the next row. HBlank DMA cannot do this here: it also runs during
 * vblank, walks off this table, and the picture shakes. */
typedef struct M7Regs {
	s16 pa, pb, pc, pd;
	s32 x, y;
} M7Regs;

static M7Regs m7_regs[2][SCREEN_HEIGHT + 1];
static volatile int m7_pub;
static volatile int m7_on;
static int m7_pending;
static int m7_prepared;
static int m7_ready;
static int m7_bg;
static uint32 m7_sig;
static u8 *m7_map_ptr;
static u8 *m7_gfx_ptr;
static u16 bg3_top_x, bg3_top_y, bg3_bot_x, bg3_bot_y;

/* Two copies so the game can rebuild the table while a frame is still
 * scanning the previous one. Bit 0 of the publish word is "on". Bit 1 is
 * the lamp: clipped lines still show the status bar. Bit 2 is the water
 * window: clipped lines hide the subscreen and the blend. The following
 * byte is which copy the next frame should read. */
static u16 iris_span[2][SCREEN_HEIGHT];
static volatile uint32 iris_publish;
static int iris_latched;
static volatile int iris_on;
static u16 iris_outside;

static int iris_wanted(void) {
	return (HDMAEN_copy & 0x80) && W12SEL_copy == 0x33 && WOBJSEL_copy == 0x33;
}

/* Floodgate and swamp drain. Window 1 is enabled and inverted on BG1, and
 * TSW bit 0 windows that layer on the subscreen, so the water shows only
 * inside the span. Channel 7 is the same WH0 table the iris uses. */
static int water_wanted(void) {
	if (!(HDMAEN_copy & 0x80) || W12SEL_copy != 3 || !(TSW_copy & 1))
		return 0;
	if (!g_zenv.dma)
		return 0;
	return dma_read(g_zenv.dma, BBAD7) == (uint8)WH0;
}

/* One store. The low byte is the exclusive right edge (0 means 256) and
 * the high byte is the left edge. Separate byte writes tear a scanline. */
static void iris_set_x(unsigned left, unsigned right_exclusive) {
	*(vu16 *)0x04000040 = (u16)((left << 8) | (right_exclusive & 0xff));
}

/* WH0 is the left edge and WH1 the inclusive right. 0x00FF (left past
 * right) is the value zelda3 stores for a fully clipped line. X1==X2 makes
 * the DS ignore the window on that line and show every layer, which is the
 * frame of the room when the circle is shut. A one-pixel window with
 * neither side enabled stays black, and its right edge clears the inside
 * bit so the next line does not inherit the picture.
 * A span of the whole screen is X1=0,X2=0, which the DS also ignores, so
 * that line shows every layer outside a one-pixel window instead.
 * An exclusive right of 256 is stored as 0 and never clears the inside
 * bit; the last pixel stays outside so the following line starts clean. */
static void iris_apply_span(u16 span) {
	unsigned left = span & 0xff;
	unsigned right = span >> 8;
	unsigned edge;

	if (left > right) {
		WIN_IN = iris_outside;
		WIN_OUT = iris_outside;
		iris_set_x(0, 1);
		return;
	}
	WIN_IN = kWinInside;
	if (left == 0 && right == 255) {
		WIN_OUT = kWinInside;
		iris_set_x(0, 1);
		return;
	}
	edge = right + 1;
	if (edge > 255)
		edge = 255;
	WIN_OUT = iris_outside;
	iris_set_x(left, edge);
}

static void iris_apply_line(unsigned ds_line) {
	if (ds_line >= SCREEN_HEIGHT)
		return;
	iris_apply_span(iris_span[iris_latched][ds_line]);
}

/* Called on the last vblank lines, before line 0 is drawn. */
static void iris_latch_frame(void) {
	uint32 pub = iris_publish;
	int on = pub & 1;
	unsigned line = REG_VCOUNT;

	iris_latched = (int)((pub >> 8) & 1);
	iris_on = on;
	if (!on) {
		REG_DISPCNT &= ~DISPLAY_WIN0_ON;
		return;
	}
	if (pub & 2)
		iris_outside = kWinHud;
	else if (pub & 4)
		iris_outside = kWinNoWater;
	else
		iris_outside = 0;
	WIN0_Y0 = 0;
	WIN0_Y1 = (u8)SCREEN_HEIGHT;
	WIN_IN = kWinInside;
	BG_PALETTE[0] = 0;
	/* Past the picture, the next line drawn is line 0. Mid-frame, this
	 * line is on screen and has to take the span that matches the scroll
	 * just written, or the hole slides for the rest of the frame. */
	if (line >= (unsigned)SCREEN_HEIGHT)
		iris_apply_line(0);
	else
		iris_apply_line(line);
	REG_DISPCNT = (REG_DISPCNT & ~(DISPLAY_WIN1_ON | DISPLAY_SPR_WIN_ON)) |
		DISPLAY_WIN0_ON;
}

/* Mirror warp. Channels 6 and 7 write BG1HOFS and BG2HOFS from
 * hdma_table_dynamic, one absolute scroll per SNES line. The game shifts
 * that table down the screen while the picture fades through white.
 * Two copies so the table can be rebuilt while a frame is still scanning
 * the previous one. Bit 0 of the publish word is "on". The next byte is
 * which copy to read. The byte after that is which hardware backgrounds
 * show BG1 or BG2 (the status bar is BG3 and is not in the mask). */
static u16 wave_hofs[2][SCREEN_HEIGHT];
static volatile uint32 wave_publish;
static int wave_latched;
static volatile int wave_on;
static unsigned wave_slots;

static int wave_wanted(void) {
	if ((HDMAEN_copy & 0xc0) != 0xc0 || !g_zenv.dma)
		return 0;
	return dma_read(g_zenv.dma, BBAD6) == (uint8)BG1HOFS &&
		dma_read(g_zenv.dma, BBAD7) == (uint8)BG2HOFS;
}

/* HOFS is latched for the next line. VOFS is left alone. */
static void wave_write(unsigned slots, u16 x) {
	vu16 *reg = (vu16 *)0x04000010;

	if (slots & 2)
		reg[2] = x;
	if (slots & 4)
		reg[4] = x;
	if (slots & 8)
		reg[6] = x;
}

static void wave_apply_line(unsigned ds_line) {
	if (ds_line >= SCREEN_HEIGHT)
		return;
	wave_write(wave_slots, wave_hofs[wave_latched][ds_line]);
}

static void wave_latch_frame(void) {
	uint32 pub = wave_publish;
	unsigned line = REG_VCOUNT;

	wave_latched = (int)((pub >> 8) & 1);
	wave_slots = (pub >> 16) & 0xfu;
	wave_on = pub & 1;
	if (!wave_on)
		return;
	/* Past the picture, the next line drawn is line 0. Mid-frame, the
	 * store takes effect on the following line. */
	if (line >= (unsigned)SCREEN_HEIGHT)
		wave_apply_line(0);
	else
		wave_apply_line(line + 1);
}

/* show_bg3_lo: hardware BG3 is the name letters, not the ground. */
static void present_wave(int show_bg3_lo) {
	int on = wave_wanted();
	int idx = (int)(((wave_publish >> 8) & 1) ^ 1);
	unsigned slots = 0;
	int i;

	if (on) {
		/* Slot 2 is BG1. Slot 1 is BG1 or BG2. Slot 3 is BG2 unless the
		 * letters own it. Both HDMA channels carry the same scroll. */
		slots = (1u << 1) | (1u << 2);
		if (!show_bg3_lo)
			slots |= 1u << 3;
		for (i = 0; i < SCREEN_HEIGHT; i++)
			wave_hofs[idx][i] = (u16)(hdma_table_dynamic[i + kCropY] & 0x3ff);
	}
	wave_publish = ((uint32)idx << 8) | (on ? 1u : 0u) | (slots << 16);
	scanline_irq_sync();
	wave_latch_frame();
}

/* Fires at the end of REG_VCOUNT, so the change applies to the next line.
 * Lines 261 and 262 are the end of vblank; the next drawn line is line 0. */
static void scanline_hblank(void) {
	unsigned line = REG_VCOUNT;

	if (bg3_split_on) {
		if (line >= (unsigned)(kBg3SplitLine - 1) && line < SCREEN_HEIGHT - 1) {
			REG_BG3HOFS = bg3_bot_x;
			REG_BG3VOFS = bg3_bot_y;
		} else {
			REG_BG3HOFS = bg3_top_x;
			REG_BG3VOFS = bg3_top_y;
		}
	}
	if (line >= 261) {
		iris_latch_frame();
		wave_latch_frame();
		return;
	}
	if (iris_on)
		iris_apply_line(line + 1);
	if (wave_on)
		wave_apply_line(line + 1);
}

static void scanline_irq_sync(void) {
	/* 2: mode 7 owns the interrupt. The iris window must not come back on
	 * over the map. 1: letter grid, door iris, or mirror wave. */
	int want = m7_on ? 2 :
		(bg3_split_on || iris_on || (iris_publish & 1) ||
		 wave_on || (wave_publish & 1)) ? 1 : 0;

	if (scanline_handler == want)
		return;
	scanline_handler = want;
	if (!want) {
		irqDisable(IRQ_HBLANK);
		REG_DISPSTAT &= ~DISP_HBLANK_IRQ;
		return;
	}
	irqSet(IRQ_HBLANK, want == 2 ? m7_hblank : scanline_hblank);
	irqEnable(IRQ_HBLANK);
	REG_DISPSTAT |= DISP_HBLANK_IRQ;
}

static void bg3_split_set(int on) {
	bg3_split_on = on;
	scanline_irq_sync();
}

/* Cone in map space. Up/down share a band and left/right share the other,
 * so one row can hold two cones. A single span across that gap would light
 * most of the screen; each row keeps the runs apart and the scroll picks
 * the one on screen. Opaque means a subscreen pixel other than palette
 * index 0, which is what the SNES subtracts. Index 0 subtracts the fixed
 * color instead, and with no torches that fixed color is 31.
 * The floor filler is the same tile repeated across the map. The cones
 * are a few hundred cells, so a name used more often than this is the
 * floor and must stay black. */
enum { kLampSpans = 4, kLampCap = 16, kLampFill = 192 };

static u16 lamp_span_l[kLampSpans][512];
static u16 lamp_span_r[kLampSpans][512];
static uint8 lamp_span_n[512];
static uint16 lamp_ink[8];
static uint16 lamp_name_n[1024];
static u16 lamp_light[8][16];
static u16 lamp_screen[SCREEN_HEIGHT];
static uint32 lamp_map_sig;
static uint16 lamp_sig_map_adr;
static uint16 lamp_sig_tile_adr;
static uint8 lamp_sig_geom;
static int lamp_rows;
static int lamp_cols;
static int lamp_cache_ok;
static uint32 lamp_pix_epoch;
static uint32 lamp_pix_gen[1024];
static uint16 lamp_pix_attr[1024];
static uint16 lamp_pix_ink[1024];
static uint8 lamp_pix_rows[1024][8];

static int lamp_cap_ch(int c) {
	return c > kLampCap ? kLampCap : c;
}

static void lamp_closed(void) {
	int y;

	for (y = 0; y < SCREEN_HEIGHT; y++)
		lamp_screen[y] = 0x00ff;
}

static int lamp_tile_blank(const Ppu *ppu, int tile_adr) {
	int i;

	for (i = 0; i < 16; i++)
		if (ppu->vram[(tile_adr + i) & 0x7fff])
			return 0;
	return 1;
}

static uint16 lamp_map_word(const BgLayer *layer, int tx, int ty) {
	uint16 adr = (uint16)(layer->tilemapAdr + ((ty & 31) << 5 | (tx & 31)));

	if ((tx & 32) && layer->tilemapWider)
		adr = (uint16)(adr + 0x400);
	if ((ty & 32) && layer->tilemapHigher)
		adr = (uint16)(adr + (layer->tilemapWider ? 0x800 : 0x400));
	return adr;
}

/* Tiles are visited left to right. A run continues the open span, or
 * starts another when a gap separates this cone from the next one. */
static void lamp_mark_run(int y, int x0, int x1) {
	int n = lamp_span_n[y];

	if (n > 0 && x0 <= (int)lamp_span_r[n - 1][y] + 1) {
		if (x1 > (int)lamp_span_r[n - 1][y])
			lamp_span_r[n - 1][y] = (u16)x1;
		return;
	}
	if (n >= kLampSpans) {
		lamp_span_r[kLampSpans - 1][y] = (u16)x1;
		return;
	}
	lamp_span_l[n][y] = (u16)x0;
	lamp_span_r[n][y] = (u16)x1;
	lamp_span_n[y] = (uint8)(n + 1);
}

static void lamp_decode_rows(const Ppu *ppu, uint16 tile, uint8 rows[8]) {
	const BgLayer *layer = &ppu->bgLayer[0];
	int name = tile & 0x3ff;
	int base = (layer->tileAdr + name * 16) & 0x7fff;
	int vflip = (tile & 0x8000) != 0;
	int hflip = (tile & 0x4000) != 0;
	int row;
	uint16 ink = 0;

	if (lamp_pix_gen[name] == lamp_pix_epoch &&
	    lamp_pix_attr[name] == (uint16)(tile & 0xc000)) {
		memcpy(rows, lamp_pix_rows[name], 8);
		return;
	}
	for (row = 0; row < 8; row++) {
		int src = vflip ? 7 - row : row;
		uint16 plane0 = ppu->vram[(base + src) & 0x7fff];
		uint16 plane1 = ppu->vram[(base + 8 + src) & 0x7fff];
		uint8 out = 0;
		int col;

		if (plane0 | plane1) {
			for (col = 0; col < 8; col++) {
				int bit = hflip ? col : 7 - col;
				int pixel = ((plane0 >> bit) & 1) |
					(((plane0 >> (8 + bit)) & 1) << 1) |
					(((plane1 >> bit) & 1) << 2) |
					(((plane1 >> (8 + bit)) & 1) << 3);

				if (pixel) {
					ink = (uint16)(ink | (1u << pixel));
					out = (uint8)(out | (1u << col));
				}
			}
		}
		rows[row] = out;
	}
	lamp_pix_gen[name] = lamp_pix_epoch;
	lamp_pix_attr[name] = (uint16)(tile & 0xc000);
	lamp_pix_ink[name] = ink;
	memcpy(lamp_pix_rows[name], rows, 8);
}

static void lamp_stamp_rows(int tx, int ty, const uint8 rows[8]) {
	int x0 = tx << 3;
	int y0 = ty << 3;
	int row;

	for (row = 0; row < 8; row++) {
		uint8 bits = rows[row];
		int x;
		int run;

		if (bits == 0)
			continue;
		if (bits == 0xff) {
			lamp_mark_run(y0 + row, x0, x0 + 7);
			continue;
		}
		run = -1;
		for (x = 0; x < 8; x++) {
			if (bits & (1u << x)) {
				if (run < 0)
					run = x;
			} else if (run >= 0) {
				lamp_mark_run(y0 + row, x0 + run, x0 + x - 1);
				run = -1;
			}
		}
		if (run >= 0)
			lamp_mark_run(y0 + row, x0 + run, x0 + 7);
	}
}

static int lamp_build(const Ppu *ppu) {
	const BgLayer *layer = &ppu->bgLayer[0];
	int wider = layer->tilemapWider;
	int higher = layer->tilemapHigher;
	int tw = wider ? 64 : 32;
	int th = higher ? 64 : 32;
	int height = th << 3;
	int tile0_blank = lamp_tile_blank(ppu, layer->tileAdr);
	int saw = 0;
	int ty;
	int tx;

	lamp_rows = height;
	lamp_cols = tw << 3;
	if (++lamp_pix_epoch == 0) {
		memset(lamp_pix_gen, 0, sizeof lamp_pix_gen);
		lamp_pix_epoch = 1;
	}
	memset(lamp_ink, 0, sizeof lamp_ink);
	memset(lamp_name_n, 0, sizeof lamp_name_n);
	for (ty = 0; ty < height; ty++)
		lamp_span_n[ty] = 0;
	for (ty = 0; ty < th; ty++) {
		for (tx = 0; tx < tw; tx++) {
			uint16 tile = ppu->vram[lamp_map_word(layer, tx, ty) & 0x7fff];
			int name = tile & 0x3ff;

			if (tile0_blank && name == 0)
				continue;
			if (lamp_name_n[name] != 0xffff)
				lamp_name_n[name]++;
		}
	}
	for (ty = 0; ty < th; ty++) {
		for (tx = 0; tx < tw; tx++) {
			uint16 tile = ppu->vram[lamp_map_word(layer, tx, ty) & 0x7fff];
			int pal = (tile >> 10) & 7;
			int name = tile & 0x3ff;
			uint8 rows[8];

			if (tile0_blank && name == 0)
				continue;
			if (lamp_name_n[name] > kLampFill)
				continue;
			lamp_decode_rows(ppu, tile, rows);
			if (lamp_pix_ink[name] == 0)
				continue;
			lamp_ink[pal] = (uint16)(lamp_ink[pal] | lamp_pix_ink[name]);
			lamp_stamp_rows(tx, ty, rows);
			saw = 1;
		}
	}
	if (!saw)
		return 0;
	for (ty = 0; ty < height; ty++)
		if (lamp_span_n[ty])
			return 1;
	return 0;
}

static u16 lamp_screen_span(int left, int right, int scroll, int width) {
	int mask = width - 1;
	int rel_l;
	int rel_r;

	if (left == 0xffff)
		return 0x00ff;
	rel_l = (left - scroll) & mask;
	rel_r = (right - scroll) & mask;
	if (rel_l <= rel_r) {
		if (rel_l >= 256)
			return 0x00ff;
		if (rel_r > 255)
			rel_r = 255;
		return (u16)(rel_l | (rel_r << 8));
	}
	/* The screen's left edge sits inside the hole, so the visible piece
	 * starts at x = 0. A second piece on the right only happens when the
	 * map wraps inside this screen; keep the longer of the two. */
	if (rel_r >= 256)
		return (u16)(0 | (255 << 8));
	if (rel_l >= 256)
		return (u16)(rel_r << 8);
	if (rel_r + 1 >= 256 - rel_l)
		return (u16)(rel_r << 8);
	return (u16)(rel_l | (255 << 8));
}

static int lamp_visible_len(u16 span) {
	unsigned left = span & 0xff;
	unsigned right = span >> 8;

	if (left > right)
		return 0;
	return (int)(right - left + 1);
}

/* One cone is in the scroll. The other sits a screen away and must not
 * widen this line. */
static u16 lamp_row_on_screen(int map_y, int scroll, int width) {
	int n = lamp_span_n[map_y];
	u16 best = 0x00ff;
	int best_len = 0;
	int i;

	for (i = 0; i < n; i++) {
		u16 piece = lamp_screen_span(lamp_span_l[i][map_y],
			lamp_span_r[i][map_y], scroll, width);
		int len = lamp_visible_len(piece);

		if (len > best_len) {
			best = piece;
			best_len = len;
		}
	}
	return best;
}

static void lamp_project(const BgLayer *layer) {
	int height = lamp_rows;
	int width = lamp_cols;
	int scroll_x = layer->hScroll;
	int scroll_y = layer->vScroll;
	int y;

	for (y = 0; y < SCREEN_HEIGHT; y++) {
		int map_y = ((y + kCropY) + scroll_y) & (height - 1);

		lamp_screen[y] = lamp_row_on_screen(map_y, scroll_x, width);
	}
}

/* Channels above the cap are the anti-aliased rim. Counting them raw
 * would make the brightest subtract 31 and the add-back would no longer
 * depend on the room color. */
static void lamp_colors(const Ppu *ppu) {
	int pal;
	int i;
	int mr = 0;
	int mg = 0;
	int mb = 0;

	for (pal = 0; pal < 8; pal++) {
		uint16 ink = lamp_ink[pal];

		for (i = 1; i < 16; i++) {
			uint16 color;
			int r, g, b;

			if ((ink & (1u << i)) == 0)
				continue;
			color = ppu->cgram[pal * 16 + i];
			r = lamp_cap_ch(color & 31);
			g = lamp_cap_ch((color >> 5) & 31);
			b = lamp_cap_ch((color >> 10) & 31);
			if (r > mr)
				mr = r;
			if (g > mg)
				mg = g;
			if (b > mb)
				mb = b;
		}
	}
	lamp_cmax_r = (uint8)mr;
	lamp_cmax_g = (uint8)mg;
	lamp_cmax_b = (uint8)mb;
	for (pal = 0; pal < 8; pal++) {
		uint16 ink = lamp_ink[pal];

		for (i = 0; i < 16; i++) {
			uint16 color;
			int r, g, b;

			if (i == 0 || (ink & (1u << i)) == 0) {
				lamp_light[pal][i] = 0;
				continue;
			}
			color = ppu->cgram[pal * 16 + i];
			r = lamp_cap_ch(color & 31);
			g = lamp_cap_ch((color >> 5) & 31);
			b = lamp_cap_ch((color >> 10) & 31);
			lamp_light[pal][i] = (u16)(sub_sat(mr, r) |
				(sub_sat(mg, g) << 5) |
				(sub_sat(mb, b) << 10));
		}
	}
	/* A black cone pixel subtracts nothing, so its light color is the
	 * whole Cmax and the room shows through. An opaque zero add would
	 * hide the room, which is only right when every cone pixel is black. */
	lamp_show_light = (mr | mg | mb) != 0;
}

static void lamp_write_colors(void) {
	int pal;
	int i;

	if (!lamp_show_light)
		return;
	for (pal = 0; pal < 8; pal++) {
		for (i = 0; i < 16; i++)
			BG_PALETTE[(8 + pal) * 16 + i] = lamp_light[pal][i];
	}
}

static void lamp_prepare(const Ppu *ppu) {
	const BgLayer *layer = &ppu->bgLayer[0];
	int words;
	uint32 sig;
	uint8 geom;

	if (!lamp_cone(ppu)) {
		lamp_dim = 0;
		lamp_show_light = 0;
		lamp_closed();
		return;
	}
	words = map_words(layer->tilemapWider, layer->tilemapHigher);
	sig = hash_words(ppu->vram, layer->tilemapAdr, words);
	geom = (uint8)((layer->tilemapWider ? 1 : 0) |
		(layer->tilemapHigher ? 2 : 0));
	if (!lamp_cache_ok || sig != lamp_map_sig || geom != lamp_sig_geom ||
	    layer->tilemapAdr != lamp_sig_map_adr ||
	    layer->tileAdr != lamp_sig_tile_adr) {
		lamp_cache_ok = lamp_build(ppu);
		lamp_map_sig = sig;
		lamp_sig_geom = geom;
		lamp_sig_map_adr = layer->tilemapAdr;
		lamp_sig_tile_adr = layer->tileAdr;
	}
	if (!lamp_cache_ok) {
		lamp_dim = 0;
		lamp_show_light = 0;
		lamp_closed();
		return;
	}
	lamp_colors(ppu);
	lamp_dim = 1;
	lamp_project(layer);
}
/* Copy the finished iris, or build the lamp hole, and arm the scanline
 * IRQ. Latch immediately: the room upload runs past line 0, and the shut
 * circle has to already be clipping when that line is drawn. */
static void present_iris(const Ppu *ppu) {
	int on = iris_wanted();
	int water = !on && water_wanted();
	int lamp = !on && !water && lamp_cone(ppu);
	int idx = (int)(((iris_publish >> 8) & 1) ^ 1);
	int i;

	if (on || water) {
		for (i = 0; i < SCREEN_HEIGHT; i++)
			iris_span[idx][i] = hdma_table_dynamic[i + kCropY];
		if (on)
			BG_PALETTE[0] = 0;
		lamp_show_light = 0;
		lamp_dim = 0;
	} else if (lamp) {
		lamp_prepare(ppu);
		memcpy(iris_span[idx], lamp_screen, sizeof lamp_screen);
		BG_PALETTE[0] = 0;
	} else {
		lamp_show_light = 0;
		lamp_dim = 0;
	}
	iris_publish = ((uint32)idx << 8) | ((on || lamp || water) ? 1u : 0u) |
		(lamp ? 2u : 0u) | (water ? 4u : 0u);
	scanline_irq_sync();
	/* Spotlight_open leaves a closed circle, and the room upload can still
	 * be going when line 0 is drawn. Latch whatever line is current. The
	 * IRQ keeps each later line on its own span, so this does not smear
	 * the circle into one bar. */
	iris_latch_frame();
}

/* True when the door iris has no open pixel. That frame is black on the
 * SNES because every layer is clipped; brightness stays up. */
static int iris_fully_closed(void) {
	int idx;
	int i;

	if (!(iris_publish & 1) || (iris_publish & 6))
		return 0;
	idx = (int)((iris_publish >> 8) & 1);
	for (i = 0; i < SCREEN_HEIGHT; i++) {
		u16 span = iris_span[idx][i];

		if ((span & 0xff) <= (span >> 8))
			return 0;
	}
	return 1;
}

/* Tile 960, 64 tiles of 4bpp. Behind the 16KB the game sprites upload.
 * Index 960 is a multiple of the 64-tile 64x64 sprite, and 960+64-1 still
 * fits in the 10-bit name. */
static u16 *iris_fill_gfx(void) {
	return SPRITE_GFX + (960 * 32) / 2;
}

static void iris_fill_init(void) {
	dmaFillWords(0x11111111, iris_fill_gfx(), 64 * 32);
}

/* The backdrop cannot be windowed, so the iris keeps it black and the
 * outside of the circle is black. Tree edges are transparent pixels of
 * that backdrop (the green or brown fringe). A solid sprite of the real
 * color sits behind the ground and only inside the window, which is where
 * those edges look through. The ground has to be on the main screen: its
 * priority is then 2, and this sprite is priority 3, so it stays behind
 * the tiles. Palette 15 is past the eight SNES sprite palettes. Hidden
 * OAM entries are reused so Link stays put. */
static void iris_lay_backdrop(const Ppu *ppu) {
	int back;
	u16 color;
	int slot;
	int n;
	int used[12];

	if (!(iris_publish & 1) || (iris_publish & 6) || iris_fully_closed())
		return;
	back = math_active(ppu) && (ppu->mathEnabled & 0x20);
	color = back ? math_color(ppu->cgram[0], ppu) : ppu->cgram[0];
	color = (u16)(color & 0x7fff);
	if (!color)
		return;
	SPRITE_PALETTE[15 * 16 + 1] = color;

	n = 0;
	for (slot = 127; slot >= 0 && n < 12; slot--) {
		SpriteEntry *e = &oamMain.oamMemory[slot];

		if (e->isRotateScale || (!e->isHidden && e->y < SCREEN_HEIGHT))
			continue;
		used[n++] = slot;
	}
	for (slot = 127; slot >= 0 && n < 12; slot--) {
		int k;
		int take = 1;

		for (k = 0; k < n; k++) {
			if (used[k] == slot)
				take = 0;
		}
		if (take)
			used[n++] = slot;
	}
	for (n = 0; n < 12; n++) {
		oamSet(&oamMain, used[n], (n % 4) * 64, (n / 4) * 64,
			3, 15, SpriteSize_64x64, SpriteColorFormat_16Color,
			iris_fill_gfx(), -1, false, false, false, false, false);
	}
	oamUpdate(&oamMain);
}

/* Overworld: BG1 is not on the main screen, so this layer can show the
 * priority-1 ground tiles with BG2's own graphics. */
static void show_bg2_front(const Ppu *ppu) {
	const BgLayer *layer = &ppu->bgLayer[1];
	int words = map_words(layer->tilemapWider, layer->tilemapHigher);

	bg2_front_on_slot1 = 1;
	if (!map_job[1].on && slot1_up.src == bg2_hi && slot1_up.ver == bg2_hi_ver &&
	    slot1_up.words == words && slot1_up.wider == layer->tilemapWider &&
	    slot1_up.higher == layer->tilemapHigher)
		return;
	queue_map(1, bg2_hi, words, layer->tilemapWider, layer->tilemapHigher, 4, 4);
}

static void upload4_reloc(const Ppu *ppu, u16 *gfx, uint16 tile_adr, int src, int dst) {
	uint16 addr = (uint16)((tile_adr + src * 16) & 0x7fff);
	uint32 h = hash_words(ppu->vram, addr, 16) ^ 0x9e3779b9u;
	uint16 tmp[16];
	int i;

	if (!h)
		h = 1;
	if (tile_hash[0][dst] == h)
		return;
	for (i = 0; i < 16; i++)
		tmp[i] = ppu->vram[(addr + i) & 0x7fff];
	conv4(tmp, gfx + dst * 16, 0);
	tile_hash[0][dst] = h;
}

static u16 swizzle_as(uint16 entry, int tile) {
	return swizzle4((uint16)((entry & ~0x3ff) | (tile & 0x3ff)));
}

/* Dungeon BG1 already uses both spare layers. Copy doorway tiles into free
 * slots of that tileset and put them on the priority-1 map, which is the
 * layer zelda3 draws above Link and below sprite priority 3. */
static void bake_bg2_doors(Ppu *ppu, int stair) {
	static int last_stair = -1;
	BgLayer *top = &ppu->bgLayer[0];
	BgLayer *ground = &ppu->bgLayer[1];
	int words = map_words(top->tilemapWider, top->tilemapHigher);
	static int remap[1024];
	static int free_tile[1024];
	int free_n = 0;
	int free_i = 0;
	int patched = 0;
	u16 *gfx;
	u16 blank;
	int i;

	if (top->hScroll != ground->hScroll || top->vScroll != ground->vScroll ||
	    top->tilemapWider != ground->tilemapWider ||
	    top->tilemapHigher != ground->tilemapHigher)
		return;
	if (last_stair != stair) {
		last_stair = stair;
		bake_valid = 0;
	}
	/* Door tiles are copied into the BG1 tileset. Slot 1 may still be
	 * showing the BG2 doorway map until the queued map is committed. */
	gfx = BG_GFX + 2 * 8192;
	if (bake_valid) {
		/* Graphics can still animate. The map entries already point at them. */
		for (i = 0; i < 1024; i++) {
			if (remap[i] >= 0)
				upload4_reloc(ppu, gfx, ground->tileAdr, i, remap[i]);
		}
		return;
	}
	blank = (u16)(blank_bg1 < 0 ? 0 : blank_bg1);
	for (i = 0; i < 1024; i++)
		remap[i] = -1;
	for (i = 1; i < 1024; i++) {
		if (!used_bg1[i] && i != blank_bg1)
			free_tile[free_n++] = i;
	}
	if (stair)
		mark_climb(ppu);
	for (i = 0; i < words; i++) {
		uint16 above = ppu->vram[(top->tilemapAdr + i) & 0x7fff];
		uint16 door;
		int src, dst;
		int name = above & 0x3ff;
		int x, y, cover = 0;

		if (stair && (doorway_wall(name) || name == 0x0ac)) {
			int cell;

			map_xy(i, top->tilemapWider, top->tilemapHigher, &x, &y);
			cell = stair_wall_cell(x, y);
			if (cell && doorway_wall(name))
				cover = 1;
			else if (cell == 2 && name == 0x0ac)
				cover = 1;
		}
		if (above & 0x2000) {
			/* The subscreen's own copy of the floor. It belongs behind
			 * the ground. Promoting it covers the door and the sprites
			 * standing on that floor. A wall on this cell still has to
			 * be drawn; the floor only stays behind it. */
			if (stair && keep_under_link(name) && !cover) {
				map_buf[2][i] = swizzle4(above);
			} else {
				map_buf[1][i] = swizzle4(above);
				map_buf[2][i] = blank;
				continue;
			}
		} else if (cover) {
			/* The wall over the opening is on this layer and has no
			 * priority bit. The cap below it does, and this row has
			 * to come with it. */
			map_buf[1][i] = swizzle4(above);
			map_buf[2][i] = blank;
			continue;
		}
		door = ppu->vram[(ground->tilemapAdr + i) & 0x7fff];
		name = door & 0x3ff;
		cover = 0;
		if (stair && (doorway_wall(name) || name == 0x0ac)) {
			int cell;

			map_xy(i, ground->tilemapWider, ground->tilemapHigher, &x, &y);
			cell = stair_wall_cell(x, y);
			if (cell && doorway_wall(name))
				cover = 1;
			else if (cell == 2 && name == 0x0ac)
				cover = 1;
		}
		if ((!(door & 0x2000) && !cover) ||
		    (stair && keep_under_link(name) && !cover)) {
			map_buf[1][i] = blank;
			continue;
		}
		src = door & 0x3ff;
		dst = remap[src];
		if (dst < 0) {
			if (free_i >= free_n) {
				map_buf[1][i] = blank;
				map_buf[3][i] = swizzle4(door);
				patched = 1;
				continue;
			}
			dst = free_tile[free_i++];
			remap[src] = dst;
			upload4_reloc(ppu, gfx, ground->tileAdr, src, dst);
		}
		map_buf[1][i] = swizzle_as(door, dst);
	}
	queue_map(1, map_buf[1], words, top->tilemapWider, top->tilemapHigher, 4, 2);
	/* The floor cells above went into the back map after it was queued. */
	queue_map(2, map_buf[2], words, top->tilemapWider, top->tilemapHigher, 8, 2);
	if (patched)
		queue_map(3, map_buf[3], words, top->tilemapWider, top->tilemapHigher, 12, 4);
	bake_valid = 1;
}

static void present_scroll(const Ppu *ppu, int back_bg3) {
	int sy0 = (int)ppu->bgLayer[0].vScroll + kCropY;
	int sy1 = (int)ppu->bgLayer[1].vScroll + kCropY;
	int sy2 = (int)ppu->bgLayer[2].vScroll + kCropY;
	int split = back_bg3 && irq_flag != 0;
	int ime = enterCriticalSection();

	hw_scroll_x[2] = (int)ppu->bgLayer[0].hScroll;
	hw_scroll_y[2] = sy0;
	if (bg2_front_on_slot1) {
		hw_scroll_x[1] = (int)ppu->bgLayer[1].hScroll;
		hw_scroll_y[1] = sy1;
	} else {
		hw_scroll_x[1] = (int)ppu->bgLayer[0].hScroll;
		hw_scroll_y[1] = sy0;
	}
	hw_scroll_x[0] = (int)ppu->bgLayer[2].hScroll;
	hw_scroll_y[0] = sy2 - kHudDrop;
	if (back_bg3) {
		hw_scroll_x[3] = (int)ppu->bgLayer[2].hScroll;
		hw_scroll_y[3] = sy2;
	} else {
		hw_scroll_x[3] = (int)ppu->bgLayer[1].hScroll;
		hw_scroll_y[3] = sy1;
	}
	hw_scroll_valid = 1;

	if (split) {
		/* Above the IRQ the name stays on the normal BG3 scroll. Below it
		 * the grid follows selectfile_var8 and BG3 V is reset to 0. */
		bg3_top_x = (u16)ppu->bgLayer[2].hScroll;
		bg3_top_y = (u16)sy2;
		bg3_bot_x = (u16)(selectfile_var8 & 0x1ff);
		bg3_bot_y = (u16)kCropY;
	}
	bg3_split_set(split);
	if (irq_flag & 0x80)
		irq_flag = 0;
	hw_scroll_apply();
	leaveCriticalSection(ime);
}

/* The lamp hole is screen space. Latch it with the scroll just written.
 * Waiting until the next vblank leaves the old hole on the new room, which
 * slides as Link walks. The door iris uses its own table. */
static void latch_scroll_window(void) {
	int ime;

	if (iris_wanted() || !(iris_publish & 1))
		return;
	ime = enterCriticalSection();
	iris_latch_frame();
	leaveCriticalSection(ime);
}

/* (A * v) with the SNES mode 7 multiply: the low 6 bits of the 8.8
 * product are dropped. */
static int32 m7_mul(int m, int v) {
	return ((int32)m * (int32)v) & ~(int32)63;
}

static void m7_hw_init(void) {
	/* DS mode 2 is the one where BG2 is affine. Mode 1 keeps BG2 as a
	 * text layer and puts the affine background on BG3. */
	videoSetMode(MODE_2_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	/* 128x128 bytes of map, then 256 tiles of 8bpp. Same layout the SNES
	 * keeps in the low and high bytes of mode 7 VRAM. */
	m7_bg = bgInit(2, BgType_Rotation, BgSize_R_1024x1024, 0, 1);
	bgSetPriority(m7_bg, 1);
	bgMosaicDisable(m7_bg);
	/* Overflow is transparent. A window that tried to clip the same edge
	 * dropped a scanline whenever that edge moved. */
	BGCTRL[m7_bg] &= ~BG_WRAP_ON;
	m7_map_ptr = (u8 *)bgGetMapPtr(m7_bg);
	m7_gfx_ptr = (u8 *)bgGetGfxPtr(m7_bg);
	m7_ready = 1;
}

static u8 m7_map_tmp[0x4000] __attribute__((aligned(4)));
static u8 m7_gfx_tmp[0x4000] __attribute__((aligned(4)));
static int m7_gfx_dirty;

/* The hash and the matrix are built while the previous picture is still
 * up. Vblank only copies bytes and stores line 0. */
static void m7_prepare_gfx(const Ppu *ppu) {
	const uint16 *src = ppu->vram;
	uint32 h = 2166136261u;
	int i;

	for (i = 0; i < 0x4000; i++)
		h = (h ^ src[i]) * 16777619u;
	if (h == 0)
		h = 1;
	if (h == m7_sig)
		return;
	m7_sig = h;
	for (i = 0; i < 0x4000; i++) {
		m7_map_tmp[i] = (u8)src[i];
		m7_gfx_tmp[i] = (u8)(src[i] >> 8);
	}
	m7_gfx_dirty = 1;
}

static void m7_flush_gfx(void) {
	if (!m7_gfx_dirty || !m7_map_ptr)
		return;
	DC_FlushRange(m7_map_tmp, sizeof m7_map_tmp);
	DC_FlushRange(m7_gfx_tmp, sizeof m7_gfx_tmp);
	dmaCopyWords(3, m7_map_tmp, m7_map_ptr, sizeof m7_map_tmp);
	dmaCopyWords(3, m7_gfx_tmp, m7_gfx_ptr, sizeof m7_gfx_tmp);
	m7_gfx_dirty = 0;
}

static void m7_palettes(const Ppu *ppu, int cover) {
	int on = math_active(ppu);
	int bg = on && (ppu->mathEnabled & 0x01);
	int back = on && (ppu->mathEnabled & 0x20);
	int obj = on && (ppu->mathEnabled & 0x10);
	int i;
	u16 color;

	color = back ? math_color(ppu->cgram[0], ppu) : ppu->cgram[0];
	BG_PALETTE[0] = cover ? 0 : (u16)(color & 0x7fff);
	for (i = 1; i < 256; i++) {
		color = bg ? math_color(ppu->cgram[i], ppu) : ppu->cgram[i];
		BG_PALETTE[i] = (u16)(color & 0x7fff);
	}
	for (i = 0; i < 128; i++) {
		color = obj ? math_color(ppu->cgram[128 + i], ppu) : ppu->cgram[128 + i];
		SPRITE_PALETTE[i] = (u16)(color & 0x7fff);
	}
}

static void m7_build(const Ppu *ppu, int buf) {
	static ZeldaM7Line cap[224];
	int x_center = ((int16)(ppu->m7matrix[4] << 3)) >> 3;
	int y_center = ((int16)(ppu->m7matrix[5] << 3)) >> 3;
	int h_scroll = ((int16)(ppu->m7matrix[6] << 3)) >> 3;
	int v_scroll = ((int16)(ppu->m7matrix[7] << 3)) >> 3;
	int clipped_h = h_scroll - x_center;
	int clipped_v = v_scroll - y_center;
	int ds;

	ZeldaCaptureMode7(cap, 224);
	clipped_h = (clipped_h & 0x2000) ? (clipped_h | ~1023) : (clipped_h & 1023);
	clipped_v = (clipped_v & 0x2000) ? (clipped_v | ~1023) : (clipped_v & 1023);

	for (ds = 0; ds < SCREEN_HEIGHT; ds++) {
		int row = ds + kCropY;
		int y = row + 1;
		int a = cap[row].a;
		int b = cap[row].b;
		int c = cap[row].c;
		int d = cap[row].d;
		int ry;
		int32 start_x, start_y, ox, oy;
		s16 step_x, step_y;
		M7Regs *s = &m7_regs[buf][ds];

		if ((ppu->mosaicEnabled & 1) && ppu->mosaicSize > 1)
			y -= (y - 1) % ppu->mosaicSize;
		ry = ppu->m7yFlip ? 255 - y : y;
		start_x = m7_mul(a, clipped_h) + m7_mul(b, ry) +
			m7_mul(b, clipped_v) + ((int32)x_center << 8);
		start_y = m7_mul(c, clipped_h) + m7_mul(d, ry) +
			m7_mul(d, clipped_v) + ((int32)y_center << 8);
		step_x = (s16)(ppu->m7xFlip ? -a : a);
		step_y = (s16)(ppu->m7xFlip ? -c : c);
		ox = start_x;
		oy = start_y;
		if (ppu->m7xFlip) {
			ox += (int32)a * 255;
			oy += (int32)c * 255;
		}
		s->pa = step_x;
		s->pb = 0;
		s->pc = step_y;
		s->pd = 0;
		s->x = ox;
		s->y = oy;
	}
	m7_regs[buf][SCREEN_HEIGHT] = m7_regs[buf][SCREEN_HEIGHT - 1];
}

/* Four stores, in ITCM, so this finishes inside HBlank. A late store
 * resets the texture X mid-row, which is the torn horizontal line.
 * Line 0 is stored from vblank. Rewriting it from the last blanking
 * lines races the start of the picture and tears the top row. */
static ITCM_CODE void m7_hblank(void) {
	unsigned line = REG_VCOUNT;
	const M7Regs *s;
	vu32 *dst;
	const u32 *src;

	if (!(REG_DISPSTAT & DISP_IN_HBLANK))
		return;
	if (line >= SCREEN_HEIGHT - 1)
		return;
	s = &m7_regs[m7_pub][line + 1];
	dst = (vu32 *)0x04000020;
	src = (const u32 *)s;
	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
	dst[3] = src[3];
}

void ZeldaHwPrepareMode7(const Ppu *ppu) {
	int buf = m7_pub ^ 1;

	m7_prepare_gfx(ppu);
	m7_build(ppu, buf);
	m7_pending = buf;
	m7_prepared = 1;
}

static void m7_regs_write(const M7Regs *s) {
	vu32 *dst = (vu32 *)0x04000020;
	const u32 *src = (const u32 *)s;

	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
	dst[3] = src[3];
}

static void m7_dma_stop(void) {
	DMA0_CR = 0;
}

void ZeldaHwPresentMode7(Ppu *ppu) {
	int buf;
	int cover = ppu->forcedBlank;
	int sprites = !cover &&
		((ppu->screenEnabled[0] | ppu->screenEnabled[1]) & 0x10) != 0;

	/* Channel 0 was the HBlank DMA. It has to be off before line 0. */
	m7_dma_stop();
	if (!m7_ready)
		m7_hw_init();
	if (!m7_prepared)
		ZeldaHwPrepareMode7(ppu);
	buf = m7_pending;
	m7_prepared = 0;
	m7_pub = buf;

	REG_DISPCNT = MODE_2_2D | DISPLAY_BG2_ACTIVE | DISPLAY_SPR_ACTIVE |
		DISPLAY_SPR_1D | DISPLAY_SPR_1D_SIZE_32;
	REG_BLDCNT = 0;
	bgSetPriority(m7_bg, 1);
	BGCTRL[m7_bg] &= ~BG_WRAP_ON;
	if (cover)
		bgHide(m7_bg);
	else
		bgShow(m7_bg);

	m7_on = cover ? 0 : 1;
	scanline_irq_sync();
	/* Start of vblank, before any copy. Line 0 is not touched again. */
	if (!cover && REG_VCOUNT >= (unsigned)SCREEN_HEIGHT)
		m7_regs_write(&m7_regs[buf][0]);

	m7_flush_gfx();
	m7_palettes(ppu, cover);
	ZeldaHwCommitSprites(sprites);

	picture_covered = cover;
	last_inidisp = cover ? 0x80 : INIDISP_copy;
	ZeldaHwUseMasterBright(1);
	if (cover && fade_level < 0)
		REG_MASTER_BRIGHT = inidisp_to_bright(0x80);
}

void ZeldaHwConsoleSuspend(void)
{
	/* scanline_handler stays set while the irq is forced off, so the next
	 * sync has to run again or the letter grid never gets its split back. */
	scanline_handler = -1;
	irqDisable(IRQ_HBLANK);
	REG_DISPSTAT &= ~DISP_HBLANK_IRQ;
	REG_BLDCNT = 0;
	REG_MOSAIC = 0;
	REG_DISPCNT &= ~(DISPLAY_WIN0_ON | DISPLAY_WIN1_ON | DISPLAY_SPR_WIN_ON);
	ZeldaHwUseMasterBright(0);
}

static void hw_bind(int clear_gfx) {
	m7_dma_stop();
	m7_on = 0;
	m7_ready = 0;
	m7_prepared = 0;
	m7_sig = 0;
	m7_gfx_dirty = 0;
	scanline_irq_sync();
	/* Sleep clears the LCD bit. The 2D engines come back with it, but a
	 * full graphics fill here stalls on that bus and the frame never
	 * returns, so the song stops on a black screen. */
	pmPowerOn(POWCNT_2D_GFX_A | POWCNT_2D_GFX_B);
	videoSetMode(MODE_0_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankE(VRAM_E_MAIN_SPRITE);
	irqSet(IRQ_VBLANK, vblank_bright);
	irqEnable(IRQ_VBLANK);
	oamInit(&oamMain, SpriteMapping_1D_32, false);
	lcdMainOnTop();

	bg_size[0] = bg_size[1] = bg_size[2] = bg_size[3] = 0xff;
	bg_id[0] = bgInit(0, BgType_Text4bpp, BgSize_T_256x256, 0, 6);
	bg_id[1] = bgInit(1, BgType_Text4bpp, BgSize_T_256x256, 4, 2);
	bg_id[2] = bgInit(2, BgType_Text4bpp, BgSize_T_256x256, 8, 2);
	bg_id[3] = bgInit(3, BgType_Text4bpp, BgSize_T_256x512, 12, 4);
	bg_size[0] = 0;
	bg_size[1] = 0;
	bg_size[2] = 0;
	bg_size[3] = 2;
	bg_tilebase[0] = 6;
	bg_tilebase[1] = 2;
	bg_tilebase[2] = 2;
	bg_tilebase[3] = 4;
	back_mode = -1;
	hw_scroll_valid = 0;
	memset(map_job, 0, sizeof map_job);
	memset(&slot1_up, 0, sizeof slot1_up);
	bgSetPriority(bg_id[0], 0);
	bgSetPriority(bg_id[1], 1);
	bgSetPriority(bg_id[2], 2);
	bgSetPriority(bg_id[3], 3);
	pic.ready = 0;
	bg3_split_set(0);

	memset(tile_hash, 0, sizeof tile_hash);
	memset(map_valid, 0, sizeof map_valid);
	memset(bg3_slot, 0, sizeof bg3_slot);
	memset(spr_hash, 0, sizeof spr_hash);
	bg3_next = 1;
	last_adr[0] = last_adr[1] = last_adr[2] = 0xffff;
	blank_bg1 = kBlankNone;
	if (clear_gfx) {
		dmaFillWords(0, BG_GFX, 128 * 1024);
		dmaFillWords(0, SPRITE_GFX, 16 * 1024);
	}
	iris_fill_init();
	oamClear(&oamMain, 0, 128);
	oamUpdate(&oamMain);
}

void ZeldaHwInit(void) {
	hw_bind(1);
}

void ZeldaHwWake(void) {
	hw_bind(0);
}

static void classify_picture(const Ppu *ppu) {
	int main_en;
	int sub_en;
	int bg1_main;
	int bg1_sub;
	int layer_math;
	int lamp;
	int overlay;
	int back_add;
	int bridge;
	int bg2_main;
	int bg2_on;
	int scrolls_match;
	int lift_bg2;
	int stair_arch;
	int bg3_on;

	pic.cover = ppu->forcedBlank || file_select_not_ready(ppu);
	main_en = pic.cover ? 0 : ppu->screenEnabled[0];
	sub_en = pic.cover ? 0 : ppu->screenEnabled[1];
	pic.main_en = main_en;
	pic.sub_en = sub_en;
	bg1_main = (main_en & 0x01) != 0;
	bg1_sub = (sub_en & 0x01) != 0;
	pic.bg1_main = bg1_main;
	pic.bg1_sub = bg1_sub;
	/* Rain and the other overworld covers: BG1 is subscreen-only, BG2 is
	 * the opaque ground, and color math targets that ground (or another
	 * main layer). Bit 5 is the backdrop in this PPU, which is all Hyrule
	 * Castle writes (CGADSUB 0x20). That must not become the rain blend.
	 * The title picture has no ground layer, so it stays behind the sprites. */
	layer_math = (ppu->mathEnabled & 0x1f) != 0;
	lamp = lamp_cone(ppu);
	pic.lamp = lamp;
	/* The lamp cone is subscreen BG1 with subtract math, which is the same
	 * test as rain. It is not rain: the cone is light added onto the room,
	 * clipped by the window. */
	overlay = !lamp && bg1_sub && !bg1_main &&
		(main_en & 0x02) &&
		ppu->addSubscreen && ppu->preventMathMode != 3 &&
		layer_math;
	/* Rooms whose header really adds the subscreen into BG2 (Translucent,
	 * Addition). Swamp Palace is one. The subscreen is mixed into the floor
	 * only where it is opaque, behind Link. Rain stays in front of him. */
	back_add = overlay && player_is_indoors && (ppu->mathEnabled & 0x02) &&
		!ppu->subtractColor &&
		ppu->bgLayer[0].hScroll == ppu->bgLayer[1].hScroll &&
		ppu->bgLayer[0].vScroll == ppu->bgLayer[1].vScroll;
	if (back_add)
		overlay = 0;
	/* The bottle bridge (overlay 0x94, CGADSUB 0x72). Its subscreen is
	 * the shadow, held 256 pixels below the floor. Rain draws that layer
	 * in front of every sprite, so on the bank Link and the man go
	 * translucent and the shadow drops out. Putting the layer behind the
	 * floor hides it under the opaque bank. It belongs between them:
	 * on the floor, behind the sprites. */
	bridge = overlay && !player_is_indoors && (ppu->mathEnabled & 0x02) &&
		!ppu->subtractColor && BYTE(overlay_index) == 0x94;
	if (bridge)
		overlay = 0;
	pic.overlay = overlay;
	pic.back_add = back_add;
	pic.bridge = bridge;
	bg2_main = (main_en & 0x02) != 0;
	bg2_on = ((main_en | sub_en) & 0x02) != 0;
	pic.bg2_on = bg2_on;
	/* Door frames are BG2 priority tiles. Lift them whenever the ground is
	 * on the main screen. In a dungeon that also needs its own BG1, the
	 * scrolls have to match so the tiles can share BG1's front layer. */
	scrolls_match =
		ppu->bgLayer[0].hScroll == ppu->bgLayer[1].hScroll &&
		ppu->bgLayer[0].vScroll == ppu->bgLayer[1].vScroll &&
		ppu->bgLayer[0].tilemapWider == ppu->bgLayer[1].tilemapWider &&
		ppu->bgLayer[0].tilemapHigher == ppu->bgLayer[1].tilemapHigher;
	lift_bg2 = bg2_main && (!bg1_main || scrolls_match);
	pic.lift_bg2 = lift_bg2;
	/* The stair cap is a priority tile. On these floors BG1 is the
	 * subscreen, so the cap and the wall over the opening would sit
	 * behind Link. For the climb, draw both in front of him. */
	stair_arch = !lamp && climbing_stairs() && player_is_indoors &&
		bg1_sub && !bg1_main && !overlay && scrolls_match;
	pic.stair_arch = stair_arch;
	bg3_on = ((main_en | sub_en) & 0x04) != 0;
	pic.show_bg3_lo = bg3_on && !bg2_on;
	pic.door_iris = iris_wanted();
	/* Rain and the lost-woods fog are drawn in front of the sprites and
	 * behind the status bar. With the bar on the bottom screen, that
	 * layer would put it back on top of the fog, so leave the layer off.
	 * A message still needs it for the text box. The triforce chamber
	 * runs that speech from module 25 while the text engine is active. */
	pic.bar_under_overlay = overlay && !ZeldaHudOnTop() &&
		main_module_index != 14 && messaging_module == 0;
}

static void lay_maps(Ppu *ppu) {
	int bake;

	climb_marked = 0;

	/* BG3 priority 0 and BG2 share the back background. Switching which
	 * one owns that map has to upload it again. */
	if (pic.bg2_on) {
		if (back_mode != 0) {
			back_mode = 0;
			map_valid[1] = 0;
		}
	} else if (pic.show_bg3_lo) {
		if (back_mode != 1) {
			back_mode = 1;
			map_valid[2] = 0;
		}
	}

	bg2_front_on_slot1 = pic.lift_bg2 && !(pic.bg1_main || pic.stair_arch);
	/* The black room border is a BG2 priority tile. While the two scrolls
	 * match it is copied onto the shell's layer. Trinexx then moves that
	 * layer, and the border goes with it. Put the shell back on its own
	 * so the border stays with the room. */
	bake = pic.lift_bg2 && (pic.bg1_main || pic.stair_arch);
	if (!bake && bg1_baked) {
		map_valid[0] = 0;
		bg1_baked = 0;
	}
	if (pic.lamp)
		present_bg1(ppu, 0, 1);
	else
		present_bg1(ppu, pic.bg1_main || pic.stair_arch, 0);
	if (pic.bg2_on)
		present_bg2(ppu, pic.lift_bg2,
			climbing_stairs() && pic.bg1_sub && !pic.bg1_main);
	if (bake) {
		bake_bg2_doors(ppu, pic.stair_arch);
		bg1_baked = 1;
	} else if (pic.lift_bg2) {
		show_bg2_front(ppu);
	}
	present_bg3(ppu, pic.show_bg3_lo);
}

/* Called while the previous picture is still up. The overworld writes a
 * new strip as the camera moves. Building that map here, and copying it
 * at the start of the blank with the new scroll, keeps the two together.
 * Doing the build in the blank misses the blank, and the picture is then
 * the new scroll over the old map for a frame. */
void ZeldaHwBuildMaps(Ppu *ppu) {
	if (!ppu || ppu->mode == 7) {
		pic.ready = 0;
		return;
	}
	vram_commit = 0;
	classify_picture(ppu);
	lay_maps(ppu);
	pic.ready = 1;
}

void ZeldaHwPresent(Ppu *ppu) {
	if (!pic.ready)
		ZeldaHwBuildMaps(ppu);
	pic.ready = 0;

	/* The map was built before this blank. Copy it before line 0, with
	 * the scroll that belongs to it. A copy that slips into the picture
	 * snows the frame, and a scroll that lands without the map shifts
	 * the whole overworld for a frame. */
	flush_map_jobs();

	/* The shut circle has to be clipped before line 0. The lamp hole
	 * waits until the scroll below is written. */
	if (pic.door_iris)
		present_iris(ppu);

	/* Offsets, then the iris, then sprites. All three land while VCOUNT is
	 * still in vblank. Tile bytes after them do not fit there. Brightness
	 * stays black through that upload when this frame leaves a force blank.
	 * Spotlight_open leaves a closed circle; the iris is armed before the
	 * upload so that circle is already latched when line 0 is drawn.
	 * Scroll changes on the next line, so it has to be written before the
	 * tile upload. Writing it afterwards splits the picture on whatever
	 * line the upload has reached: running, and the pan into the next room. */
	ZeldaHwUseMasterBright(1);
	if (pic.cover && fade_level < 0)
		REG_MASTER_BRIGHT = inidisp_to_bright(0x80);
	present_scroll(ppu, pic.show_bg3_lo);
	present_wave(pic.show_bg3_lo);
	if (!pic.door_iris)
		present_iris(ppu);
	latch_scroll_window();
	/* Already built before this vblank. Copy it up before the background
	 * upload, or a room of guards is still being written while it is drawn. */
	ZeldaHwCommitSprites(((pic.main_en | pic.sub_en) & 0x10) != 0);
	if (pic.main_en & 0x02)
		iris_lay_backdrop(ppu);
	vram_commit = 1;
	lay_maps(ppu);
	vram_commit = 0;
	/* A stair rebuild can still queue a map. It lands here when the blank
	 * has room, and at the start of the next one when it does not. */
	flush_map_jobs();
	present_palettes(ppu);

	if ((pic.main_en & 0x04) && !pic.bar_under_overlay) {
		bgSetPriority(bg_id[0], 0);
		bgShow(bg_id[0]);
	} else if ((pic.sub_en & 0x04) && !pic.bar_under_overlay) {
		bgSetPriority(bg_id[0], 3);
		bgShow(bg_id[0]);
	} else {
		bgHide(bg_id[0]);
	}

	if (pic.lamp) {
		/* In front of Link and the floor, behind the status bar. The
		 * blend adds this light onto whichever of those is showing.
		 * Door frames stay between the light and the floor. */
		if (lamp_show_light) {
			bgSetPriority(bg_id[2], 0);
			bgShow(bg_id[2]);
		} else {
			bgHide(bg_id[2]);
		}
		if (bg2_front_on_slot1) {
			bgSetPriority(bg_id[1], 1);
			bgShow(bg_id[1]);
		} else {
			bgHide(bg_id[1]);
		}
	} else if (pic.bg1_main) {
		bgSetPriority(bg_id[1], 1);
		bgSetPriority(bg_id[2], 2);
		bgShow(bg_id[1]);
		bgShow(bg_id[2]);
	} else if (pic.overlay) {
		/* In front of every sprite, including a priority-3 ledge jump,
		 * and behind the HUD. Doorways sit between the rain and Link. */
		bgSetPriority(bg_id[2], 0);
		bgShow(bg_id[2]);
		if (bg2_front_on_slot1) {
			bgSetPriority(bg_id[1], 1);
			bgShow(bg_id[1]);
		} else {
			bgHide(bg_id[1]);
		}
	} else if (pic.bridge) {
		/* On the floor, behind every sprite. The stone that covers Link
		 * is the front layer and is not part of this blend. */
		bgSetPriority(bg_id[2], 2);
		bgShow(bg_id[2]);
		if (bg2_front_on_slot1) {
			bgSetPriority(bg_id[1], 1);
			bgShow(bg_id[1]);
		} else {
			bgHide(bg_id[1]);
		}
	} else if (pic.back_add) {
		/* Same priority as the floor, and a lower background number, so
		 * this layer is on top of the floor and behind every sprite.
		 * Opaque pixels blend into the floor. Clear pixels leave it. */
		bgSetPriority(bg_id[2], 2);
		bgShow(bg_id[2]);
		if (bg2_front_on_slot1 || pic.stair_arch) {
			bgSetPriority(bg_id[1], 1);
			bgShow(bg_id[1]);
		} else {
			bgHide(bg_id[1]);
		}
	} else if (pic.bg1_sub) {
		/* Behind the ground, and not blended. Castle's subscreen sits here:
		 * the opaque floor covers it, so the room and Link stay solid.
		 * With no ground (the title), this layer is the picture. Doorways
		 * stay in front of Link. During a stair climb the arch is the
		 * priority part of this layer, and it is on the front layer too. */
		bgSetPriority(bg_id[2], 3);
		bgShow(bg_id[2]);
		if (bg2_front_on_slot1 || pic.stair_arch) {
			bgSetPriority(bg_id[1], 1);
			bgShow(bg_id[1]);
		} else {
			bgHide(bg_id[1]);
		}
	} else if (bg2_front_on_slot1) {
		bgSetPriority(bg_id[1], 1);
		bgShow(bg_id[1]);
		bgHide(bg_id[2]);
	} else {
		bgHide(bg_id[1]);
		bgHide(bg_id[2]);
	}

	if (pic.bg2_on) {
		/* Main-screen BG2 is in the same band as BG1 priority 0: layer 2
		 * (BG1) is in front of layer 3, Link is in front of both, and
		 * sprites 0/1 are behind both. Subscreen-only BG2 is the title
		 * picture, added behind every sprite. */
		bgSetPriority(bg_id[3], (pic.main_en & 0x02) ? 2 : 3);
		bgShow(bg_id[3]);
	} else if (pic.show_bg3_lo) {
		bgSetPriority(bg_id[3], 3);
		bgShow(bg_id[3]);
	} else {
		bgHide(bg_id[3]);
	}
	REG_DISPCNT |= DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D | DISPLAY_SPR_1D_SIZE_32;
	present_mosaic(ppu);
	present_overlay_blend(ppu, pic.overlay, pic.back_add, pic.bridge);
	{
		int ime = enterCriticalSection();

		picture_covered = pic.cover;
		last_inidisp = (pic.cover || iris_fully_closed()) ? 0x80 : INIDISP_copy;
		if (fade_level >= 0)
			apply_forced_fade();
		else if (use_master_bright)
			REG_MASTER_BRIGHT = inidisp_to_bright(last_inidisp);
		leaveCriticalSection(ime);
	}
	/* A map resize during the upload writes the single scroll back.
	 * Put the wave on the line that store would have stuck to. */
	if (wave_publish & 1)
		wave_latch_frame();
}
