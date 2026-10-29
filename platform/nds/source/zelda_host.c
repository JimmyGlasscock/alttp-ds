#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "types.h"
#include "config.h"
#include "assets.h"
#include "util.h"
#include "audio.h"
#include "zelda_rtl.h"
#include "snes/ppu.h"
#include "zelda_video.h"
#include "zelda_bottom.h"
#include "zelda_save.h"
#include "zelda_audio.h"
#include "messaging.h"
#include "variables.h"

const uint8 *g_asset_ptrs[kNumberOfAssets];
uint32 g_asset_sizes[kNumberOfAssets];

void NORETURN Die(const char *error)
{
	consoleDemoInit();
	iprintf("\n%s\n", error ? error : "error");
	for (;;)
		swiWaitForVBlank();
}

void ZeldaApuLock(void)
{
}

void ZeldaApuUnlock(void)
{
}

/* The build links zelda3_assets.dat into this image. The bytes are in RAM
 * when the ARM9 binary is loaded, so a menu that cannot reopen the .nds
 * still has the game data. */
extern const uint8 zelda3_assets_dat[];
extern const uint8 zelda3_assets_dat_end[];

/* Ambient sound 1 is the outdoor rain. The Zora waterfall plays that
 * same sound. Ambient sound 3 is rain indoors. Each is a hard-panned
 * pair: one voice is the left side and the other is the right. The
 * levels are linear, 0x50 outdoors and 0x32 indoors. Eight decibels
 * down is 10^(-8/20), which rounds to 0x20 and 0x14. */
static void quiet_rain(void)
{
	static const struct {
		uint16 addr;
		uint8 from, to;
	} kVol[] = {
		{ 0x2655, 0x50, 0x20 },
		{ 0x2666, 0x50, 0x20 },
		{ 0x267a, 0x32, 0x14 },
		{ 0x268b, 0x32, 0x14 },
	};
	uint8 *bank = (uint8 *)kSoundBank_intro;
	uint32 size = kSoundBank_intro_SIZE;
	uint32 i = 0;
	int n;

	if (!bank)
		return;
	while (i + 4 <= size) {
		uint16 num = bank[i] | ((uint16)bank[i + 1] << 8);
		uint16 target;
		uint32 base;

		if (num == 0)
			break;
		target = bank[i + 2] | ((uint16)bank[i + 3] << 8);
		i += 4;
		if (i + num > size)
			break;
		base = i;
		for (n = 0; n < (int)(sizeof kVol / sizeof kVol[0]); n++) {
			uint16 addr = kVol[n].addr;
			uint32 off;

			if (addr < target || (uint32)(addr - target) >= num)
				continue;
			off = base + (uint32)(addr - target);
			if (bank[off] == kVol[n].from)
				bank[off] = kVol[n].to;
		}
		i += num;
	}
}

void ZeldaLoadAssets(void)
{
	uint8 *data = (uint8 *)zelda3_assets_dat;
	size_t length = (size_t)(zelda3_assets_dat_end - zelda3_assets_dat);

	if (length < 16)
		Die("Failed to read zelda3_assets.dat");

	{
		static const char kAssetsSig[] = { kAssets_Sig };

		if (length < 16 + 32 + 32 + 8 + kNumberOfAssets * 4 ||
		    memcmp(data, kAssetsSig, 48) != 0 ||
		    *(uint32 *)(data + 80) != kNumberOfAssets)
			Die("Invalid assets file");
	}

	{
		uint32 offset = 88 + kNumberOfAssets * 4 + *(uint32 *)(data + 84);
		size_t ai;

		for (ai = 0; ai < kNumberOfAssets; ai++) {
			uint32 size = *(uint32 *)(data + 88 + ai * 4);
			offset = (offset + 3) & ~3;
			if ((uint64)offset + size > length)
				Die("Assets file corruption");
			g_asset_sizes[ai] = size;
			g_asset_ptrs[ai] = data + offset;
			offset += size;
		}
	}
	quiet_rain();
}

MemBlk FindInAssetArray(int asset, int idx)
{
	return FindIndexInMemblk((MemBlk) { g_asset_ptrs[asset], g_asset_sizes[asset] }, idx);
}

/* ARM9 LDRH/LDR ignore low address bits. Packed-asset counts sit at size-2,
 * which is odd for kSprGfx, so the stock finder returns the wrong 1536 bytes. */
static uint16 read_u16(const uint8 *p)
{
	return (uint16)p[0] | ((uint16)p[1] << 8);
}

static uint32 read_u32(const uint8 *p)
{
	return (uint32)p[0] | ((uint32)p[1] << 8) |
		((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

MemBlk __wrap_FindIndexInMemblk(MemBlk data, size_t i)
{
	size_t end, mx, left_off, right_off;

	if (!data.ptr || data.size < 2)
		return (MemBlk) { 0, 0 };
	end = data.size - 2;
	mx = read_u16(data.ptr + end);
	if (mx < 8192) {
		if (i > mx || mx * 2 > end)
			return (MemBlk) { 0, 0 };
		left_off = (i == 0) ? mx * 2 : mx * 2 + read_u16(data.ptr + i * 2 - 2);
		right_off = (i == mx) ? end : mx * 2 + read_u16(data.ptr + i * 2);
	} else {
		mx -= 8192;
		if (i > mx || mx * 4 > end)
			return (MemBlk) { 0, 0 };
		left_off = (i == 0) ? mx * 4 : mx * 4 + read_u32(data.ptr + i * 4 - 4);
		right_off = (i == mx) ? end : mx * 4 + read_u32(data.ptr + i * 4);
	}
	if (left_off > right_off || right_off > end)
		return (MemBlk) { 0, 0 };
	return (MemBlk) { data.ptr + left_off, right_off - left_off };
}

/* kKeys "AXsSUDLRBY" before the 16-bit reverse in NMI_ReadJoypads.
 * bit 0 SNES B sword, 1 SNES Y item, 2 Select, 3 Start,
 * 4-7 Up Down Left Right, 8 SNES A lift/chest/dash, 9 SNES X, 10 SNES L, 11 SNES R.
 * The button map picks the DS key for each bit. Until it is saved, the
 * names match: DS B is the sword, DS A lifts, DS Y uses the item.
 */
static int paused;
static int settings_open;
static int settings_menu;
static int lid_was_down;
static volatile int lid_closed;
static volatile int lid_wake;
/* Title, file select, game over, credits: hold the frame until the lid opens. */
static int lid_hold_still;
static int lid_frozen;
static PmEventCookie lid_cookie;
/* Two frames after the game is back: one samples the closing press, one eats it. */
static int settings_eat;

enum {
	kUiOff = 0,
	kUiGameOut,
	kUiMenuIn,
	kUiMenu,
	kUiMenuOut,
	kUiGameIn
};

enum {
	kPageSettings = 0,
	kPageButtons
};

static int ui;
static int fade;
static int cursor;
static int hud_pick;
static int beep_on;
static int bomb_99;
static int arrow_99;
static int run_speed;
static int dark_bright;
static int hair_blonde;
static int spawn_dungeon;
static int mirror_free;
static int scroll_row;
static int save_on_exit;
static int menu_page;
static int map_draft[kSnesButtonCount];

enum {
	kRowHud = 0,
	kRowBeep,
	kRowSpawn,
	kRowMirror,
	kRowBomb,
	kRowArrow,
	kRowSpeed,
	kRowDark,
	kRowHair,
	kRowMap,
	kRowSave,
	kRowCancel,
	kSettingCount = kRowSave,
	kVisible = 5,
	kCaretX = 16,
	kLabelX = 36,
	kValueX = 120,
	kTextX = 36,
	kTitleY = 22,
	kSaveY = 146,
	kCancelY = 166
};

static const int kSlotY[kVisible] = { 42, 60, 78, 96, 114 };
static const char *const kHudChoice[] = { "Top Screen", "Bottom Screen" };
static const char *const kOnOff[] = { "Off", "On" };
static const char *const kRunChoice[] = { "Normal", "Double", "Triple" };

static int settings_value_x(void)
{
	int label = ZeldaVideoMenuWidth("Low HP Indicator");
	int mirror = ZeldaVideoMenuWidth("Magic Mirror");
	int value = ZeldaVideoMenuWidth("Bottom Screen");
	int x;

	if (mirror > label)
		label = mirror;
	x = kLabelX + label + 12;

	if (x < kValueX)
		x = kValueX;
	if (x + value > 248)
		x = 248 - value;
	return x;
}

static const char *settings_label(int row)
{
	switch (row) {
	case kRowHud:
		return "HUD";
	case kRowBeep:
		return "Low HP Indicator";
	case kRowSpawn:
		return "Spawn";
	case kRowMirror:
		return "Magic Mirror";
	case kRowBomb:
		return "Bomb Capacity";
	case kRowArrow:
		return "Arrow Capacity";
	case kRowSpeed:
		return "Run Speed";
	case kRowDark:
		return "Dark Rooms";
	case kRowHair:
		return "Hair Color";
	case kRowMap:
		return "Button Mapping";
	default:
		return "";
	}
}

static const char *settings_value(int row)
{
	switch (row) {
	case kRowHud:
		return kHudChoice[hud_pick];
	case kRowBeep:
		return kOnOff[beep_on];
	case kRowSpawn:
		return spawn_dungeon ? "Dungeon" : "Normal";
	case kRowMirror:
		return mirror_free ? "Free" : "Normal";
	case kRowBomb:
		return bomb_99 ? "99" : "10";
	case kRowArrow:
		return arrow_99 ? "99" : "30";
	case kRowSpeed:
		return kRunChoice[run_speed];
	case kRowDark:
		return dark_bright ? "Bright" : "Normal";
	case kRowHair:
		return hair_blonde ? "Blonde" : "Pink";
	case kRowMap:
		return "";
	default:
		return "";
	}
}

static void settings_follow(void)
{
	if (cursor >= kSettingCount)
		return;
	if (cursor < scroll_row)
		scroll_row = cursor;
	if (cursor >= scroll_row + kVisible)
		scroll_row = cursor - kVisible + 1;
}

static int settings_caret_y(void)
{
	if (cursor == kRowSave)
		return kSaveY;
	if (cursor == kRowCancel)
		return kCancelY;
	return kSlotY[cursor - scroll_row];
}

static void settings_draw(void)
{
	const char *title = "Settings";
	int value_x = settings_value_x();
	int i;

	ZeldaVideoMenuClear();
	ZeldaVideoMenuText((256 - ZeldaVideoMenuWidth(title)) / 2, kTitleY, title);
	for (i = 0; i < kVisible; i++) {
		int row = scroll_row + i;

		if (row >= kSettingCount)
			break;
		ZeldaVideoMenuText(kLabelX, kSlotY[i], settings_label(row));
		ZeldaVideoMenuText(value_x, kSlotY[i], settings_value(row));
	}
	ZeldaVideoMenuText(kTextX, kSaveY, "Save");
	ZeldaVideoMenuText(kTextX, kCancelY, "Cancel");
	ZeldaVideoMenuText(kCaretX, settings_caret_y(), ">");
}

static void settings_move(int dir)
{
	if (dir < 0) {
		if (cursor > kRowHud)
			cursor--;
	} else if (cursor < kRowCancel) {
		cursor++;
	}
	settings_follow();
	if (settings_menu)
		settings_draw();
}

static void settings_scroll(int dir)
{
	int n = 2;
	int *v = NULL;

	if (cursor == kRowHud)
		v = &hud_pick;
	else if (cursor == kRowBeep)
		v = &beep_on;
	else if (cursor == kRowSpawn)
		v = &spawn_dungeon;
	else if (cursor == kRowMirror)
		v = &mirror_free;
	else if (cursor == kRowBomb)
		v = &bomb_99;
	else if (cursor == kRowArrow)
		v = &arrow_99;
	else if (cursor == kRowSpeed) {
		v = &run_speed;
		n = 3;
	} else if (cursor == kRowDark) {
		v = &dark_bright;
	} else if (cursor == kRowHair) {
		v = &hair_blonde;
	} else {
		return;
	}
	if (dir < 0)
		*v = (*v + n - 1) % n;
	else
		*v = (*v + 1) % n;
	if (settings_menu)
		settings_draw();
}

static void settings_close(int save)
{
	if (ui != kUiMenu)
		return;
	save_on_exit = save;
	ui = kUiMenuOut;
}

static void settings_apply(void)
{
	if (!save_on_exit)
		return;
	ZeldaHudSetOnTop(hud_pick == 0);
	ZeldaLowHpBeepSet(beep_on);
	ZeldaBombCapSet(bomb_99 ? 99 : 10);
	ZeldaArrowCapSet(arrow_99 ? 99 : 30);
	ZeldaRunSpeedSet(run_speed + 1);
	ZeldaDarkRoomsSet(dark_bright);
	ZeldaHairSet(hair_blonde);
	ZeldaSpawnSet(spawn_dungeon);
	ZeldaMirrorSet(mirror_free);
	save_on_exit = 0;
}

/* The select menu is still the game underneath. Drop the overlay on it. */
static void settings_dismiss(void)
{
	settings_apply();
	if (settings_menu) {
		ZeldaVideoMenuEnd();
		settings_menu = 0;
	}
	ZeldaVideoSetFade(-1);
	settings_open = 0;
	settings_eat = 2;
	ui = kUiOff;
	fade = 0;
	menu_page = kPageSettings;
}

/* Display order. kMapBit is the joypad bit each row edits. */
enum { kMapSave = kSnesButtonCount, kMapCancel };

static const int kMapBit[kSnesButtonCount] = {
	kSnesA, kSnesB, kSnesX, kSnesY,
	kSnesL, kSnesR, kSnesStart, kSnesSelect,
	kSnesUp, kSnesDown, kSnesLeft, kSnesRight
};

static const char *const kMapLabel[kSnesButtonCount] = {
	"A Button", "B Button", "X Button", "Y Button",
	"L Button", "R Button", "Start", "Select",
	"Up", "Down", "Left", "Right"
};

static const char *const kDsName[kDsButtonCount] = {
	"DPAD Up", "DPAD Down", "DPAD Left", "DPAD Right",
	"A", "B", "X", "Y", "L", "R", "Start", "Select"
};

static int buttons_value_x(void)
{
	int label = 0;
	int value = 0;
	int i, w, x;

	for (i = 0; i < kSnesButtonCount; i++) {
		w = ZeldaVideoMenuWidth(kMapLabel[i]);
		if (w > label)
			label = w;
	}
	for (i = 0; i < kDsButtonCount; i++) {
		w = ZeldaVideoMenuWidth(kDsName[i]);
		if (w > value)
			value = w;
	}
	x = kLabelX + label + 12;
	if (x < kValueX)
		x = kValueX;
	if (x + value > 248)
		x = 248 - value;
	return x;
}

static void buttons_follow(void)
{
	if (cursor >= kSnesButtonCount)
		return;
	if (cursor < scroll_row)
		scroll_row = cursor;
	if (cursor >= scroll_row + kVisible)
		scroll_row = cursor - kVisible + 1;
}

static int buttons_caret_y(void)
{
	if (cursor == kMapSave)
		return kSaveY;
	if (cursor == kMapCancel)
		return kCancelY;
	return kSlotY[cursor - scroll_row];
}

static void buttons_draw(void)
{
	const char *title = "Button Mapping";
	int value_x = buttons_value_x();
	int i;

	ZeldaVideoMenuClear();
	ZeldaVideoMenuText((256 - ZeldaVideoMenuWidth(title)) / 2, kTitleY, title);
	for (i = 0; i < kVisible; i++) {
		int row = scroll_row + i;
		int ds;

		if (row >= kSnesButtonCount)
			break;
		ds = map_draft[kMapBit[row]];
		if ((unsigned)ds >= kDsButtonCount)
			ds = kDsA;
		ZeldaVideoMenuText(kLabelX, kSlotY[i], kMapLabel[row]);
		ZeldaVideoMenuText(value_x, kSlotY[i], kDsName[ds]);
	}
	ZeldaVideoMenuText(kTextX, kSaveY, "Save");
	ZeldaVideoMenuText(kTextX, kCancelY, "Cancel");
	ZeldaVideoMenuText(kCaretX, buttons_caret_y(), ">");
}

static void buttons_move(int dir)
{
	if (dir < 0) {
		if (cursor > 0)
			cursor--;
	} else if (cursor < kMapCancel) {
		cursor++;
	}
	buttons_follow();
	if (settings_menu)
		buttons_draw();
}

static void buttons_scroll(int dir)
{
	int n = kDsButtonCount;
	int *v;

	if (cursor >= kMapSave)
		return;
	v = &map_draft[kMapBit[cursor]];
	if (dir < 0)
		*v = (*v + n - 1) % n;
	else
		*v = (*v + 1) % n;
	if (settings_menu)
		buttons_draw();
}

/* Save copies the draft into the live map. Cancel leaves that map alone. */
static void buttons_leave(int save)
{
	if (save)
		ZeldaButtonMapSet(map_draft);
	menu_page = kPageSettings;
	cursor = kRowMap;
	settings_follow();
	if (settings_menu)
		settings_draw();
}

static void buttons_enter(void)
{
	ZeldaButtonMapGet(map_draft);
	menu_page = kPageButtons;
	cursor = 0;
	scroll_row = 0;
	buttons_draw();
}

static void buttons_keys(u32 down)
{
	if (down & KEY_UP)
		buttons_move(-1);
	else if (down & KEY_DOWN)
		buttons_move(1);
	else if (down & KEY_LEFT)
		buttons_scroll(-1);
	else if (down & KEY_RIGHT)
		buttons_scroll(1);
	else if (down & KEY_B)
		buttons_leave(0);
	else if ((down & (KEY_A | KEY_START)) && cursor >= kMapSave)
		buttons_leave(cursor == kMapSave);
}

static void on_lid_sleep(void *user, PmEvent event)
{
	(void)user;
	if (event == PmEvent_OnSleep)
		lid_closed = 1;
	else if (event == PmEvent_OnWakeup)
		lid_wake = 1;
}

static void note_lid(u32 held)
{
	int slept = lid_closed;
	int down = (held & KEY_LID) != 0;
	int edge;

	lid_closed = 0;
	edge = slept || (down && !lid_was_down);
	lid_was_down = down;
	if (!edge)
		return;
	if (settings_open)
		settings_dismiss();
	if (SelectMenu_LidClosed()) {
		paused = 0;
		lid_hold_still = 0;
	} else {
		lid_hold_still = 1;
	}
}

/* Screen changes that load a new picture: title to file select, and file
 * select to spawn select. The game changes module in one frame and the
 * first picture of the new screen can be half built, so the old picture
 * is faded out first, with the game held, and the new one is faded in from
 * black. The host watches the module number after each frame.
 *
 *   Out   Game stopped. The last picture shown is still on the screen and
 *         fades to black. The frame that changed module is not presented.
 *   Hold  Black. The game runs (loading the chosen file) until the spawn
 *         select shows up.
 *   In    The new picture fades in under whatever brightness the game sets.
 */
enum {
	kModTitle = 20,
	kModIntro = 0,
	kModFileSelect = 1,
	kModNameFile = 4,
	kModLoadFile = 5,
	kModSpawnSelect = 27,
	kTxOutStep = 2,
	kTxInStep = 1,
	/* Frames to wait for a force blank to end before giving up. */
	kTxBlankWait = 120
};

enum {
	kTxNone = 0,
	kTxOut,
	kTxHold,
	kTxIn
};

static int tx_phase;
static int tx_level;
static int tx_prev = -1;
static int tx_after;
static int tx_wait;

static void tx_reset(void)
{
	tx_phase = kTxNone;
	tx_level = 0;
	tx_wait = 0;
	tx_prev = -1;
	ZeldaVideoHoldBrightness(0);
	ZeldaVideoSetTransition(0);
}

static void tx_finish_out(void)
{
	tx_level = 16;
	tx_wait = 0;
	ZeldaVideoSetTransition(16);
	ZeldaVideoHoldBrightness(0);
	tx_phase = tx_after;
}

static void tx_begin_out(int after)
{
	tx_after = after;
	if (ZeldaVideoShownBlack()) {
		tx_finish_out();
		return;
	}
	tx_phase = kTxOut;
	tx_level = 0;
	ZeldaVideoSetTransition(0);
	ZeldaVideoHoldBrightness(1);
}

/* Modules 1..4: file select, copy file, erase file, name file. */
static int tx_in_file_menus(int mod)
{
	return mod >= kModFileSelect && mod <= kModNameFile;
}

/* After each game frame, before it is presented. */
static void tx_watch(void)
{
	int mod = main_module_index;
	int prev = tx_prev;

	tx_prev = mod;
	if (mod == prev || prev < 0)
		return;
	if (mod == kModFileSelect && (prev == kModTitle || prev == kModIntro)) {
		tx_begin_out(kTxIn);
	} else if (tx_in_file_menus(mod) && tx_in_file_menus(prev)) {
		/* File select, copy, erase, and name file: every switch between
		 * them rebuilds the whole screen. */
		tx_begin_out(kTxIn);
	} else if (mod == kModLoadFile && tx_in_file_menus(prev)) {
		tx_begin_out(kTxHold);
	} else if (mod == kModSpawnSelect) {
		if (tx_phase == kTxHold || tx_phase == kTxNone) {
			tx_phase = kTxIn;
			tx_level = 16;
			tx_wait = 0;
			ZeldaVideoSetTransition(16);
		}
	} else if (tx_phase == kTxHold) {
		/* Loading went somewhere with no spawn select. The game fades
		 * that in itself. */
		tx_phase = kTxNone;
		tx_level = 0;
		ZeldaVideoSetTransition(0);
	}
}

/* Once per presented frame, after the picture is up. */
static void tx_step(void)
{
	if (tx_phase == kTxOut) {
		tx_level += kTxOutStep;
		if (tx_level >= 16)
			tx_finish_out();
		else
			ZeldaVideoSetTransition(tx_level);
	} else if (tx_phase == kTxIn) {
		/* The new screen is still being built under a force blank.
		 * Start the fade once it is on, or the fade runs unseen. */
		if (tx_level == 16 && (INIDISP_copy & 0x80) && tx_wait < kTxBlankWait) {
			tx_wait++;
			return;
		}
		tx_level -= kTxInStep;
		if (tx_level <= 0) {
			tx_level = 0;
			tx_phase = kTxNone;
		}
		ZeldaVideoSetTransition(tx_level);
	}
}

static void ZeldaSettingsEnter(void)
{
	settings_open = 1;
	settings_menu = 0;
	ui = kUiGameOut;
	fade = 0;
	hud_pick = ZeldaHudOnTop() ? 0 : 1;
	beep_on = ZeldaLowHpBeep() ? 1 : 0;
	bomb_99 = ZeldaBombCap() == 99;
	arrow_99 = ZeldaArrowCap() == 99;
	run_speed = ZeldaRunSpeed() - 1;
	if (run_speed < 0)
		run_speed = 0;
	if (run_speed > 2)
		run_speed = 2;
	dark_bright = ZeldaDarkRooms() ? 1 : 0;
	hair_blonde = ZeldaHairBlonde() ? 1 : 0;
	spawn_dungeon = ZeldaSpawnDungeon() ? 1 : 0;
	mirror_free = ZeldaMirrorFree() ? 1 : 0;
	cursor = kRowHud;
	scroll_row = 0;
	save_on_exit = 0;
	menu_page = kPageSettings;
}

static int map_keys(u32 held)
{
	static const u32 kDsKey[kDsButtonCount] = {
		KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
		KEY_A, KEY_B, KEY_X, KEY_Y,
		KEY_L, KEY_R, KEY_START, KEY_SELECT
	};
	int inputs = 0;
	int added = ZeldaBottomAddedStart();
	int i;

	if ((held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT))
		held &= ~(KEY_START | KEY_SELECT);
	if ((held & (KEY_L | KEY_R | KEY_SELECT)) == (KEY_L | KEY_R | KEY_SELECT))
		held &= ~(KEY_L | KEY_R | KEY_SELECT);
	/* A Start bit the bottom screen added is SNES Start, not the DS key. */
	if (added)
		held &= ~KEY_START;

	for (i = 0; i < kSnesButtonCount; i++) {
		int ds = ZeldaButtonMap(i);

		if ((unsigned)ds < kDsButtonCount && (held & kDsKey[ds]))
			inputs |= 1 << i;
	}
	if (added)
		inputs |= 1 << kSnesStart;
	return inputs;
}

static void note_dungeon(int slot, int entrance)
{
	ZeldaSpawnRemember(slot, entrance);
}

static void forget_slot(int slot)
{
	ZeldaSpawnForget(slot);
}

static void copy_slot(int from, int to)
{
	ZeldaSpawnCopy(from, to);
}

static int file_dungeon_entrance(int slot)
{
	/* A game still in bed has never been in a dungeon. A leftover entry
	 * from a file erased by an older build must not skip the opening. */
	if (sram_progress_indicator == 0)
		return -1;
	return ZeldaSpawnPrepare(slot);
}

void ZeldaHostInit(void)
{
	ParseConfigFile(NULL);
	g_config.enable_msu = 0;
	g_config.enhanced_mode7 = false;
	g_config.extend_y = false;
	g_config.extended_aspect_ratio = 0;
	g_config.new_renderer = true;
	g_config.enable_audio = true;
	/* Hardware voices run at the SPC rate. Nothing resamples a mixed buffer. */
	g_config.audio_freq = 32000;
	g_config.audio_channels = 2;

	ZeldaLoadAssets();
	ZeldaInitialize();
	g_zenv.ppu->extraLeftRight = 0;
	g_wanted_zelda_features = 0;
	ZeldaEnableMsu(0);
	ZeldaSetLanguage(NULL);
	ZeldaVideoInit();
	ZeldaSaveMount();
	ZeldaAudioInit();
	SelectMenu_OnSettings = ZeldaSettingsEnter;
	SaveGame_NoteDungeon = note_dungeon;
	LoadFile_DungeonEntrance = file_dungeon_entrance;
	SaveFile_OnForget = forget_slot;
	SaveFile_OnCopy = copy_slot;
	memset(&lid_cookie, 0, sizeof lid_cookie);
	pmAddEventHandler(&lid_cookie, on_lid_sleep, NULL);
	/* Calico's hinge sleep clears the LCD power bit. The 2D picture does
	 * not come back, and the song stops with it. */
	pmSetSleepAllowed(false);
}

void ZeldaHostSave(void)
{
	ZeldaWriteSram();
	ZeldaSaveFlush();
}

void ZeldaHostOnKeys(u32 held, u32 down)
{
	if (!(held & KEY_START) &&
	    (down & (KEY_L | KEY_R | KEY_SELECT)) &&
	    (held & (KEY_L | KEY_R | KEY_SELECT)) == (KEY_L | KEY_R | KEY_SELECT)) {
		if (settings_menu)
			ZeldaVideoMenuEnd();
		ZeldaVideoSetFade(-1);
		tx_reset();
		settings_open = 0;
		settings_menu = 0;
		settings_eat = 0;
		menu_page = kPageSettings;
		ui = kUiOff;
		SelectMenu_LockInput = 0;
		ZeldaHostSave();
		ZeldaReset(true);
		SelectMenu_ForgetGame();
		paused = 0;
		lid_was_down = 0;
		lid_closed = 0;
		lid_wake = 0;
		lid_hold_still = 0;
		lid_frozen = 0;
		return;
	}
	if (settings_open) {
		if (ui != kUiMenu)
			return;
		if (menu_page == kPageButtons) {
			buttons_keys(down);
			return;
		}
		if (down & KEY_UP)
			settings_move(-1);
		else if (down & KEY_DOWN)
			settings_move(1);
		else if (down & KEY_LEFT)
			settings_scroll(-1);
		else if (down & KEY_RIGHT)
			settings_scroll(1);
		else if (down & KEY_B)
			settings_close(0);
		else if ((down & KEY_A) && cursor == kRowMap)
			buttons_enter();
		else if ((down & (KEY_A | KEY_START)) && cursor >= kRowSave)
			settings_close(cursor == kRowSave);
		return;
	}
	if ((down & (KEY_START | KEY_SELECT)) &&
	    (held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
		paused ^= 1;
	}
}

void ZeldaHostTick(u32 held)
{
	int spun;
	int catching;

	/* Master Sword gleam, $012E = 0x2C. The open edge, before note_lid. */
	if (lid_was_down && (held & KEY_LID) == 0)
		sound_effect_1 = 0x2c;
	note_lid(held);
	if ((held & KEY_LID) && lid_hold_still) {
		lid_frozen = 1;
		return;
	}
	lid_hold_still = 0;
	/* In game the picture keeps running so the select menu can open.
	 * The song still holds, the same as the title. */
	lid_frozen = (held & KEY_LID) != 0;
	if (settings_open || paused || ZeldaBottomHoldsGame())
		return;
	/* The old picture is fading out. The game waits for it. */
	if (tx_phase == kTxOut)
		return;
	if (settings_eat) {
		SelectMenu_LockInput = 1;
		settings_eat--;
	}
	/* A press before the sword drop runs the rest of the intro here.
	 * The sword stays on its cue. The half-second of music before that cue
	 * plays while the old picture is still up. */
	spun = 0;
	catching = 0;
	/* The sword-drop skip runs many frames before the picture updates.
	 * A loaded game is never in that skip. */
	if (!ZeldaIntroCatchingUp())
		SelectMenu_LidStep();
	for (;;) {
		ZeldaRunFrame(map_keys(held));
		if (!ZeldaIntroCatchingUp())
			break;
		if (!catching) {
			ZeldaAudioDropLeadIn();
			catching = 1;
		}
		if (++spun >= 1000) {
			ZeldaIntroCancelCatchup();
			break;
		}
		ZeldaAudioCatchupFrame();
	}
	if (ZeldaIntroCatchupLanded())
		ZeldaAudioPlayLeadIn();
	else if (catching)
		ZeldaAudioDropLeadIn();
	SelectMenu_LockInput = 0;
	tx_watch();
	/* Sprite tiles are built here, while the previous picture is still
	 * up. The vblank blit only has to DMA them. */
	ZeldaVideoComposeSprites();
}

void ZeldaHostBlit(u16 *fb)
{
	(void)fb;
	lid_wake = 0;
	switch (ui) {
	case kUiGameOut:
		if (!paused)
			ZeldaVideoDrawPpuFrame();
		ZeldaVideoSetFade(fade);
		if (fade < 16) {
			fade += 2;
			break;
		}
		ZeldaVideoMenuBegin();
		settings_menu = 1;
		settings_draw();
		ui = kUiMenuIn;
		break;
	case kUiMenuIn:
		ZeldaVideoSetFade(fade);
		if (fade > 0)
			fade -= 2;
		else
			ui = kUiMenu;
		break;
	case kUiMenu:
		ZeldaVideoSetFade(0);
		break;
	case kUiMenuOut:
		if (fade < 16) {
			fade += 2;
			ZeldaVideoSetFade(fade);
			break;
		}
		settings_apply();
		ZeldaVideoMenuEnd();
		settings_menu = 0;
		ui = kUiGameIn;
		__attribute__((fallthrough));
	case kUiGameIn:
		if (!paused)
			ZeldaVideoDrawPpuFrame();
		ZeldaVideoSetFade(fade);
		if (fade > 0) {
			fade -= 2;
			break;
		}
		ZeldaVideoSetFade(-1);
		settings_open = 0;
		settings_eat = 2;
		ui = kUiOff;
		break;
	default:
		if (!paused && !ZeldaBottomHoldsGame()) {
			/* Out: the frame that changed screen stays unshown. */
			if (tx_phase != kTxOut)
				ZeldaVideoDrawPpuFrame();
			tx_step();
		}
		break;
	}
}

void ZeldaHostAudio(void)
{
	if (paused || lid_frozen) {
		if (lid_frozen && !lid_hold_still)
			ZeldaAudioHold();
		return;
	}
	ZeldaAudioFrame();
}
