#include <nds.h>

#include <string.h>

#include "zelda_rtl.h"
#include "variables.h"
#include "snes/ppu.h"
#include "zelda_bottom.h"
#include "zelda_bottom_map.h"
#include "zelda_save.h"
#include "zelda_video.h"
#include "tex_menu.h"
#include "gear-text.h"
#include "items-text.h"
#include "selected-item-border.h"
#include "life-text.h"
#include "one-half.h"

enum {
	kWidth = 256,
	kHeight = 192,
	kTexW = 48,
	kTexH = 48,
	/* Slots well above the two colors in the background tile. */
	/* Sixteen slots for the game's own HUD colors: rupee, bomb, arrow,
	 * digits, four apiece. Slot 0 of each group is never drawn. */
	kPalHud = 224,
	/* Thirty-two slots for the equipped item: eight BG3 palettes, four each. */
	kPalItem = 192,
	/* Same again for the hearts. */
	kPalHeart = 128,
	/* The LIFE text's own colors; index 0 is clear. */
	kPalLife = 120,
	/* Thirty-two slots for the magic meter, again one group per palette. */
	kPalMagic = 64,
	/* The hearts' shadow over each of the two background colors. */
	kPalShadow = 126,
	/* The selected-item border's own colors; index 0 is clear. */
	kPalBorder = 188,
	/* Thirty-two slots for the items on the item panel, and one for the
	 * panel's title text. Both fade on their own. */
	kPalMenu = 32,
	kPalPanelText = 30,
	/* The LIFE title on the gear panel, four slots, index 0 clear. */
	kPalPanelLife = 26,
	kPalGreen = 252,
	kPalText = 253,
	kPalBlack = 254,
	kPalGold = 255
};

/* A filled rectangle with a solid border inside its w x h, drawn over the
 * background. Boxes are drawn in order. */
typedef struct {
	int x, y, w, h;
	int border_w;
	u8 border;
	u8 fill;
} HudBox;

/* The first two are the buttons; kImages refers to them by index. */
static const HudBox kBoxes[] = {
	{ 5, 163, 122, 24, 1, kPalGold, kPalBlack },
	{ 130, 163, 122, 24, 1, kPalGold, kPalBlack },
	{ 5, 4, 150, 153, 2, kPalGreen, kPalBlack },
	{ 167, 4, 78, 41, 2, kPalGreen, kPalBlack }
};

/* A graphic, centered in one of the boxes, or at (x, y) when box is -1.
 * Palette index 0 is transparent. Every other pixel is drawn in color, or
 * when color is 0, in the slot pal_base plus its own index. Rows are padded
 * to 4 bytes. panel says when it is drawn: always, or only while the big
 * panel is showing that state. */
enum {
	kPanelNormal,
	kPanelItems,
	kPanelGear,
	kPanelAlways = 0xff
};

typedef struct {
	const u8 *pix;
	int w, h;
	int box;
	int x, y;
	u8 color;
	u8 pal_base;
	u8 panel;
} HudImage;

#define STRIDE(w) (((w) + 3) & ~3)

static const HudImage kImages[] = {
	{ (const u8 *)gear_textBitmap, 26, 7, 0, 0, 0, kPalText, 0, kPanelAlways },
	{ (const u8 *)items_textBitmap, 34, 7, 1, 0, 0, kPalText, 0, kPanelAlways },
	{ (const u8 *)selected_item_borderBitmap, 50, 50, -1, 183, 52, 0, kPalBorder, kPanelAlways },
	{ (const u8 *)life_textBitmap, 44, 8, -1, 186, 108, 0, kPalLife, kPanelAlways },
	{ (const u8 *)gear_textBitmap, 26, 7, -1, 66, 11, kPalPanelText, 0, kPanelGear },
	{ (const u8 *)life_textBitmap, 44, 8, -1, 12, 78, 0, kPalPanelLife, kPanelGear },
	{ (const u8 *)items_textBitmap, 34, 7, -1, 63, 11, kPalPanelText, 0, kPanelItems }
};

/* The rupee, bomb and arrow counters. The icons and digits are the game's
 * own 2bpp HUD tiles, read out of BG3 in the PPU's VRAM with the HUD
 * palettes from CGRAM, so they follow whatever the game has loaded. */
enum {
	kIconX = 189,
	kArrowX = 186,
	kRupeeY = 10,
	kBombY = 21,
	kArrowY = 32,
	/* Digits start 3 pixels past the 8-wide rupee and bomb icons. */
	kValueX = kIconX + 8 + 3,
	kTileH = 8,
	kMaxSpriteW = 32,
	/* Rows the counters can touch, so a change redraws only these. */
	kDynTop = kRupeeY,
	kDynBottom = kArrowY + kTileH,

	/* HUD tile numbers in the BG3 character set. */
	kTileRupee = 0xa8,
	kTileBomb = 0x88,
	kTileArrow = 0xa7,
	kTileDigit0 = 0x90,

	kGroupRupee = 0,
	kGroupBomb = 1,
	kGroupArrow = 2,
	kGroupDigit = 3
};

/* BG3 palette numbers the game draws each group with. */
static const u8 kGroupPal[4] = { 7, 3, 0, 1 };

typedef struct {
	int x, y, w, h;
	u8 px[kTileH][kMaxSpriteW];	/* palette slot, 0 is clear */
} HudSprite;

enum { kRupeeIcon, kBombIcon, kArrowIcon, kRupeeNum, kBombNum, kArrowNum, kSprites };

typedef struct {
	HudSprite spr[kSprites];
	u16 pal[16];
} HudCounters;

static HudCounters shown;
static HudCounters next;

static void read_tile(const Ppu *ppu, int tile, u8 out[kTileH][kTileH])
{
	int base = ppu->bgLayer[2].tileAdr + tile * 8;
	int r, x;

	for (r = 0; r < 8; r++) {
		u16 w = ppu->vram[(base + r) & 0x7fff];

		for (x = 0; x < 8; x++)
			out[r][x] = ((w >> (7 - x)) & 1) | (((w >> (15 - x)) & 1) << 1);
	}
}

/* An icon of one or two tiles side by side (right < 0 for one), cropped to
 * the pixels it draws. (x, y) is where the top-left drawn pixel lands. */
static void build_icon(HudSprite *s, const Ppu *ppu, int left, int right,
		       int group, int x, int y)
{
	u8 strip[kTileH][2 * kTileH];
	int n = right < 0 ? 1 : 2;
	int minx = 99, maxx = -1, miny = 99, maxy = -1;
	int t, r, c;

	for (t = 0; t < n; t++) {
		u8 one[kTileH][kTileH];

		read_tile(ppu, t ? right : left, one);
		for (r = 0; r < kTileH; r++)
			memcpy(&strip[r][t * kTileH], one[r], kTileH);
	}
	for (r = 0; r < kTileH; r++) {
		for (c = 0; c < n * kTileH; c++) {
			if (!strip[r][c])
				continue;
			if (c < minx) minx = c;
			if (c > maxx) maxx = c;
			if (r < miny) miny = r;
			if (r > maxy) maxy = r;
		}
	}
	s->x = x;
	s->y = y;
	if (maxx < 0)
		return;
	s->w = maxx - minx + 1;
	s->h = maxy - miny + 1;
	for (r = 0; r < s->h; r++) {
		for (c = 0; c < s->w; c++) {
			u8 p = strip[miny + r][minx + c];

			s->px[r][c] = p ? kPalHud + group * 4 + p : 0;
		}
	}
}

/* A count as digit cells, left aligned at kValueX. Leading zeros stay,
 * as on the game's own bar. */
static void build_number(HudSprite *s, const Ppu *ppu, int value, int digits,
			 int y)
{
	int i, r, c;

	s->x = kValueX;
	s->y = y;
	s->w = digits * kTileH;
	s->h = kTileH;
	memset(s->px, 0, sizeof s->px);
	for (i = digits - 1; i >= 0; i--) {
		u8 cell[kTileH][kTileH];

		read_tile(ppu, kTileDigit0 + value % 10, cell);
		value /= 10;
		for (r = 0; r < kTileH; r++) {
			for (c = 0; c < kTileH; c++) {
				u8 p = cell[r][c];

				s->px[r][i * kTileH + c] =
					p ? kPalHud + kGroupDigit * 4 + p : 0;
			}
		}
	}
}

static void build_counters(HudCounters *d)
{
	const Ppu *ppu = g_zenv.ppu;
	int rupees = link_rupees_actual;
	int g, p;

	memset(d, 0, sizeof *d);
	for (g = 0; g < 4; g++)
		for (p = 1; p < 4; p++)
			d->pal[g * 4 + p] = ppu->cgram[kGroupPal[g] * 4 + p] & 0x7fff;

	build_icon(&d->spr[kRupeeIcon], ppu, kTileRupee, -1, kGroupRupee, kIconX, kRupeeY);
	build_icon(&d->spr[kBombIcon], ppu, kTileBomb, kTileBomb + 1, kGroupBomb, kIconX, kBombY);
	build_icon(&d->spr[kArrowIcon], ppu, kTileArrow, kTileArrow + 2, kGroupArrow, kArrowX, kArrowY);
	/* Three digits, four once the wallet holds a thousand. */
	build_number(&d->spr[kRupeeNum], ppu, rupees, rupees > 999 ? 4 : 3, kRupeeY);
	build_number(&d->spr[kBombNum], ppu, link_item_bombs % 100, 2, kBombY);
	build_number(&d->spr[kArrowNum], ppu, link_num_arrows % 100, 2, kArrowY);
}

static void set_counter_palette(const HudCounters *d)
{
	int i;

	for (i = 1; i < 16; i++)
		BG_PALETTE_SUB[kPalHud + i] = d->pal[i];
}

/* The equipped item, drawn at 2x inside the selected-item border. The game
 * keeps that picture as a 2x2 block of BG3 tile entries: the HUD item box
 * during play, and the menu slot the cursor is on while the item menu is
 * up. The box itself is rewritten only when the menu closes, so the slot
 * is what moves the 2x icon with the cursor. Each pixel is drawn twice
 * across and twice down, with nothing smoothed. */
enum {
	kItemX = 192,
	kItemY = 62,
	kItemSize = 32,
	/* HUDXY(5, 1) in the 32-word-wide HUD buffer. */
	kItemBoxWord = 1 * 32 + 5
};

typedef struct {
	u8 px[kItemSize][kItemSize];	/* palette slot, 0 is clear */
	u16 pal[32];
} HudItem;

static HudItem shown_item;
static HudItem next_item;

static void build_item(HudItem *it, const u16 e[4])
{
	const Ppu *ppu = g_zenv.ppu;
	int t, r, c, p;

	memset(it, 0, sizeof *it);
	for (t = 0; t < 4; t++) {
		u16 entry = e[t];
		int pal = (entry >> 10) & 7;
		u8 tile[kTileH][kTileH];

		read_tile(ppu, entry & 0x3ff, tile);
		for (p = 1; p < 4; p++)
			it->pal[pal * 4 + p] = ppu->cgram[pal * 4 + p] & 0x7fff;
		for (r = 0; r < kTileH; r++) {
			for (c = 0; c < kTileH; c++) {
				int sr = (entry & 0x8000) ? kTileH - 1 - r : r;
				int sc = (entry & 0x4000) ? kTileH - 1 - c : c;
				u8 v = tile[sr][sc];
				int x = ((t & 1) * kTileH + c) * 2;
				int y = ((t >> 1) * kTileH + r) * 2;

				if (!v)
					continue;
				v += kPalItem + pal * 4;
				it->px[y][x] = v;
				it->px[y][x + 1] = v;
				it->px[y + 1][x] = v;
				it->px[y + 1][x + 1] = v;
			}
		}
	}
}

static void set_item_palette(const HudItem *it)
{
	int i;

	for (i = 0; i < 32; i++)
		if (i & 3)
			BG_PALETTE_SUB[kPalItem + i] = it->pal[i];
}

/* The hearts. The game lays them out as tile entries in its HUD buffer,
 * ten to a row and two rows at most, empty, half and full alike, so the
 * capacity and the current health are already in there. Each heart is a
 * 7x7 shape inside its 8x8 tile, and (kHeartX, kHeartY) is where the first
 * heart's top-left drawn pixel lands.
 *
 * The drop shadow is part of the sprite. Every drawn heart pixel marks the
 * pixel one down and one right as kShadowMark, if nothing is there. That is
 * worked out when the hearts change, and drawing it costs the same as
 * drawing any other pixel. compose_row turns the mark into the background
 * darkened. */
enum {
	kHeartX = 169,
	kHeartY = 119,
	kHeartCols = 10,
	kHeartRows = 2,
	kHeartW = kHeartCols * kTileH,
	kHeartH = kHeartRows * kTileH,
	/* HUDXY(20, 1) in the 32-word-wide HUD buffer. */
	kHeartWord = 1 * 32 + 20,
	/* Slot 1 is never a heart color, so it can stand for shadow. */
	kShadowMark = 1
};

typedef struct {
	u8 px[kHeartH][kHeartW];	/* palette slot, 0 clear, kShadowMark shadow */
	u16 pal[32];
} HudHearts;

static HudHearts shown_hearts;
static HudHearts next_hearts;

static void build_hearts(HudHearts *h)
{
	const Ppu *ppu = g_zenv.ppu;
	const u16 *src = &hud_tile_indices_buffer[kHeartWord];
	int i, r, c, p, x, y;

	memset(h, 0, sizeof *h);
	for (i = 0; i < kHeartCols * kHeartRows; i++) {
		u16 entry = src[(i / kHeartCols) * 32 + i % kHeartCols];
		int pal = (entry >> 10) & 7;
		u8 tile[kTileH][kTileH];

		read_tile(ppu, entry & 0x3ff, tile);
		for (p = 1; p < 4; p++)
			h->pal[pal * 4 + p] = ppu->cgram[pal * 4 + p] & 0x7fff;
		for (r = 0; r < kTileH; r++) {
			for (c = 0; c < kTileH; c++) {
				int sr = (entry & 0x8000) ? kTileH - 1 - r : r;
				int sc = (entry & 0x4000) ? kTileH - 1 - c : c;
				u8 v = tile[sr][sc];

				/* The shape starts one pixel into its tile. */
				x = (i % kHeartCols) * kTileH + c - 1;
				y = (i / kHeartCols) * kTileH + r - 1;
				if (!v || x < 0 || y < 0)
					continue;
				h->px[y][x] = kPalHeart + pal * 4 + v;
			}
		}
	}
	for (y = 0; y < kHeartH - 1; y++) {
		for (x = 0; x < kHeartW - 1; x++) {
			if (h->px[y][x] > kShadowMark && !h->px[y + 1][x + 1])
				h->px[y + 1][x + 1] = kShadowMark;
		}
	}
}

static void set_heart_palette(const HudHearts *h)
{
	int i;

	for (i = 0; i < 32; i++)
		if (i & 3)
			BG_PALETTE_SUB[kPalHeart + i] = h->pal[i];
}

/* A BG3 tile entry as pixels, with its flips applied. */
static void read_entry(const Ppu *ppu, u16 entry, u8 out[kTileH][kTileH])
{
	u8 raw[kTileH][kTileH];
	int r, c;

	read_tile(ppu, entry & 0x3ff, raw);
	for (r = 0; r < kTileH; r++) {
		for (c = 0; c < kTileH; c++) {
			int sr = (entry & 0x8000) ? kTileH - 1 - r : r;
			int sc = (entry & 0x4000) ? kTileH - 1 - c : c;

			out[r][c] = raw[sr][sc];
		}
	}
}

/* The magic meter, turned a quarter turn clockwise so it lies on its side
 * and fills left to right, and made longer in the middle.
 *
 * The game's meter is a capsule 16 pixels across inside a block of 3 x 6
 * tiles, standing up and filling from the bottom. Its ends are rounded
 * caps and between them is a straight pocket of 32 rows. Here the caps keep
 * their pixels and only the pocket grows, from 32 to kMeterPocket, so a full
 * sprite is 5 + 69 + 5 = 79 long. The game replaces that top tile with
 * its own 1/2, so the plain cap is read instead and the label rows above
 * it are left off.
 *
 * The game fills its pocket in steps of 8 magic, which would stretch to
 * steps 2 or 3 pixels wide. Instead the fill is drawn from link_magic_power
 * itself, 0 to 128, over the whole pocket. Pocket rows are cut from the
 * game's own tiles: an empty row, a filled row, and the bright row that
 * leads the fill. The frame and the bottom cap are read live from the
 * HUD buffer. */
enum {
	kMeterX = 169,
	kMeterY = 140,
	kMeterThick = 16,
	/* Where the capsule sits in its 3-tile-wide block. */
	kSrcLeft = 4,
	kTopRows = 8,		/* source rows 0..7 of the plain top tile */
	kTopCapRows = 5,	/* source rows 3..7, the rounded end */
	kBottomRows = 5,	/* source rows 40..44 */
	kMeterPocket = 69,
	kMeterLen = kBottomRows + kMeterPocket + kTopCapRows,
	kMagicMax = 128,
	/* HUDXY(2, 0) in the 32-word-wide HUD buffer. */
	kMagicWord = 2,
	/* Fill tiles from the game's own meter table. */
	kFillEmpty = 0x3cf5,
	kFillFull = 0x3c5e,
	kFillEdge = 0x3c4e,
	/* The 1/2 upgrade's label, just above the meter. */
	kHalfX = 203,
	kHalfY = 136,
	kHalfW = 13,
	kHalfH = 7
};

typedef struct {
	u8 px[kMeterThick][kMeterLen];	/* palette slot, 0 is clear */
	u16 pal[32];
} HudMagic;

static HudMagic shown_magic;
static HudMagic next_magic;
static int shown_half;

static void note_pal(HudMagic *m, const Ppu *ppu, u16 entry)
{
	int pal = (entry >> 10) & 7;
	int p;

	for (p = 1; p < 4; p++)
		m->pal[pal * 4 + p] = ppu->cgram[pal * 4 + p] & 0x7fff;
}

static u8 slot_for(u16 entry, u8 v)
{
	return v ? kPalMagic + ((entry >> 10) & 7) * 4 + v : 0;
}

/* One cross-section of the pocket, 16 pixels, with the given fill tile
 * between the frame's two sides. */
static void pocket_section(HudMagic *m, const Ppu *ppu, const u16 *buf,
			   u16 fill, u8 out[kMeterThick])
{
	/* Tile row 1 is straight pocket: frame, fill, frame. */
	u16 entry[3] = { buf[32 + kMagicWord], fill, buf[32 + kMagicWord + 2] };
	u8 tile[3][kTileH][kTileH];
	int i, ty;

	for (i = 0; i < 3; i++) {
		read_entry(ppu, entry[i], tile[i]);
		note_pal(m, ppu, entry[i]);
	}
	for (ty = 0; ty < kMeterThick; ty++) {
		int x = ty + kSrcLeft;

		out[ty] = slot_for(entry[x >> 3], tile[x >> 3][0][x & 7]);
	}
}

static void build_magic(HudMagic *m)
{
	/* Row 0 of the meter becomes the game's 1/2. These are the plain cap. */
	static const u16 kPlainTop[3] = { 0x2850, 0xa856, 0x2852 };
	const Ppu *ppu = g_zenv.ppu;
	const u16 *buf = &hud_tile_indices_buffer[0];
	u8 section_empty[kMeterThick], section_full[kMeterThick], section_edge[kMeterThick];
	int power = link_magic_power;
	int filled, tx, ty, i, t;
	u8 caps[2][3][kTileH][kTileH];
	u16 cap_entry[2][3];

	memset(m, 0, sizeof *m);
	if (power > kMagicMax)
		power = kMagicMax;
	/* Round up, so any magic at all shows. */
	filled = (power * kMeterPocket + kMagicMax - 1) / kMagicMax;

	/* Tile rows 0 and 5 of the block hold the two caps. */
	for (t = 0; t < 2; t++) {
		for (i = 0; i < 3; i++) {
			cap_entry[t][i] = t ? buf[5 * 32 + kMagicWord + i]
					    : kPlainTop[i];
			read_entry(ppu, cap_entry[t][i], caps[t][i]);
			note_pal(m, ppu, cap_entry[t][i]);
		}
	}
	pocket_section(m, ppu, buf, kFillEmpty, section_empty);
	pocket_section(m, ppu, buf, kFillFull, section_full);
	pocket_section(m, ppu, buf, kFillEdge, section_edge);

	for (tx = 0; tx < kMeterLen; tx++) {
		for (ty = 0; ty < kMeterThick; ty++) {
			int x = ty + kSrcLeft;
			u8 v;

			if (tx < kBottomRows) {
				/* Source row 44 - tx, row 4 - tx of the bottom tiles. */
				v = slot_for(cap_entry[1][x >> 3],
					     caps[1][x >> 3][kBottomRows - 1 - tx][x & 7]);
			} else if (tx < kBottomRows + kMeterPocket) {
				int k = tx - kBottomRows;

				v = k < filled - 1 ? section_full[ty] :
				    k == filled - 1 ? section_edge[ty] :
				    section_empty[ty];
			} else {
				/* Source row 7 - j of the top tiles. */
				int j = tx - kBottomRows - kMeterPocket;

				v = slot_for(cap_entry[0][x >> 3],
					     caps[0][x >> 3][kTopRows - 1 - j][x & 7]);
			}
			m->px[ty][tx] = v;
		}
	}
}

static void set_magic_palette(const HudMagic *m)
{
	int i;

	for (i = 0; i < 32; i++)
		if (i & 3)
			BG_PALETTE_SUB[kPalMagic + i] = m->pal[i];
}

/* link_magic_consumption == 1 is the 1/2 magic upgrade. */
static int half_magic(void)
{
	return link_magic_consumption == 1;
}

/* Black and white, drawn with the HUD's own black and white. Index 0 of
 * the bitmap is clear; a 0 palette entry is black and anything else white. */
static void draw_half_row(u8 *line, int y)
{
	const u8 *src;
	int x;

	if (!half_magic() || y < kHalfY || y >= kHalfY + kHalfH)
		return;
	src = (const u8 *)one_halfBitmap + (y - kHalfY) * STRIDE(kHalfW);
	for (x = 0; x < kHalfW; x++) {
		if (src[x])
			line[kHalfX + x] = one_halfPal[src[x]] ? kPalText : kPalBlack;
	}
}

/* The item panel. The game keeps its item menu as a 32-word-wide tile map
 * in uvram_screen: each of the 20 slots is a 2x2 block of BG3 tile entries
 * at a fixed spot, and the blinking cursor is a ring of 12 more entries
 * around the selected slot, drawn only while the blink is on. The game's
 * own D-pad code runs the menu, so reading those entries gives the icons,
 * their order, and the cursor with no menu logic here.
 *
 * Slots are laid out 16 wide with kMenuGapX between them and 16 tall with
 * kMenuGapY between them, from (kMenuX, kMenuY). The cursor ring hugs its
 * slot, 8 pixels out, as it does on the SNES.
 *
 * Tiles are decoded a row at a time as the screen row is built, so a
 * snapshot is only tile entries and the palette. */
enum {
	kMenuX = 22,
	kMenuY = 29,
	kMenuGapX = 9,
	kMenuGapY = 14,
	kMenuCols = 5,
	kMenuRows = 4,
	kMenuItems = kMenuCols * kMenuRows,
	kMenuIcon = 16,
	kMenuPitchX = kMenuIcon + kMenuGapX,
	kMenuPitchY = kMenuIcon + kMenuGapY,
	/* Rows an icon or its ring can touch. */
	kMenuTop = kMenuY - kTileH,
	kMenuBottom = kMenuY + (kMenuRows - 1) * kMenuPitchY + kMenuIcon + kTileH,
	/* Cursor ring corner, and the blank tile the box is filled with. */
	kRingTile = 0x60,
	kBlankTile = 0xf5,
	/* Inside the big panel's border. */
	kPanelTop = 6,
	kPanelBottom = 155,
	kPanelRowsPerFrame = 30,
	kPanelStep = 2,
	kPanelBlack = 16
};

typedef struct {
	u16 icon[kMenuItems][4];
	u16 ring[4][4];		/* entries at offsets -1..2; the middle is unused */
	int ring_item;		/* -1 while the cursor is blinked off */
	u16 pal[32];
} HudMenu;

static HudMenu shown_menu;
static HudMenu next_menu;

/* What the big panel is showing, and how far faded to black it is. */
static int panel_shown;
static int panel_level;
/* Next row of the panel to draw while it is black, or -1. */
static int panel_redraw = -1;

/* HUDXY(4 + 3 * col, 7 + 3 * row) in uvram_screen. */
static int menu_word(int i)
{
	return (7 + 3 * (i / kMenuCols)) * 32 + 4 + 3 * (i % kMenuCols);
}

static void build_menu(HudMenu *m)
{
	const Ppu *ppu = g_zenv.ppu;
	const u16 *ui = (const u16 *)(g_ram + 0x1000);
	int i, dx, dy;

	memset(m, 0, sizeof *m);
	m->ring_item = -1;
	for (i = 0; i < 32; i++)
		if (i & 3)
			m->pal[i] = ppu->cgram[i] & 0x7fff;
	for (i = 0; i < kMenuItems; i++) {
		const u16 *p = ui + menu_word(i);

		m->icon[i][0] = p[0];
		m->icon[i][1] = p[1];
		m->icon[i][2] = p[32];
		m->icon[i][3] = p[33];
		/* The ring's top-left corner is unflipped. The corner a slot
		 * shares with its right neighbor is flipped, so this is the
		 * selected slot alone. */
		if ((p[-33] & 0xc3ff) != kRingTile)
			continue;
		m->ring_item = i;
		for (dy = 0; dy < 4; dy++)
			for (dx = 0; dx < 4; dx++)
				if (dx == 0 || dx == 3 || dy == 0 || dy == 3)
					m->ring[dy][dx] = p[(dy - 1) * 32 + dx - 1];
	}
}

/* One pixel row of a BG3 tile entry, written at dst. Clear pixels are
 * left alone. */
static void draw_entry_row(u8 *dst, const Ppu *ppu, u16 entry, int ty)
{
	int tile = entry & 0x3ff;
	int base = (entry >> 10) & 7;
	u16 w;
	int x;

	if (tile == kBlankTile)
		return;
	if (entry & 0x8000)
		ty = kTileH - 1 - ty;
	w = ppu->vram[(ppu->bgLayer[2].tileAdr + tile * 8 + ty) & 0x7fff];
	if (!w)
		return;
	for (x = 0; x < kTileH; x++) {
		int bit = (entry & 0x4000) ? x : kTileH - 1 - x;
		int v = ((w >> bit) & 1) | (((w >> (8 + bit)) & 1) << 1);

		if (v)
			dst[x] = kPalMenu + base * 4 + v;
	}
}

static void compose_menu(u8 *line, int y)
{
	const Ppu *ppu = g_zenv.ppu;
	const HudMenu *m = &shown_menu;
	int i, dx, dy;

	if (y < kMenuTop || y >= kMenuBottom)
		return;
	for (i = 0; i < kMenuItems; i++) {
		int ix = kMenuX + (i % kMenuCols) * kMenuPitchX;
		int r = y - (kMenuY + (i / kMenuCols) * kMenuPitchY);

		if (r < 0 || r >= kMenuIcon)
			continue;
		draw_entry_row(line + ix, ppu, m->icon[i][(r >> 3) * 2], r & 7);
		draw_entry_row(line + ix + kTileH, ppu, m->icon[i][(r >> 3) * 2 + 1], r & 7);
	}
	if (m->ring_item < 0)
		return;
	for (dy = -1; dy <= 2; dy++) {
		int ix = kMenuX + (m->ring_item % kMenuCols) * kMenuPitchX;
		int r = y - (kMenuY + (m->ring_item / kMenuCols) * kMenuPitchY + dy * kTileH);

		if (r < 0 || r >= kTileH)
			continue;
		for (dx = -1; dx <= 2; dx++) {
			if (dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1)
				continue;
			draw_entry_row(line + ix + dx * kTileH, ppu,
				       m->ring[dy + 1][dx + 1], r);
		}
	}
}

/* Rows of the menu that differ between two snapshots, as [*lo, *hi). */
static void note_rows(int *lo, int *hi, int top, int bottom)
{
	if (top < *lo)
		*lo = top;
	if (bottom > *hi)
		*hi = bottom;
}

static void item_rows(int i, int ring, int *lo, int *hi)
{
	int top = kMenuY + (i / kMenuCols) * kMenuPitchY;

	if (ring)
		note_rows(lo, hi, top - kTileH, top + kMenuIcon + kTileH);
	else
		note_rows(lo, hi, top, top + kMenuIcon);
}

static void menu_changed_rows(const HudMenu *a, const HudMenu *b, int *lo, int *hi)
{
	int i;

	*lo = 9999;
	*hi = -1;
	if (memcmp(a->pal, b->pal, sizeof a->pal)) {
		note_rows(lo, hi, kMenuTop, kMenuBottom);
		return;
	}
	for (i = 0; i < kMenuItems; i++)
		if (memcmp(a->icon[i], b->icon[i], sizeof a->icon[i]))
			item_rows(i, 0, lo, hi);
	if (a->ring_item != b->ring_item || memcmp(a->ring, b->ring, sizeof a->ring)) {
		if (a->ring_item >= 0)
			item_rows(a->ring_item, 1, lo, hi);
		if (b->ring_item >= 0)
			item_rows(b->ring_item, 1, lo, hi);
	}
}

/* The gear panel: what Link has, read from the same variables and tile
 * entries the game's own equipment and progress boxes use, so each icon is
 * the game's art. Anything not owned is simply not drawn.
 *
 *   row 1  sword, shield, tunic, mitt, boots, flippers, moon pearl
 *   row 2  BOTTLES: four bottles          PENDANTS: three
 *   row 3  LIFE: current hearts           CRYSTALS: four, then three
 *   row 4  HEART PIECES: the four-part heart    KEYS: only inside a dungeon
 *
 * Every picture is a list of 8x8 tile cells (x, y, tile entry). The cells
 * are always added in the same order, empty ones with entry 0, so two
 * snapshots line up cell for cell and a change redraws only its own rows.
 * The labels are a small built-in font, drawn straight into the row. */
enum {
	kGearLeft = 12,
	kGearRight = 96,
	kGearIconY = 24,
	kGearIconPitch = 20,
	kGearBottleLabelY = 46,
	kGearBottleY = 56,
	kGearPendantPitch = 18,
	/* The heart shape starts one pixel into its tile. */
	kGearHeartY = 88 - 1,
	kGearHeartX = kGearLeft - 1,
	kGearCrystalLabelY = 78,
	kGearCrystalY = 88,
	kGearCrystalRowStep = 10,
	kGearPiecesLabelY = 122,
	kGearPiecesY = 132,
	/* The key icon is 8 tall, centered on the 16-tall heart piece icon. */
	kGearKeyY = kGearPiecesY + 4,
	kGearKeyDigitX = kGearRight + kTileH + 2,
	kGearTop = 24,
	kGearBottom = 150,
	kGearMaxCells = 128
};

typedef struct {
	s16 x, y;
	u16 e;
} GearCell;

typedef struct {
	int n;
	/* Keys in the current dungeon, or -1 outside one. */
	int keys;
	GearCell c[kGearMaxCells];
	u16 pal[32];
} HudGear;

static HudGear shown_gear;
static HudGear next_gear;

/* The game's 2x2 icons, copied from its item tables (hud.c). */
static const u16 kGSword[5][4] = {
	{ 0, 0, 0, 0 },
	{ 0x2c64, 0x2cce, 0x2c75, 0x3d25 },
	{ 0x2c8a, 0x2c65, 0x2474, 0x3d26 },
	{ 0x248a, 0x2465, 0x3c74, 0x2d48 },
	{ 0x288a, 0x2865, 0x2c74, 0x2d39 }
};
static const u16 kGShield[4][4] = {
	{ 0, 0, 0, 0 },
	{ 0x2cfd, 0x6cfd, 0x2cfe, 0x6cfe },
	{ 0x34ff, 0x74ff, 0x349f, 0x749f },
	{ 0x2880, 0x2881, 0x288d, 0x288e }
};
static const u16 kGArmor[3][4] = {
	{ 0x3c68, 0x7c68, 0x3c78, 0x7c78 },
	{ 0x2c68, 0x6c68, 0x2c78, 0x6c78 },
	{ 0x2468, 0x6468, 0x2478, 0x6478 }
};
static const u16 kGGloves[3][4] = {
	{ 0, 0, 0, 0 },
	{ 0x2130, 0x2131, 0x2140, 0x2141 },
	{ 0x28da, 0x28db, 0x28ea, 0x28eb }
};
static const u16 kGBoots[4] = { 0x3429, 0x342a, 0x342b, 0x342c };
static const u16 kGFlippers[4] = { 0x2c9a, 0x2c9b, 0x2c9d, 0x2c9e };
static const u16 kGPearl[4] = { 0x2433, 0x2434, 0x2435, 0x2436 };
static const u16 kGBottles[9][4] = {
	{ 0, 0, 0, 0 },
	{ 0x2044, 0x2045, 0x2046, 0x2047 },
	{ 0x2837, 0x2838, 0x2cc3, 0x2cd3 },
	{ 0x24d2, 0x64d2, 0x24e2, 0x24e3 },
	{ 0x3cd2, 0x7cd2, 0x3ce2, 0x3ce3 },
	{ 0x2cd2, 0x6cd2, 0x2ce2, 0x2ce3 },
	{ 0x2855, 0x6855, 0x2c57, 0x2c5a },
	{ 0x2837, 0x2838, 0x2839, 0x283a },
	{ 0x2837, 0x2838, 0x2839, 0x283a }
};
static const u16 kGPendant[3][4] = {
	{ 0x252b, 0x252c, 0x252d, 0x252e },
	{ 0x2d2b, 0x2d2c, 0x2d2d, 0x2d2e },
	{ 0x3d2b, 0x3d2c, 0x3d2d, 0x3d2e }
};
static const u16 kGHeartPieces[4][4] = {
	{ 0x2484, 0x6484, 0x2485, 0x6485 },
	{ 0x24ad, 0x6484, 0x2485, 0x6485 },
	{ 0x24ad, 0x6484, 0x24ae, 0x6485 },
	{ 0x24ad, 0x64ad, 0x24ae, 0x6485 }
};

/* The status bar's key icon, and its digits (palette 1, tile 0x90 up). */
enum {
	kGKeyIcon = 0x2871,
	kGKeyDigit0 = 0x2400 | kTileDigit0
};

static void gear_cell(HudGear *g, int x, int y, u16 e)
{
	if (g->n >= kGearMaxCells)
		return;
	g->c[g->n].x = (s16)x;
	g->c[g->n].y = (s16)y;
	g->c[g->n].e = e;
	g->n++;
}

/* A 16x16 icon, or empty (v == NULL) so the cell count never changes. */
static void gear_icon(HudGear *g, int x, int y, const u16 *v)
{
	int i;

	for (i = 0; i < 4; i++)
		gear_cell(g, x + (i & 1) * kTileH, y + (i >> 1) * kTileH, v ? v[i] : 0);
}

/* The inventory crystal, one 16x8 picture. Owned or not, both tiles are
 * added so the cell count never changes. */
static void gear_crystal(HudGear *g, int x, int y, int got)
{
	gear_cell(g, x, y, got ? 0x2d44 : 0);
	gear_cell(g, x + kTileH, y, got ? 0x2d45 : 0);
}

static int clamp_to(int v, int max)
{
	return v < 0 ? 0 : v > max ? max : v;
}

static void build_gear(HudGear *g)
{
	const Ppu *ppu = g_zenv.ppu;
	const u16 *hearts = &hud_tile_indices_buffer[kHeartWord];
	int sword = link_sword_type == 0xff ? 0 : clamp_to(link_sword_type, 4);
	int x, i, r, c;

	memset(g, 0, sizeof *g);
	for (i = 0; i < 32; i++)
		if (i & 3)
			g->pal[i] = ppu->cgram[i] & 0x7fff;

	/* Row 1: sword, shield, tunic, mitt, boots, flippers, moon pearl. */
	x = kGearLeft;
	gear_icon(g, x, kGearIconY, kGSword[sword]);
	x += kGearIconPitch;
	gear_icon(g, x, kGearIconY, kGShield[clamp_to(link_shield_type, 3)]);
	x += kGearIconPitch;
	gear_icon(g, x, kGearIconY, kGArmor[clamp_to(link_armor, 2)]);
	x += kGearIconPitch;
	gear_icon(g, x, kGearIconY, kGGloves[clamp_to(link_item_gloves, 2)]);
	x += kGearIconPitch;
	gear_icon(g, x, kGearIconY, link_item_boots ? kGBoots : NULL);
	x += kGearIconPitch;
	gear_icon(g, x, kGearIconY, link_item_flippers ? kGFlippers : NULL);
	x += kGearIconPitch;
	gear_icon(g, x, kGearIconY, link_item_moon_pearl ? kGPearl : NULL);

	/* Row 2: four bottles, three pendants. */
	for (i = 0; i < 4; i++)
		gear_icon(g, kGearLeft + i * kGearIconPitch, kGearBottleY,
			  kGBottles[clamp_to(link_bottle_info[i], 8)]);
	for (i = 0; i < 3; i++)
		gear_icon(g, kGearRight + i * kGearPendantPitch, kGearBottleY,
			  ((link_which_pendants >> i) & 1) ? kGPendant[i] : NULL);

	/* Row 3: the hearts as the status bar has them, ten to a row. */
	for (r = 0; r < kHeartRows; r++)
		for (c = 0; c < kHeartCols; c++)
			gear_cell(g, kGearHeartX + c * kTileH, kGearHeartY + r * kTileH,
				  hearts[r * 32 + c]);
	/* The same crystal seven times. Top row, right to left: 1 2 3 4.
	 * The row under it, also right to left and centered: 5 6 7.
	 * The save's bits are not in that order. 1 Palace of Darkness,
	 * 2 Swamp, 3 Skull Woods, 4 Thieves' Town, 5 Ice Palace,
	 * 6 Misery Mire, 7 Turtle Rock. */
	{
		static const u8 kCrystalX[7] = {
			137, 123, 109, 95,
			130, 116, 102
		};
		static const u8 kCrystalRow[7] = { 0, 0, 0, 0, 1, 1, 1 };
		static const u8 kCrystalBit[7] = { 1, 4, 6, 5, 2, 0, 3 };

		for (i = 0; i < 7; i++)
			gear_crystal(g, kCrystalX[i],
				     kGearCrystalY + kCrystalRow[i] * kGearCrystalRowStep,
				     (link_has_crystals >> kCrystalBit[i]) & 1);
	}

	/* Row 4: pieces of heart. */
	gear_icon(g, kGearLeft, kGearPiecesY, kGHeartPieces[clamp_to(link_heart_pieces, 3)]);

	/* The same row: small keys, which the game marks as being in a
	 * dungeon by keeping the count at 0xff everywhere else. The icon and
	 * digit are the status bar's own, and the digit is its last decimal
	 * place. The cells are always added so the count never changes. */
	g->keys = link_num_keys == 0xff ? -1 : link_num_keys;
	gear_cell(g, kGearRight, kGearKeyY, g->keys >= 0 ? kGKeyIcon : 0);
	gear_cell(g, kGearKeyDigitX, kGearKeyY,
		  g->keys >= 0 ? kGKeyDigit0 + g->keys % 10 : 0);
}

/* Capital letters 5 pixels wide and 7 tall, like the panel titles; I is 3
 * wide. One byte a row, the left pixel in the top bit of the width. */
typedef struct {
	char ch;
	u8 w;
	u8 row[7];
} Glyph;

static const Glyph kGlyphs[] = {
	{ 'A', 5, { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'B', 5, { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
	{ 'C', 5, { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
	{ 'D', 5, { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e } },
	{ 'E', 5, { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
	{ 'H', 5, { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'I', 3, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x02, 0x07 } },
	{ 'K', 5, { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
	{ 'L', 5, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
	{ 'N', 5, { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
	{ 'O', 5, { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'P', 5, { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'R', 5, { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
	{ 'S', 5, { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
	{ 'T', 5, { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'Y', 5, { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 } }
};

enum { kGlyphH = 7, kSpaceW = 3 };

typedef struct {
	int x, y;
	const char *text;
	u8 dungeon_only;
} GearLabel;

static const GearLabel kGearLabels[] = {
	{ kGearLeft, kGearBottleLabelY, "BOTTLES", 0 },
	{ kGearRight, kGearBottleLabelY, "PENDANTS", 0 },
	{ kGearRight, kGearCrystalLabelY, "CRYSTALS", 0 },
	{ kGearLeft, kGearPiecesLabelY, "HEART PIECES", 0 },
	{ kGearRight, kGearPiecesLabelY, "KEYS", 1 }
};

static void draw_label_row(u8 *line, int y, const GearLabel *l)
{
	const char *s;
	int x = l->x;
	int r = y - l->y;

	if (r < 0 || r >= kGlyphH)
		return;
	for (s = l->text; *s; s++) {
		unsigned i;

		if (*s == ' ') {
			x += kSpaceW;
			continue;
		}
		for (i = 0; i < sizeof kGlyphs / sizeof kGlyphs[0]; i++) {
			const Glyph *g = &kGlyphs[i];
			int col;

			if (g->ch != *s)
				continue;
			for (col = 0; col < g->w; col++)
				if ((g->row[r] >> (g->w - 1 - col)) & 1)
					line[x + col] = kPalPanelText;
			x += g->w + 1;
			break;
		}
	}
}

static void compose_gear(u8 *line, int y)
{
	const Ppu *ppu = g_zenv.ppu;
	const HudGear *g = &shown_gear;
	unsigned i;
	int k;

	for (i = 0; i < sizeof kGearLabels / sizeof kGearLabels[0]; i++)
		if (!kGearLabels[i].dungeon_only || g->keys >= 0)
			draw_label_row(line, y, &kGearLabels[i]);
	if (y < kGearTop || y >= kGearBottom)
		return;
	for (k = 0; k < g->n; k++) {
		const GearCell *c = &g->c[k];

		if (c->e && y >= c->y && y < c->y + kTileH)
			draw_entry_row(line + c->x, ppu, c->e, y - c->y);
	}
}

static void gear_changed_rows(const HudGear *a, const HudGear *b, int *lo, int *hi)
{
	int i;

	*lo = 9999;
	*hi = -1;
	if (a->n != b->n || memcmp(a->pal, b->pal, sizeof a->pal)) {
		note_rows(lo, hi, kGearTop, kGearBottom);
		return;
	}
	/* The KEYS label comes and goes with the dungeon. */
	if ((a->keys >= 0) != (b->keys >= 0))
		note_rows(lo, hi, kGearPiecesLabelY, kGearPiecesLabelY + kGlyphH);
	for (i = 0; i < a->n; i++)
		if (a->c[i].e != b->c[i].e)
			note_rows(lo, hi, a->c[i].y, a->c[i].y + kTileH);
}

static u16 dim(u16 c, int level)
{
	int keep = kPanelBlack - level;

	if (!level)
		return c;

	return RGB15(((c >> 0) & 31) * keep / kPanelBlack,
		     ((c >> 5) & 31) * keep / kPanelBlack,
		     ((c >> 10) & 31) * keep / kPanelBlack);
}

/* The mirror fade. Positive adds white; negative holds every channel at
 * least that high, which is the trip back down from white. */
static u16 wash(u16 c, int white)
{
	int ch[3], i;

	if (!white)
		return c;
	for (i = 0; i < 3; i++) {
		int v = (c >> (5 * i)) & 31;

		if (white > 0) {
			v += white;
			if (v > 31)
				v = 31;
		} else if (v < -white) {
			v = -white;
		}
		ch[i] = v;
	}
	return RGB15(ch[0], ch[1], ch[2]);
}

/* The panel's colors at the current fade. The box behind them is black, so
 * dimming each color toward black is the same as fading the panel. */
static void set_panel_palette(void)
{
	const u16 *pal = panel_shown == kPanelGear ? shown_gear.pal : shown_menu.pal;
	int i;

	/* The map uses the same slots, and all it needs besides. */
	if (panel_shown == kPanelNormal) {
		const u16 *map = ZeldaMapColors();
		int white = ZeldaMapMirrorWhite();

		for (i = 0; i < kMapColors; i++)
			BG_PALETTE_SUB[ZeldaMapSlot(i)] =
				dim(wash(map[i], white), panel_level);
		return;
	}
	for (i = 0; i < 32; i++)
		if (i & 3)
			BG_PALETTE_SUB[kPalMenu + i] = dim(pal[i], panel_level);
	for (i = 1; i < 4; i++)
		BG_PALETTE_SUB[kPalPanelLife + i] = dim(life_textPal[i], panel_level);
	BG_PALETTE_SUB[kPalPanelText] = dim(RGB15(31, 31, 31), panel_level);
}

static int hud_up;

void ZeldaBottomNoteIdle(void)
{
	hud_up = 0;
}

/* Modules 6 through 19 are the underworld and overworld loads, play, the
 * item menu, the iris, the transitions, and death. 22 is the crystal
 * prize, which refills the same bar. 24 is Agahnim's defeat, the flute
 * bird through the bat at the pyramid: the idle picture takes this screen
 * for that, then play fades back. The title, the file screens, attract,
 * and the ending are outside this too. */
static int in_game(void)
{
	return (main_module_index >= 6 && main_module_index <= 19) ||
		main_module_index == 22;
}

/* The flute bird is called here, and the cutscene runs until Link is set
 * down at the pyramid. */
static int bird_cutscene(void)
{
	return main_module_index == 24;
}

/* Agahnim's mirror warp into the dark world. It runs through the arrival
 * and Sahasrahla's telepathy, and ends when Link can move again. */
static int aga_warp(void)
{
	return main_module_index == 21;
}

/* X opens the dungeon map (submodule 3 of the menu module) or the overworld
 * map (submodule 7). The flute's bird map is submodule 10. Choosing a place
 * leaves state 5 for 6, which only fades the world map out; Link is still
 * standing where he called the bird, so the mini map would come back on
 * the old spot. State 7 loads the chosen place, which is what moves the
 * map, and leaves the state at 8. The idle picture stays until then.
 * Start's item menu is submodule 1 and is not this. */
static int map_open(void)
{
	if (main_module_index != 14)
		return 0;
	if (submodule_index == 3 || submodule_index == 7)
		return 1;
	return submodule_index == 10 && overworld_map_state < 8;
}

/* GAME OVER letters and the continue choice. The collapse before that
 * still shows the playfield, so the HUD stays. A fairy revival skips this
 * and keeps the HUD while the hearts fill back up. */
static int game_over_screen(void)
{
	return main_module_index == 18 &&
		submodule_index >= 7 && submodule_index <= 9;
}

/* The HUD shows during play, except while a map or the game over screen
 * is up. Then the bottom screen goes back to the idle picture. */
static int hud_wanted(void)
{
	return !ZeldaHudOnTop() && in_game() && !map_open() && !game_over_screen();
}

/* The item menu is up from its first drawn frame until Start closes it.
 * Map states 0 and 1 are still clearing and filling the tile map, and 5 and
 * 6 are closing. The bottle list (7 to 12) is part of the open menu. */
static int menu_open(void)
{
	int s = overworld_map_state;

	return main_module_index == 14 && submodule_index == 1 &&
		s >= 2 && s != 5 && s != 6;
}

/* The cursor's item is in the menu tile map from the first drawn menu
 * through the frame Start is pressed (state 5). The next frame rebuilds
 * the HUD item box, and the 2x icon follows that again. */
static int menu_cursor_live(void)
{
	int s;

	if (main_module_index != 14 || submodule_index != 1)
		return 0;
	s = overworld_map_state;
	return s >= 2 && s != 6;
}

/* Four tile entries, top-left, top-right, bottom-left, bottom-right. */
static void item_entries(u16 out[4])
{
	const u16 *src = &hud_tile_indices_buffer[kItemBoxWord];
	int stride = 32;
	int item, slot, i;

	if (menu_cursor_live()) {
		item = hud_cur_item;
		slot = -1;
		if (item >= 1 && item <= kMenuItems) {
			if (hud_inventory_order[0]) {
				for (i = 0; i < kMenuItems; i++) {
					if (hud_inventory_order[i] == item) {
						slot = i;
						break;
					}
				}
			} else {
				slot = item - 1;
			}
		}
		if (slot >= 0)
			src = (const u16 *)(g_ram + 0x1000) + menu_word(slot);
	}
	out[0] = src[0];
	out[1] = src[1];
	out[2] = src[stride];
	out[3] = src[stride + 1];
}

/* Nonzero while the bottom screen is fading; see step_fade. */
static int fade_phase;

/* Set by the gear button; the item menu or Start clears it. */
static int gear_on;
static int menu_was_open;

static int panel_target(void)
{
	return menu_open() ? kPanelItems : gear_on ? kPanelGear : kPanelNormal;
}

/* Screen touches on the two buttons. Items is Start: it opens the game's own
 * item menu, and again it closes it. Gear shows the gear panel, and closes
 * the item menu first if that is up. Start is added to the keys for this
 * one frame, so the game sees an ordinary press. */
static int button_hit(const HudBox *b, int x, int y)
{
	return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

/* The gear panel stops the game, the way the item menu does. The menu closing
 * is the one exception: it takes game frames to finish, and the gear panel
 * can already be waiting for it. */
int ZeldaBottomHoldsGame(void)
{
	return gear_on && hud_up && !ZeldaHudOnTop() &&
		!(main_module_index == 14 && submodule_index == 1);
}

static int start_will_work(void)
{
	/* State 4 is the plain menu and 10 the bottle list; both take Start. */
	if (menu_open())
		return overworld_map_state == 4 || overworld_map_state == 10;
	return (main_module_index == 7 || main_module_index == 9) &&
		submodule_index == 0;
}

/* Touching an icon on the item panel equips it, which in this game is just
 * moving the menu's cursor there: hud_cur_item is both. The game's own menu
 * code redraws the ring from it on the next frame, and the 2x icon reads
 * that same slot, so the panel follows with no work here. The cursor move
 * gets the same sound and blink reset as a D-pad press.
 *
 * Each slot's touch area is its whole cell, icon plus half the gap on every
 * side, so the gaps between icons do not miss. Only the plain menu takes
 * touches (map state 4). The bottle list and the menu opening or closing do
 * not. */
static int item_at_touch(int x, int y)
{
	int col = (x - (kMenuX - kMenuGapX / 2)) / kMenuPitchX;
	int row = (y - (kMenuY - kMenuGapY / 2)) / kMenuPitchY;
	int slot, item;

	if (x < kMenuX - kMenuGapX / 2 || y < kMenuY - kMenuGapY / 2)
		return 0;
	if (col >= kMenuCols || row >= kMenuRows)
		return 0;
	slot = row * kMenuCols + col;
	/* The same order the game's own cursor walks: its custom order once
	 * the player has rearranged, otherwise the slot number. */
	item = hud_inventory_order[0] ? hud_inventory_order[slot] : slot + 1;
	if (item < 1 || item > kMenuItems)
		return 0;
	/* Owned, as Hud_DoWeHaveThisItem tests it. Item 16 is the bottle slot,
	 * whose count is the bottle index. */
	return (&link_item_bow)[item - 1] ? item : 0;
}

static void touch_item(int x, int y)
{
	int item;

	if (!menu_open() || overworld_map_state != 4 || panel_shown != kPanelItems ||
	    panel_level)
		return;
	item = item_at_touch(x, y);
	if (!item || item == hud_cur_item)
		return;
	hud_cur_item = item;
	timer_for_flashing_circle = 16;
	sound_effect_2 = 32;
}

/* B backs out of the gear panel or the item menu. The game has no use for B
 * in either, and a press that closed one must not also reach the game as a
 * sword swing once play resumes, so it is taken out of the keys, and stays
 * out until B is let go. */
static int b_eat;
/* Start added for the touch items button or for B closing the item menu. */
static int added_start;

static void add_start(u32 *held)
{
	if (*held & KEY_START)
		return;
	*held |= KEY_START;
	added_start = 1;
}

static void back_out(u32 *held, u32 *down)
{
	if (!(*down & KEY_B) || !hud_up || fade_phase || ZeldaHudOnTop())
		return;
	if (gear_on) {
		gear_on = 0;
	} else if (menu_open() && start_will_work()) {
		add_start(held);
	} else {
		return;
	}
	b_eat = 1;
}

int ZeldaBottomAddedStart(void)
{
	return added_start;
}

void ZeldaBottomInput(u32 *held, u32 *down)
{
	touchPosition t;

	added_start = 0;
	back_out(held, down);
	if (b_eat) {
		if (*held & KEY_B) {
			*held &= ~KEY_B;
			*down &= ~KEY_B;
		} else {
			b_eat = 0;
		}
	}

	/* Start while the game is held would do nothing. Leave the gear panel,
	 * and the game gets the press this same frame. */
	if (gear_on && (*down & KEY_START) && ZeldaBottomHoldsGame())
		gear_on = 0;
	if (!(*down & KEY_TOUCH) || !hud_up || fade_phase)
		return;
	touchRead(&t);
	if (button_hit(&kBoxes[1], t.px, t.py)) {
		gear_on = 0;
		if (start_will_work())
			add_start(held);
	} else if (button_hit(&kBoxes[0], t.px, t.py)) {
		if (menu_open()) {
			gear_on = 1;
			if (start_will_work())
				add_start(held);
		} else {
			gear_on = !gear_on;
		}
	} else {
		touch_item(t.px, t.py);
	}
}

static void draw_image_row(u8 *line, int y, const HudImage *im)
{
	int top = im->y;
	int left = im->x;
	const u8 *src;
	int x;

	if (im->box >= 0) {
		const HudBox *b = &kBoxes[im->box];

		top = b->y + (b->h - im->h) / 2;
		left = b->x + (b->w - im->w) / 2;
	}
	if (y < top || y >= top + im->h)
		return;
	src = im->pix + (y - top) * STRIDE(im->w);
	for (x = 0; x < im->w; x++) {
		if (src[x])
			line[left + x] = im->color ? im->color : im->pal_base + src[x];
	}
}

/* Everything that never changes while the HUD is up: the tiled background,
 * the boxes, and the graphics that are always there. Drawing these pixel by
 * pixel was most of the cost of a row, so they are drawn once into base and
 * every row after that starts as a copy. */
static u8 base[kHeight][kWidth] __attribute__((aligned(4)));

static void compose_base_row(u8 *line, int y)
{
	const u8 *tex = (const u8 *)tex_menuBitmap + (y % kTexH) * kTexW;
	unsigned i;
	int x;

	for (x = 0; x < kWidth; x += kTexW)
		memcpy(line + x, tex, kWidth - x < kTexW ? kWidth - x : kTexW);

	for (i = 0; i < sizeof kBoxes / sizeof kBoxes[0]; i++) {
		const HudBox *b = &kBoxes[i];
		int edge_row;

		if (y < b->y || y >= b->y + b->h)
			continue;
		edge_row = y < b->y + b->border_w ||
			y >= b->y + b->h - b->border_w;
		for (x = b->x; x < b->x + b->w; x++) {
			int edge_col = x < b->x + b->border_w ||
				x >= b->x + b->w - b->border_w;

			line[x] = (edge_row || edge_col) ? b->border : b->fill;
		}
	}

	for (i = 0; i < sizeof kImages / sizeof kImages[0]; i++)
		if (kImages[i].panel == kPanelAlways)
			draw_image_row(line, y, &kImages[i]);
}

static void build_base(void)
{
	int y;

	for (y = 0; y < kHeight; y++)
		compose_base_row(base[y], y);
}

/* One screen row: the base, then whatever changes on that row. */
static void compose_row(u8 *line, int y)
{
	unsigned i;
	int x;

	memcpy(line, base[y], kWidth);

	/* The big panel's title text and items. */
	for (i = 0; i < sizeof kImages / sizeof kImages[0]; i++)
		if (kImages[i].panel == panel_shown)
			draw_image_row(line, y, &kImages[i]);
	if (panel_shown == kPanelItems)
		compose_menu(line, y);
	else if (panel_shown == kPanelGear)
		compose_gear(line, y);
	else if (y >= kMapY && y < kMapY + kMapH)
		ZeldaMapRow(line + kMapX, y);

	if (y >= kMeterY && y < kMeterY + kMeterThick) {
		const u8 *row = shown_magic.px[y - kMeterY];

		for (x = 0; x < kMeterLen; x++) {
			if (row[x])
				line[kMeterX + x] = row[x];
		}
	}
	draw_half_row(line, y);

	if (y >= kHeartY && y < kHeartY + kHeartH) {
		const u8 *row = shown_hearts.px[y - kHeartY];

		for (x = 0; x < kHeartW; x++) {
			u8 p = row[x];
			u8 *dst = &line[kHeartX + x];

			if (p == kShadowMark) {
				/* The hearts sit on the two-color background. */
				if (*dst < 2)
					*dst += kPalShadow;
			} else if (p) {
				*dst = p;
			}
		}
	}

	if (y >= kItemY && y < kItemY + kItemSize) {
		const u8 *row = shown_item.px[y - kItemY];

		for (x = 0; x < kItemSize; x++) {
			if (row[x])
				line[kItemX + x] = row[x];
		}
	}

	for (i = 0; i < kSprites; i++) {
		const HudSprite *s = &shown.spr[i];

		if (y < s->y || y >= s->y + s->h)
			continue;
		for (x = 0; x < s->w; x++) {
			u8 p = s->px[y - s->y][x];

			if (p)
				line[s->x + x] = p;
		}
	}
}

/* VRAM takes 16-bit writes, so rows are built in RAM and DMA'd. */
static void put_rows(int from, int to)
{
	static u8 line[kWidth] __attribute__((aligned(4)));
	u8 *dst = (u8 *)ZeldaVideoBottomFramebuffer();
	int y;

	for (y = from; y < to; y++) {
		compose_row(line, y);
		/* DMA reads RAM, not the cache. */
		DC_FlushRange(line, kWidth);
		dmaCopy(line, dst + y * kWidth, kWidth);
	}
}

/* The map alone, which moves with Link and is most of the panel. It touches
 * only its own pixels, between the green borders, so the rest of each row is
 * left as it is. */
static void put_map_rows(int from, int to)
{
	u8 *dst = (u8 *)ZeldaVideoBottomFramebuffer();
	int y;

	for (y = from; y < to; y++)
		ZeldaMapPutRow(dst, kWidth, y, kPalGreen);
}

/* Checksum of what the snapshots were last built from; see dyn_sig. */
static u32 last_sig;

static void draw_hud(void)
{
	int i;

	build_counters(&shown);
	{
		u16 e[4];

		item_entries(e);
		build_item(&shown_item, e);
	}
	build_hearts(&shown_hearts);
	build_magic(&shown_magic);
	dmaCopy(tex_menuPal, BG_PALETTE_SUB, tex_menuPalLen);
	for (i = 1; i < 4; i++) {
		BG_PALETTE_SUB[kPalBorder + i] = selected_item_borderPal[i];
		BG_PALETTE_SUB[kPalLife + i] = life_textPal[i];
	}
	/* The hearts' shadow is black at 80% opacity, so 20% of the
	 * background color is left. */
	for (i = 0; i < 2; i++) {
		u16 c = tex_menuPal[i];

		BG_PALETTE_SUB[kPalShadow + i] = RGB15(((c >> 0) & 31) * 2 / 10,
						       ((c >> 5) & 31) * 2 / 10,
						       ((c >> 10) & 31) * 2 / 10);
	}
	/* #50a865 at 5 bits per channel. */
	BG_PALETTE_SUB[kPalGreen] = RGB15(0x50 >> 3, 0xa8 >> 3, 0x65 >> 3);
	BG_PALETTE_SUB[kPalText] = RGB15(31, 31, 31);
	BG_PALETTE_SUB[kPalBlack] = RGB15(0, 0, 0);
	/* #e3b23d at 5 bits per channel. */
	BG_PALETTE_SUB[kPalGold] = RGB15(0xe3 >> 3, 0xb2 >> 3, 0x3d >> 3);
	set_counter_palette(&shown);
	set_item_palette(&shown_item);
	set_heart_palette(&shown_hearts);
	set_magic_palette(&shown_magic);
	shown_half = half_magic();
	/* The panel comes up already showing what the game is in. */
	panel_shown = panel_target();
	panel_level = 0;
	panel_redraw = -1;
	menu_was_open = menu_open();
	build_menu(&shown_menu);
	build_gear(&shown_gear);
	if (panel_shown == kPanelNormal) {
		int lo, hi;

		ZeldaMapInvalidate();
		ZeldaMapUpdate(&lo, &hi);
	}
	set_panel_palette();
	build_base();
	last_sig = 0;
	put_rows(0, kHeight);
}

/* Counts tick up a point per frame while they fill, the player swaps items,
 * and the game can reload its tiles, so look every frame and touch the
 * screen only on a change, and only the rows that changed. */
static void commit_counters(void)
{
	int lo = kDynBottom, hi = kDynTop, i;

	if (!memcmp(&next, &shown, sizeof shown))
		return;
	/* Only the rows of what changed. */
	if (memcmp(next.pal, shown.pal, sizeof next.pal)) {
		lo = kDynTop;
		hi = kDynBottom;
	} else {
		for (i = 0; i < kSprites; i++) {
			if (!memcmp(&next.spr[i], &shown.spr[i], sizeof next.spr[i]))
				continue;
			if (next.spr[i].y < lo)
				lo = next.spr[i].y;
			if (next.spr[i].y + next.spr[i].h > hi)
				hi = next.spr[i].y + next.spr[i].h;
		}
	}
	memcpy(&shown, &next, sizeof shown);
	set_counter_palette(&shown);
	put_rows(lo, hi);
}

/* The three counts alone. While rupees pour in they change every frame, and
 * the icons and colors do not, so only the numbers are redrawn. */
static void refresh_counts(void)
{
	const Ppu *ppu = g_zenv.ppu;
	int rupees = link_rupees_actual;

	memcpy(&next, &shown, sizeof next);
	build_number(&next.spr[kRupeeNum], ppu, rupees, rupees > 999 ? 4 : 3, kRupeeY);
	build_number(&next.spr[kBombNum], ppu, link_item_bombs % 100, 2, kBombY);
	build_number(&next.spr[kArrowNum], ppu, link_num_arrows % 100, 2, kArrowY);
	commit_counters();
}

static void refresh_counters(void)
{
	build_counters(&next);
	commit_counters();
}

static void refresh_item(void)
{
	u16 e[4];

	item_entries(e);
	build_item(&next_item, e);
	if (memcmp(&next_item, &shown_item, sizeof shown_item)) {
		memcpy(&shown_item, &next_item, sizeof shown_item);
		set_item_palette(&shown_item);
		put_rows(kItemY, kItemY + kItemSize);
	}
}

static void refresh_hearts(void)
{
	build_hearts(&next_hearts);
	if (memcmp(&next_hearts, &shown_hearts, sizeof shown_hearts)) {
		memcpy(&shown_hearts, &next_hearts, sizeof shown_hearts);
		set_heart_palette(&shown_hearts);
		put_rows(kHeartY, kHeartY + kHeartH);
	}
}

static void refresh_magic(void)
{
	int half = half_magic();

	build_magic(&next_magic);
	if (!memcmp(&next_magic, &shown_magic, sizeof shown_magic) &&
	    half == shown_half)
		return;
	memcpy(&shown_magic, &next_magic, sizeof shown_magic);
	shown_half = half;
	set_magic_palette(&shown_magic);
	/* The label sits a few rows above the meter and overlaps its top. */
	put_rows(kHalfY, kMeterY + kMeterThick);
}

/* Building the four snapshots decodes a few thousand pixels, and doing all
 * of it every frame is enough to push a frame past its time, which slows the
 * game and the music with it. What they are built from is small: the HUD's
 * tile entries, the BG3 palettes, and four counts. A checksum of those says
 * whether anything changed, and only then is everything rebuilt. The tile
 * graphics themselves can be reloaded without those changing, so otherwise
 * one snapshot is checked each frame, round robin. */
static u32 dyn_sig(void)
{
	const Ppu *ppu = g_zenv.ppu;
	const u16 *buf = hud_tile_indices_buffer;
	u16 item[4];
	u32 h = 2166136261u;
	int i, r;

	/* The 2x item and the hearts. Not the meter or the counters: those
	 * change every frame as they fill, and have their own checks. The
	 * item entries are the menu slot under the cursor while the menu is
	 * up, so moving the cursor redraws the 2x icon this frame. */
	item_entries(item);
	for (i = 0; i < 4; i++)
		h = (h ^ item[i]) * 16777619u;
	for (r = 0; r < kHeartRows; r++)
		for (i = 0; i < kHeartCols; i++)
			h = (h ^ buf[kHeartWord + r * 32 + i]) * 16777619u;
	for (i = 0; i < 32; i++)
		h = (h ^ ppu->cgram[i]) * 16777619u;
	return h;
}

/* The meter's tile entries and the magic itself. */
static u32 magic_sig(void)
{
	const u16 *buf = hud_tile_indices_buffer;
	u32 h = (u32)link_magic_power + 2166136261u;
	int r, i;

	h = (h ^ link_magic_consumption) * 16777619u;

	for (r = 0; r < 6; r++)
		for (i = 0; i < 3; i++)
			h = (h ^ buf[r * 32 + kMagicWord + i]) * 16777619u;
	return h;
}

static u32 counts_sig(void)
{
	u32 h = (u32)link_rupees_actual + 2166136261u;

	h = (h ^ (u32)link_item_bombs) * 16777619u;
	return (h ^ (u32)link_num_arrows) * 16777619u;
}

static u32 last_magic_sig, last_counts_sig;
static int check_next;

static void refresh_dynamic(void)
{
	u32 sig = dyn_sig();
	u32 msig = magic_sig();
	u32 csig = counts_sig();

	if (sig != last_sig) {
		last_sig = sig;
		last_magic_sig = msig;
		last_counts_sig = csig;
		refresh_counters();
		refresh_item();
		refresh_hearts();
		refresh_magic();
		return;
	}
	/* Rupees pouring in or the meter filling change one thing a frame, so
	 * redraw just that. */
	if (csig != last_counts_sig) {
		last_counts_sig = csig;
		refresh_counts();
	}
	if (msig != last_magic_sig) {
		last_magic_sig = msig;
		refresh_magic();
	}
	/* The safety net, every other frame: rebuilding a snapshot is the most
	 * expensive thing this screen does on a frame where nothing changed. */
	if (++check_next & 1)
		return;
	switch ((check_next >> 1) & 3) {
	case 0: refresh_counters(); break;
	case 1: refresh_item(); break;
	case 2: refresh_hearts(); break;
	default: refresh_magic(); break;
	}
}

/* The big panel changes between normal, items and gear through black: its
 * colors dim to nothing, then the new contents are drawn a slice of rows a
 * frame (the colors are black, so the work cannot be seen), then they come
 * back. While items is up, the cursor blink and any change in the menu
 * redraw only the rows they touch. */
static void update_panel(void)
{
	int target;

	if (menu_open() && !menu_was_open)
		gear_on = 0;
	menu_was_open = menu_open();
	target = panel_target();

	if (panel_redraw >= 0) {
		int to = panel_redraw + kPanelRowsPerFrame;

		if (to > kPanelBottom)
			to = kPanelBottom;
		put_rows(panel_redraw, to);
		panel_redraw = to >= kPanelBottom ? -1 : to;
		return;
	}

	if (target != panel_shown) {
		panel_level += kPanelStep;
		if (panel_level >= kPanelBlack) {
			panel_level = kPanelBlack;
			panel_shown = target;
			build_menu(&shown_menu);
			build_gear(&shown_gear);
			if (panel_shown == kPanelNormal) {
				int lo, hi;

				ZeldaMapInvalidate();
				ZeldaMapUpdate(&lo, &hi);
			}
			panel_redraw = kPanelTop;
		}
		set_panel_palette();
		return;
	}

	if (panel_level) {
		panel_level -= kPanelStep;
		if (panel_level < 0)
			panel_level = 0;
		set_panel_palette();
	}
	if (panel_shown == kPanelItems) {
		int lo, hi;

		build_menu(&next_menu);
		menu_changed_rows(&shown_menu, &next_menu, &lo, &hi);
		if (hi > lo) {
			memcpy(&shown_menu, &next_menu, sizeof shown_menu);
			set_panel_palette();
			put_rows(lo, hi);
		}
	} else if (panel_shown == kPanelGear) {
		int lo, hi;

		build_gear(&next_gear);
		gear_changed_rows(&shown_gear, &next_gear, &lo, &hi);
		if (hi > lo) {
			memcpy(&shown_gear, &next_gear, sizeof shown_gear);
			set_panel_palette();
			put_rows(lo, hi);
		}
	} else {
		int lo, hi;

		/* Link moving, the map scrolling, the marker blinking. */
		if (ZeldaMapUpdate(&lo, &hi)) {
			if (ZeldaMapTakePalette())
				set_panel_palette();
			put_map_rows(lo, hi);
		}
	}
}

/* Opening or closing a map, and the game over screen, swap the bottom
 * screen between the HUD and the idle picture through black: fade out what
 * is up, swap, fade the other in. It runs on the sub engine's master
 * brightness, 0 clear to 16 black, two steps a frame like the top screen's
 * fades. */
enum {
	kFadeNone,
	kFadeOut,
	kFadeIn,
	kFadeStep = 2,
	kFadeBlack = 16
};

static int fade_level;
static int map_was_open;
static int over_was_up;
static int bird_was;
static int aga_was;

static void set_bottom_fade(int level)
{
	REG_MASTER_BRIGHT_SUB = level ? (u16)((1u << 15) | level) : 0;
}

/* Put up whichever picture the game wants now. */
static void swap_picture(int want)
{
	if (want) {
		draw_hud();
		hud_up = 1;
	} else {
		ZeldaVideoShowBottom();
	}
}

static void step_fade(int want)
{
	if (fade_phase == kFadeOut) {
		fade_level += kFadeStep;
		if (fade_level < kFadeBlack) {
			set_bottom_fade(fade_level);
			return;
		}
		fade_level = kFadeBlack;
		set_bottom_fade(fade_level);
		/* Black. Nothing shows the swap. */
		swap_picture(want);
		fade_phase = kFadeIn;
		return;
	}
	/* What should be showing changed partway in. Go back down. */
	if (want != hud_up) {
		fade_phase = kFadeOut;
		return;
	}
	if (hud_up)
		refresh_dynamic();
	fade_level -= kFadeStep;
	if (fade_level <= 0) {
		fade_level = 0;
		fade_phase = kFadeNone;
	}
	set_bottom_fade(fade_level);
}

void ZeldaHwSetDim(int level);

/* While the item menu is up on the bottom screen, the frozen game on the
 * top screen is dimmed to half, easing in and out a step a frame. */
enum {
	kTopDimMax = 8,
	kTopDimStep = 1
};

static int top_dim;

static void update_top_dim(void)
{
	/* gear_on carries the dim through the item menu closing into the gear
	 * panel, so it does not flicker between the two. */
	int target = (hud_up && !ZeldaHudOnTop() && (menu_open() || gear_on)) ?
		kTopDimMax : 0;

	if (top_dim == target)
		return;
	if (top_dim < target) {
		top_dim += kTopDimStep;
		if (top_dim > target)
			top_dim = target;
	} else {
		top_dim -= kTopDimStep;
		if (top_dim < target)
			top_dim = target;
	}
	ZeldaHwSetDim(top_dim);
}

static void finish_rain(void)
{
	int dim = -1;

	if (hud_up && panel_shown == kPanelNormal)
		dim = panel_level;
	ZeldaMapRainCommit(dim);
}

void ZeldaBottomPrepare(void)
{
	ZeldaMapRainPrepare(hud_up && panel_shown == kPanelNormal);
}

void ZeldaBottomUpdate(void)
{
	int want = hud_wanted();
	int map = map_open();
	int over = game_over_screen();
	int bird = bird_cutscene();
	int aga = aga_warp();

	update_top_dim();

	if (fade_phase) {
		map_was_open = map;
		over_was_up = over;
		bird_was = bird;
		aga_was = aga;
		step_fade(want);
		finish_rain();
		return;
	}
	/* The map, the game over screen, the flute-bird cutscene, and
	 * Agahnim's warp into the dark world, with the HUD on the bottom
	 * setting: fade rather than cut. Those cutscenes fade the idle
	 * picture in, and fading back to the HUD is the return to play
	 * once that module ends. The first frame of a fade is the first
	 * step of fading out. Continue leaves the game over screen and
	 * comes back through the load below, which fades the HUD in. */
	if (map != map_was_open || over != over_was_up || bird != bird_was ||
	    aga != aga_was) {
		map_was_open = map;
		over_was_up = over;
		bird_was = bird;
		aga_was = aga;
		if (!ZeldaHudOnTop() && want != hud_up &&
		    (in_game() || bird || aga || map || over)) {
			fade_phase = kFadeOut;
			fade_level = 0;
			step_fade(want);
			finish_rain();
			return;
		}
	}
	if (want && hud_up) {
		refresh_dynamic();
		update_panel();
		finish_rain();
		return;
	}
	if (!hud_up)
		gear_on = 0;
	if (want == hud_up) {
		finish_rain();
		return;
	}
	/* Loading a game, including after Continue on the game over screen:
	 * the idle picture fades out and the HUD fades in, the same swap the
	 * maps use. Leaving the game stays a cut. */
	if (want) {
		fade_phase = kFadeOut;
		fade_level = 0;
		step_fade(want);
		finish_rain();
		return;
	}
	swap_picture(want);
	finish_rain();
}
