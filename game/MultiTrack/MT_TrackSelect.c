// MultiTrack: the retail track carousel (MM_TrackSelect_MenuProc_Retail) again,
// with every stock, custom and NF track on it, in three sets (stock, NF,
// custom) that Left / Right slide between. Instead of the preview video,
// each track shows a ray traced picture of its start (scripts/track_shots.py,
// assets/track_shots/<key>.png), drawn at the window's resolution; the stock
// tracks (and their NF versions) keep their minimap.

#include "platform/native_assets.h"
#include "platform/native_renderer.h"

#define MT_SHOT_CACHE 12 // pictures kept as textures (one is ~1.6 MB)
#define MT_ROW_W 280       // rows (retail 256): up to the picture panel
#define MT_PANEL_TEXT_W 216 // text under the picture: centred on it, inside the screen

// carousel turning towards the next row (retail: 3 frames), and the row it
// turns from (the cursor itself moves at once)
static int s_mtChangeFrames, s_mtChangeDir, s_mtShown = -1;

// track sets: the stock tracks, their Nitro-Fueled versions, custom tracks
enum
{
	MT_SET_STOCK,
	MT_SET_NF,
	MT_SET_CUSTOM,
	MT_SET_COUNT
};
static char *s_mtSetName[MT_SET_COUNT] = {"CTR", "NF", "CUSTOM"};

// sliding from one set to the next: the old set leaves sideways while the new
// one comes in from the other side (time based: the menu's frame rate varies)
#define MT_SLIDE_MS 1200
#define MT_SLIDE_W  560 // (past the 512 wide screen: nothing of a set left on it)
static int s_mtSlideMs, s_mtSlideDir, s_mtSlideFrom;

// Rows of a set: the unlocked stock tracks of this mode, or the table's NF /
// custom tracks whose host slot belongs to this mode. Returns the number of rows.
static int MT_RowInfo(struct MainMenu_LevelRow *stock, int numStock, int battle, int set, int row, int *levID, struct MTEntry **custom)
{
	int n = 0;
	*custom = NULL;

	if (set == MT_SET_STOCK)
	{
		for (int i = 0; i < numStock; i++)
		{
			if (!MM_TrackSelect_boolTrackOpen(&stock[i]))
				continue;
			if (n == row)
				*levID = stock[i].levID;
			n++;
		}
		return n;
	}

	struct MTHeader *table = MT_Table();
	for (int i = 0; i < table->count; i++)
	{
		struct MTEntry *e = &table->entries[i];
		if (((e->flags & MT_FLAG_BATTLE) != 0) != battle)
			continue;
		if (((e->flags & MT_FLAG_REMASTER) != 0) != (set == MT_SET_NF))
			continue;
		if (n == row)
		{
			*levID = e->slot;
			*custom = e;
		}
		n++;
	}
	return n;
}

// arcade fills the grid with bots, which need the lev's nav paths
static int MT_NoBots(struct GameTracker *gGT, struct MTEntry *e)
{
	return e && (e->flags & MT_FLAG_NO_AI) && (gGT->gameMode1 & ARCADE_MODE);
}

// the retail row of a level (its minimap, thumbnail), or NULL
static struct MainMenu_LevelRow *MT_StockRow(struct MainMenu_LevelRow *stock, int numStock, int levID)
{
	for (int i = 0; i < numStock; i++)
	{
		if (stock[i].levID == levID)
			return &stock[i];
	}
	return NULL;
}

// ---- track pictures --------------------------------------------------------

static struct
{
	char key[16];
	TextureID tex; // 0: no picture
	u32 used;      // menu frame last drawn
} s_mtShots[MT_SHOT_CACHE];
static u32 s_mtFrame;

// the picture of a row: stock-<levelID> or track-<table index>, loaded on
// first use; the least recently drawn one makes room
static TextureID MT_ShotTexture(struct MTEntry *custom, int levID)
{
	char key[16];
	if (custom)
		snprintf(key, sizeof(key), "track-%d", (int)(custom - MT_Table()->entries));
	else
		snprintf(key, sizeof(key), "stock-%d", levID);

	int victim = 0;
	for (int i = 0; i < MT_SHOT_CACHE; i++)
	{
		if (s_mtShots[i].used && !strcmp(s_mtShots[i].key, key))
		{
			s_mtShots[i].used = s_mtFrame;
			return s_mtShots[i].tex;
		}
		if (s_mtShots[i].used < s_mtShots[victim].used)
			victim = i;
	}
	if (s_mtShots[victim].used == s_mtFrame)
		return 0; // (every slot drawn this frame)

	NativeRenderer_FreeImageTexture(s_mtShots[victim].tex);
	char rel[64], path[1024];
	snprintf(rel, sizeof(rel), "track_shots/%s.png", key);
	int w = 0, h = 0;
	s_mtShots[victim].tex = NativeAssets_BuildPath(rel, path, sizeof(path)) ? NativeRenderer_LoadImageTexture(path, &w, &h) : 0;
	snprintf(s_mtShots[victim].key, sizeof(s_mtShots[victim].key), "%s", key);
	s_mtShots[victim].used = s_mtFrame;
	return s_mtShots[victim].tex;
}

static void MT_AddPrim(u32 *ot, void *prim, u32 len)
{
	*(u32 *)prim = CtrGpu_PackOTTag(*ot, len << 24);
	*ot = (u32)CtrGpu_PrimToOTLink24(prim);
}

// a texture quad drawn from a GL texture (Psy-X override packet), not VRAM:
// override on, the quad, override off (linked last first)
static void MT_DrawPicture(TextureID tex, int x, int y, int w, int h, u32 *ot, struct PrimMem *primMem)
{
	u32 *p = (u32 *)primMem->cursor;
	DR_PSYX_TEX *off = (DR_PSYX_TEX *)p;
	POLY_FT4 *q = (POLY_FT4 *)(p + 3);
	DR_PSYX_TEX *on = (DR_PSYX_TEX *)(p + 13);
	primMem->cursor = p + 16;

	off->code[0] = 0xB1000000;
	off->code[1] = 0;
	MT_AddPrim(ot, off, 2);

	memset(q, 0, sizeof(*q));
	q->r0 = q->g0 = q->b0 = 0x80;
	q->x0 = x;
	q->y0 = y;
	q->x1 = x + w;
	q->y1 = y;
	q->x2 = x;
	q->y2 = y + h;
	q->x3 = x + w;
	q->y3 = y + h;
	// (uv 0..255 over the whole picture: the override's size is given as 256)
	q->u1 = q->u3 = 0xFF;
	q->v2 = q->v3 = 0xFF;
	MT_AddPrim(ot, q, 9);
	q->code = 0x2C;

	on->code[0] = 0xB1000000 | ((u32)tex & 0xFFFFFF);
	on->code[1] = (256u << 16) | 256u;
	MT_AddPrim(ot, on, 2);
}

// a flat triangle pointing left (dir -1) or right (1), h high, its tip at x
static void MT_DrawArrow(int x, int y, int h, int dir, u32 rgb, u32 *ot, struct PrimMem *primMem)
{
	u32 *p = (u32 *)primMem->cursor;
	primMem->cursor = p + 5;
	p[1] = 0x20000000u | (rgb & 0xFFFFFFu);
	short xy[6] = {(short)x, (short)(y + h / 2), (short)(x - dir * h * 3 / 4), (short)y, (short)(x - dir * h * 3 / 4), (short)(y + h)};
	memcpy(&p[2], xy, sizeof(xy));
	MT_AddPrim(ot, p, 4);
}

// ---- text ------------------------------------------------------------------

// all the menu's text is the big font at one scale (FP 4096 = retail size):
// the largest that fits the longest track name in a row and the longest line
// under the picture
static int s_mtTextScale[2];

static int MT_TextWidth(char *str, int scale)
{
	return (DecalFont_GetLineWidth(str, FONT_BIG) * scale) >> 12;
}

static int MT_TextHeight(int scale)
{
	return (data.font_charPixHeight[FONT_BIG] * scale) >> 12;
}

// DecalFont_DrawLine (FONT_BIG) with every glyph and advance scaled
static void MT_DrawText(char *str, int posX, int posY, int scale, int flags)
{
	struct GameTracker *gGT = sdata->gGT;
	struct IconGroup *group = gGT->iconGroup[data.font_IconGroupID[FONT_BIG]];
	if (flags & JUSTIFY_CENTER)
		posX -= MT_TextWidth(str, scale) >> 1;
	Color *colors = (Color *)data.ptrColor[flags & 0xfff];
	int advance = 0; // (FP: no rounding drift along the line)
	for (u8 *c = (u8 *)str; *c != 0; c++)
	{
		int charWidth = data.font_charPixWidth[FONT_BIG];
		if ((*c == ':') || (*c == '.'))
			charWidth = data.font_puncPixWidth[FONT_BIG];
		if (*c < 3)
			continue; // (accent placeholders: none in track names)
		if (((u32)(*c - 0x21) < 0xdf) && (group != NULL))
		{
			u16 iconID = data.font_characterIconID[*c - 0x21];
			if ((iconID < 0x80) && (iconID < group->numIcons))
			{
				struct Icon **icons = ICONGROUP_GETICONS(group);
				DecalHUD_DrawPolyGT4(icons[iconID], posX + (advance >> 12), posY, &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT,
				                     ColorCode_Load(&colors[0]), ColorCode_Load(&colors[1]), ColorCode_Load(&colors[2]), ColorCode_Load(&colors[3]), 0,
				                     scale);
			}
		}
		advance += charWidth * scale;
	}
}

// the text scale of this mode's screen (once: the track table doesn't change)
static int MT_TextScale(struct MainMenu_LevelRow *stock, int numStock, int battle)
{
	if (s_mtTextScale[battle])
		return s_mtTextScale[battle];
	int nameW = 1, lineW = 1;
	char line[96];
	for (int i = 0; i < numStock; i++)
	{
		int w = DecalFont_GetLineWidth(sdata->lngStrings[data.metaDataLEV[stock[i].levID].name_LNG], FONT_BIG);
		nameW = (w > nameW) ? w : nameW;
	}
	struct MTHeader *table = MT_Table();
	for (int i = 0; i < table->count; i++)
	{
		struct MTEntry *e = &table->entries[i];
		if (((e->flags & MT_FLAG_BATTLE) != 0) != battle)
			continue;
		int w = DecalFont_GetLineWidth(e->name, FONT_BIG);
		nameW = (w > nameW) ? w : nameW;
		snprintf(line, sizeof(line), "BY %s", e->author);
		w = DecalFont_GetLineWidth(line, FONT_BIG);
		lineW = (w > lineW) ? w : lineW;
		snprintf(line, sizeof(line), "ON %s", sdata->lngStrings[data.metaDataLEV[e->slot].name_LNG]);
		w = DecalFont_GetLineWidth(line, FONT_BIG);
		lineW = (w > lineW) ? w : lineW;
	}
	char *fixed[] = {"NO AI PATHS", "USE TIME TRIAL OR VS", "999/999  L1/R1 PAGE", s_mtSetName[0], s_mtSetName[1], s_mtSetName[2]};
	for (int i = 0; i < 6; i++)
	{
		int w = DecalFont_GetLineWidth(fixed[i], FONT_BIG);
		lineW = (w > lineW) ? w : lineW;
	}
	int a = ((MT_ROW_W - 16) << 12) / nameW, b = (MT_PANEL_TEXT_W << 12) / lineW;
	int scale = (a < b) ? a : b;
	s_mtTextScale[battle] = (scale > FP(1.0)) ? FP(1.0) : scale;
	return s_mtTextScale[battle];
}

// ---- drawing ---------------------------------------------------------------

// the stock level's minimap in the panel (retail: 6 layers about mapRect's centre)
static void MT_DrawMap(struct GameTracker *gGT, int mapTextureID, RECT16 mapRect)
{
	struct Icon *map0 = gGT->ptrIcons[mapTextureID], *map1 = gGT->ptrIcons[mapTextureID + 1];
	int mapW = (s16)(map0->texLayout.u1 - map0->texLayout.u0);
	int mapH = (s16)((map0->texLayout.v2 - map0->texLayout.v0) + (map1->texLayout.v2 - map1->texLayout.v0));
	for (int layer = 0; layer < MM_TRACK_SELECT_MAP_LAYER_COUNT; layer++)
	{
		struct TrackSelectMapOffset *o = &MM_TRACK_DRAW_MAP_OFFSET[layer];
		UI_Map_DrawMap(map0, map1, o->offsetX + mapRect.x + mapRect.w / 2 + mapW / 2, o->offsetY + mapRect.y + mapRect.h / 2 + mapH / 2,
		               &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT, o->type);
	}
}

// the picture panel (retail: the preview video's box, the title above it, the
// minimap): the cursor's track
static void MT_DrawPanel(struct GameTracker *gGT, struct MainMenu_LevelRow *stockRow, struct MTEntry *custom, int levID, int cursor, int numRows,
                         int scale, int set, int dx)
{
	int lineH = MT_TextHeight(scale) + 2;
	// stock tracks and their NF versions have the stock minimap; custom ones
	// none, nor NF tracks on a fan remake (key "NF-<id>" in their author field;
	// "NF-<slot>": a stock level with NF collision, its own map)
	int stockMap = !custom || ((custom->flags & MT_FLAG_REMASTER) && (strncmp(custom->author, "NF-", 3) || ((custom->author[3] >= '0') && (custom->author[3] <= '9'))));
	// (NF tracks: their own, scripts/nf_minimap.py)
	const struct MTMap *nfMap = custom ? MT_Map_Find(custom) : NULL;
	int mapID = (!nfMap && stockRow && stockMap) ? stockRow->mapTextureID : -1;
	// (one layout for every row, the retail one with a map: title, picture and
	// the lines under it stay put while scrolling; no map, its place left empty)
	RECT16 r = {MM_TRACK_TRANSITIONS[1].currX + MM_TRACK_SELECT_PREVIEW_X + dx, MM_TRACK_TRANSITIONS[1].currY + MM_TRACK_SELECT_PREVIEW_Y_WITH_MAP,
	            MM_TRACK_VIDEO_WIDTH, MM_TRACK_VIDEO_HEIGHT};

	if (MM_TRACK_LAP_BOX_OPEN == 0)
	{
		// the set's name where retail has "SELECT LEVEL" (two big lines; their
		// block's middle kept)
		int ty = MM_TRACK_TRANSITIONS[3].currY + (u16)r.y + (MM_TRACK_SELECT_LEVEL_TEXT_Y_STEP + data.font_charPixHeight[FONT_BIG]) / 2 - lineH / 2;
		int tx = MM_TRACK_TRANSITIONS[3].currX + MM_TRACK_SELECT_TITLE_X + dx;
		MT_DrawText(s_mtSetName[set], tx, ty, scale, JUSTIFY_CENTER | ORANGE);
		// (the slide arrows as triangles: the font has no '>')
		int half = (MT_TextWidth(s_mtSetName[set], scale) >> 1) + MT_TextHeight(scale) / 2, ah = MT_TextHeight(scale) * 2 / 3;
		Color c = ColorCode_Load(&((Color *)data.ptrColor[ORANGE])[0]);
		u32 rgb = (u32)c.r | ((u32)c.g << 8) | ((u32)c.b << 16);
		MT_DrawArrow(tx - half - ah, ty + (MT_TextHeight(scale) - ah) / 2, ah, -1, rgb, gGT->pushBuffer_UI.ptrOT, &gGT->backBuffer->primMem);
		MT_DrawArrow(tx + half + ah, ty + (MT_TextHeight(scale) - ah) / 2, ah, 1, rgb, gGT->pushBuffer_UI.ptrOT, &gGT->backBuffer->primMem);
	}
	r.y += MM_TRACK_SELECT_TITLE_TO_MAP_Y;

	if (((mapID >= 0) || nfMap) && (MM_TRACK_LAP_BOX_OPEN == 0))
	{
		RECT16 mapRect = r;
		mapRect.h = MM_TRACK_SELECT_MAP_BOX_H;
		mapRect.x += MM_TRACK_TRANSITIONS[2].currX - MM_TRACK_TRANSITIONS[1].currX; // (dx: in r.x)
		mapRect.y += MM_TRACK_TRANSITIONS[2].currY - MM_TRACK_TRANSITIONS[1].currY + MM_TRACK_SELECT_MAP_CENTER_Y_OFFSET;
		if (nfMap)
		{
			MT_DrawPicture(nfMap->tex, mapRect.x + mapRect.w / 2 - nfMap->w / 2, mapRect.y + mapRect.h / 2 - nfMap->h / 2, nfMap->w, nfMap->h,
			               gGT->pushBuffer_UI.ptrOT, &gGT->backBuffer->primMem);
		}
		else
		{
			MT_DrawMap(gGT, mapID, mapRect);
		}
	}

	// the picture (none taken: the retail thumbnail, stock tracks only)
	int px = r.x + MM_TRACK_VIDEO_FRAME_SRC_OFFSET_X, py = r.y + MM_TRACK_VIDEO_FRAME_SRC_OFFSET_Y;
	TextureID tex = MT_ShotTexture(custom, levID);
	if (tex != 0)
	{
		MT_DrawPicture(tex, px, py, MM_TRACK_VIDEO_FRAME_WIDTH, MM_TRACK_VIDEO_FRAME_HEIGHT, gGT->pushBuffer_UI.ptrOT, &gGT->backBuffer->primMem);
	}
	else if (stockRow && !custom)
	{
		MM_RECTMENU_DRAW_POLY_GT4(gGT->ptrIcons[stockRow->videoThumbnail], px, py, &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT, MM_VIDEO_COLOR,
		                          MM_VIDEO_COLOR, MM_VIDEO_COLOR, MM_VIDEO_COLOR, 0, FP(1.0));
	}
	RECTMENU_DrawInnerRect((RECT *)&r, 1, gGT->backBuffer->otMem.uiOT);

	// under the picture: who made it (custom tracks), and where in the list
	int cx = r.x + r.w / 2, y = r.y + r.h + 4;
	if (MM_TRACK_LAP_BOX_OPEN == 0)
	{
		if (custom && !(custom->flags & MT_FLAG_REMASTER))
		{
			char info[64];
			snprintf(info, sizeof(info), "BY %s", custom->author);
			MT_DrawText(info, cx, y, scale, JUSTIFY_CENTER | WHITE);
			snprintf(info, sizeof(info), "ON %s", sdata->lngStrings[data.metaDataLEV[custom->slot].name_LNG]);
			MT_DrawText(info, cx, y + lineH, scale, JUSTIFY_CENTER | PERIWINKLE);
			y += 2 * lineH;
		}
		if (MT_NoBots(gGT, custom))
		{
			MT_DrawText("NO AI PATHS", cx, y, scale, JUSTIFY_CENTER | RED);
			MT_DrawText("USE TIME TRIAL OR VS", cx, y + lineH, scale, JUSTIFY_CENTER | RED);
			y += 2 * lineH;
		}
		if ((mapID < 0) && !nfMap)
		{
			char count[32];
			snprintf(count, sizeof(count), "%d/%d  L1/R1 PAGE", cursor + 1, numRows);
			MT_DrawText(count, cx, y, scale, JUSTIFY_CENTER | PERIWINKLE);
		}
	}
}

// the carousel: nine rows on the retail wheel, the shown row in the middle,
// turning towards the cursor
static void MT_DrawCarousel(struct GameTracker *gGT, struct MainMenu_LevelRow *stock, int numStock, int battle, int set, int shown, int numRows,
                            int changeFrames, int scale, int dx)
{
	for (int i = 0; i < MM_TRACK_SELECT_VISIBLE_ROWS; i++)
	{
		int angle = (i - MM_TRACK_SELECT_CENTER_ROW) * MM_TRACK_SELECT_ROW_ANGLE_STEP;
		if (changeFrames > 0)
		{
			angle += ((MM_TRACK_SELECT_TRACK_CHANGE_FRAMES - changeFrames) * MM_TRACK_SELECT_ROW_ANGLE_STEP) / MM_TRACK_SELECT_TRACK_CHANGE_FRAMES *
			         s_mtChangeDir;
		}
		int x = MM_TRACK_TRANSITIONS[0].currX + ((MATH_Cos(angle & 0xFFF) * MM_TRACK_SELECT_ROW_X_RADIUS) >> MM_TRACK_SELECT_ROW_X_SHIFT) +
		        MM_TRACK_SELECT_ROW_X_OFFSET + dx;
		int y = MM_TRACK_TRANSITIONS[0].currY + ((MATH_Sin(angle & 0xFFF) * MM_TRACK_SELECT_ROW_Y_RADIUS) >> MM_TRACK_SELECT_ROW_Y_SHIFT) +
		        MM_TRACK_SELECT_ROW_Y_OFFSET;
		RECT16 rowRect = {x, y, MT_ROW_W, MM_TRACK_SELECT_ROW_H};

		int row = ((shown + i - MM_TRACK_SELECT_CENTER_ROW) % numRows + numRows) % numRows;
		int levID = 0;
		struct MTEntry *e;
		MT_RowInfo(stock, numStock, battle, set, row, &levID, &e);
		char *name = e ? e->name : sdata->lngStrings[data.metaDataLEV[levID].name_LNG];

		MT_DrawText(name, x + MM_TRACK_SELECT_ROW_NAME_X_OFFSET, y + (MM_TRACK_SELECT_ROW_H - MT_TextHeight(scale) + 1) / 2, scale, ORANGE);

		if ((changeFrames == 0) && (i == MM_TRACK_SELECT_CENTER_ROW))
		{
			RECT16 hl = {rowRect.x + MM_TRACK_SELECT_HIGHLIGHT_INSET_X, rowRect.y + MM_TRACK_SELECT_HIGHLIGHT_INSET_Y,
			             rowRect.w - MM_TRACK_SELECT_HIGHLIGHT_W_SHRINK, rowRect.h - MM_TRACK_SELECT_HIGHLIGHT_H_SHRINK};
			CTR_Box_DrawClearBox((RECT *)&hl, &sdata->menuRowHighlight_Normal, TRANS_50_DECAL, gGT->backBuffer->otMem.uiOT, &gGT->backBuffer->primMem);
		}
		RECTMENU_DrawInnerRect((RECT *)&rowRect, 0, gGT->backBuffer->otMem.uiOT);
	}
}

void MM_TrackSelect_MenuProc(struct RectMenu *menu)
{
	struct GameTracker *gGT = sdata->gGT;
	s16 elapsedFrames = MM_TRACK_TRANSITION_FRAME;
	int battle = (gGT->gameMode1 & BATTLE_MODE) != 0;
	(void)menu;

	if (MT_STATE->magic != MT_MAGIC)
	{
		MT_STATE->magic = MT_MAGIC;
		MT_STATE->redirectLevel = -1;
		memset(MT_STATE->cursor, 0, sizeof(MT_STATE->cursor));
		MT_STATE->set[0] = MT_SET_STOCK;
		MT_STATE->set[1] = MT_SET_STOCK;
	}

	if (MM_TRACK_TRANSITION_STATE == ENTERING_MENU)
	{
		MM_ERROR_MESSAGE_POS_INDEX = battle ? 2 : 1;
		MM_TransitionInOut(MM_TRACK_TRANSITIONS, elapsedFrames, MM_TRACK_SELECT_SLIDE_FRAMES);
		if (elapsedFrames == 0)
			MM_TRACK_TRANSITION_STATE = IN_MENU;
		else
			elapsedFrames--;
		s_mtShown = -1;
		s_mtChangeFrames = 0;
		s_mtSlideMs = 0;
	}
	else if (MM_TRACK_TRANSITION_STATE == EXITING_MENU)
	{
		MM_TransitionInOut(MM_TRACK_TRANSITIONS, elapsedFrames, MM_TRACK_SELECT_SLIDE_FRAMES);
		elapsedFrames++;

		if (elapsedFrames > MM_TRACK_SELECT_TRANSITION_FRAMES)
		{
			MM_TRACK_TRANSITION_FRAME = elapsedFrames;
			MM_ERROR_MESSAGE_POS_INDEX = 0;

			if (MM_TRACK_TRANSITION_START_AFTER_EXIT == 0)
			{
				MT_STATE->redirectLevel = -1;
				MM_DESIRED_MENU = &MM_MENU_CHARACTER_SELECT;
				MM_Characters_RestoreIDs();
				return;
			}

			if (battle)
			{
				MM_DESIRED_MENU = &MM_MENU_BATTLE_WEAPONS;
				MM_Battle_Init();
				return;
			}

			if ((gGT->gameMode1 & TIME_TRIAL) != 0)
			{
				MM_GHOST_TAPE_PLAYING = MM_ALLOC_HIGH_MEM(MM_TRACK_SELECT_GHOST_TAPE_ALLOC_SIZE, MM_LOADED_GHOST_DATA);
				memset(MM_GHOST_TAPE_PLAYING, 0, MM_TRACK_SELECT_GHOST_TAPE_CLEAR_SIZE);
				MM_REPLAY_HUMAN_GHOST = 0;
				SelectProfile_ToggleMode(SELECT_PROFILE_SCREEN_GHOST);
				MM_DESIRED_MENU = &MM_MENU_GHOST_SELECTION;
				return;
			}

			MM_DESIRED_MENU = &MM_MENU_QUEUE_LOAD_TRACK;
			return;
		}
	}
	MM_TRACK_TRANSITION_FRAME = elapsedFrames;
	s_mtFrame++;

	struct MainMenu_LevelRow *stock = battle ? &MM_BATTLE_TRACKS[0] : &MM_ARCADE_TRACKS[0];
	int numStock = battle ? MM_TRACK_SELECT_BATTLE_TRACK_COUNT : MM_TRACK_SELECT_ARCADE_TRACK_COUNT;

	int levID = 0;
	struct MTEntry *custom;
	int set = MT_STATE->set[battle];
	if ((set < 0) || (set >= MT_SET_COUNT) || (MT_RowInfo(stock, numStock, battle, set, -1, &levID, &custom) == 0))
	{
		set = MT_SET_STOCK; // (a set with no tracks: this build has no NF / custom ones)
		s_mtShown = -1;
	}
	int numRows = MT_RowInfo(stock, numStock, battle, set, -1, &levID, &custom);
	int cursor = MT_STATE->cursor[battle][set];
	if (cursor >= numRows)
		cursor = 0;
	if ((s_mtShown < 0) || (s_mtShown >= numRows))
		s_mtShown = cursor;

	struct MTEntry *cursorEntry;
	MT_RowInfo(stock, numStock, battle, set, cursor, &levID, &cursorEntry);

	if (MM_TRACK_LAP_BOX_OPEN == 0)
	{
		int tap = MM_GAME_BUTTON_TAPS[0] &
		          (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_L1 | BTN_R1 | BTN_TRIANGLE | BTN_SQUARE_one | BTN_CROSS_one | BTN_CIRCLE);

		// (retail: no input while the carousel turns; none while a set slides)
		if ((MM_TRACK_TRANSITION_STATE == IN_MENU) && (tap != 0) && (s_mtChangeFrames == 0) && (s_mtSlideMs == 0))
		{
			if (tap & (BTN_LEFT | BTN_RIGHT))
			{
				// the next set (with tracks) that way; the cursor stays where it was in each
				int dir = (tap & BTN_RIGHT) ? 1 : -1, next = set;
				for (int k = 1; k < MT_SET_COUNT; k++)
				{
					int s2 = ((set + dir * k) % MT_SET_COUNT + MT_SET_COUNT) % MT_SET_COUNT;
					if (MT_RowInfo(stock, numStock, battle, s2, -1, &levID, &custom) > 0)
					{
						next = s2;
						break;
					}
				}
				if (next != set)
				{
					MT_STATE->cursor[battle][set] = cursor;
					s_mtSlideFrom = set;
					s_mtSlideDir = dir;
					s_mtSlideMs = 1;
					set = next;
					numRows = MT_RowInfo(stock, numStock, battle, set, -1, &levID, &custom);
					cursor = MT_STATE->cursor[battle][set];
					if (cursor >= numRows)
						cursor = 0;
					s_mtShown = cursor;
					MT_RowInfo(stock, numStock, battle, set, cursor, &levID, &cursorEntry);
					OtherFX_Play(0, 1);
				}
			}
			else if (tap & BTN_DOWN)
			{
				cursor = (cursor + 1 >= numRows) ? 0 : cursor + 1;
				s_mtChangeFrames = MM_TRACK_SELECT_TRACK_CHANGE_FRAMES;
				s_mtChangeDir = -1;
			}
			else if (tap & BTN_UP)
			{
				cursor = (cursor == 0) ? numRows - 1 : cursor - 1;
				s_mtChangeFrames = MM_TRACK_SELECT_TRACK_CHANGE_FRAMES;
				s_mtChangeDir = 1;
			}
			else if (tap & (BTN_L1 | BTN_R1))
			{
				// a page of the carousel at once
				cursor = (tap & BTN_L1) ? ((cursor - MM_TRACK_SELECT_VISIBLE_ROWS) % numRows + numRows) % numRows
				                        : (cursor + MM_TRACK_SELECT_VISIBLE_ROWS) % numRows;
				s_mtShown = cursor;
			}

			if (tap & (BTN_UP | BTN_DOWN | BTN_L1 | BTN_R1))
				OtherFX_Play(0, 1);

			if ((tap & (BTN_CROSS_one | BTN_CIRCLE)) && MT_NoBots(gGT, cursorEntry))
			{
				OtherFX_Play(2, 1);
			}
			else if (tap & (BTN_CROSS_one | BTN_CIRCLE))
			{
				OtherFX_Play(1, 1);
				if ((gGT->gameMode1 & (BATTLE_MODE | TIME_TRIAL)) == 0)
				{
					MM_TRACK_LAP_BOX_OPEN = MM_TRACK_TRANSITION_STATE;
				}
				else
				{
					MM_TRACK_TRANSITION_START_AFTER_EXIT = MM_TRACK_TRANSITION_STATE;
					MM_TRACK_TRANSITION_STATE = EXITING_MENU;
				}
			}
			else if (tap & (BTN_TRIANGLE | BTN_SQUARE_one))
			{
				OtherFX_Play(2, 1);
				MM_TRACK_TRANSITION_START_AFTER_EXIT = 0;
				MM_TRACK_TRANSITION_STATE = EXITING_MENU;
			}

			RECTMENU_ClearInput();
		}
	}
	else
	{
		s32 lapState = 0;
		MM_MENU_LAP_SELECT.rowSelected = MM_USELESS_LAP_ROW_COPY;

		if (MM_TRACK_TRANSITION_STATE == IN_MENU)
			lapState = RECTMENU_ProcessInput(&MM_MENU_LAP_SELECT);

		RECTMENU_DrawSelf(&MM_MENU_LAP_SELECT, MM_TRACK_TRANSITIONS[2].currX, MM_TRACK_TRANSITIONS[2].currY, MM_TRACK_SELECT_LAP_MENU_WIDTH);

		MM_USELESS_LAP_ROW_COPY = MM_MENU_LAP_SELECT.rowSelected;
		gGT->numLaps = MM_LAP_COUNT_BY_ROW[MM_MENU_LAP_SELECT.rowSelected].lapCount;

		if (lapState == 1)
		{
			MM_TRACK_TRANSITION_STATE = EXITING_MENU;
			MM_TRACK_TRANSITION_START_AFTER_EXIT = 1;
		}
		else if (lapState == -1)
		{
			MM_TRACK_LAP_BOX_OPEN = 0;
		}

		if ((gGT->gameMode2 & CHEAT_ONELAP) != 0)
			gGT->numLaps = 1;
	}

	MT_STATE->cursor[battle][set] = cursor;
	MT_STATE->set[battle] = set;

	// highlighted row decides what QueueLoadTrack will load
	MT_RowInfo(stock, numStock, battle, set, cursor, &levID, &custom);
	gGT->currLEV = levID;
	MT_STATE->redirectBase = custom ? custom->bigBase : 0;
	MT_STATE->redirectLevel = custom ? custom->slot : -1;
	MT_STATE->entry = custom ? (short)(custom - MT_Table()->entries + 1) : 0;

	// no preview videos: every track shows its picture instead
	MM_TrackSelect_Video_State(1);

	int scale = MT_TextScale(stock, numStock, battle);
	int dx = 0;
	if (s_mtSlideMs > 0)
	{
		// eased (slow out, slow in): the old set leaves by dir, the new one follows
		int t = (s_mtSlideMs << 12) / MT_SLIDE_MS;
		int p = (t * t >> 12) * (FP(3.0) - 2 * t) >> 12;
		dx = s_mtSlideDir * ((MT_SLIDE_W * (FP(1.0) - p)) >> 12);

		int oldLev = 0;
		struct MTEntry *oldEntry;
		int oldRows = MT_RowInfo(stock, numStock, battle, s_mtSlideFrom, -1, &oldLev, &oldEntry);
		int oldCursor = MT_STATE->cursor[battle][s_mtSlideFrom];
		oldCursor = (oldCursor < oldRows) ? oldCursor : 0;
		MT_RowInfo(stock, numStock, battle, s_mtSlideFrom, oldCursor, &oldLev, &oldEntry);
		int oldDx = dx - s_mtSlideDir * MT_SLIDE_W;
		MT_DrawCarousel(gGT, stock, numStock, battle, s_mtSlideFrom, oldCursor, oldRows, 0, scale, oldDx);
		MT_DrawPanel(gGT, MT_StockRow(stock, numStock, oldLev), oldEntry, oldLev, oldCursor, oldRows, scale, s_mtSlideFrom, oldDx);

		int ms = gGT->elapsedTimeMS;
		s_mtSlideMs += (ms > 0 && ms < 100) ? ms : 33;
		if (s_mtSlideMs >= MT_SLIDE_MS)
			s_mtSlideMs = 0;
	}
	MT_DrawCarousel(gGT, stock, numStock, battle, set, s_mtShown, numRows, s_mtChangeFrames, scale, dx);
	if (s_mtChangeFrames > 0)
	{
		if (--s_mtChangeFrames == 0)
			s_mtShown = cursor;
	}
	MT_DrawPanel(gGT, MT_StockRow(stock, numStock, levID), custom, levID, cursor, numRows, scale, set, dx);
}
