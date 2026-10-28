// MultiTrack: Settings on the main menu. A cog at the bottom left of the main
// menu's top level (not its sub menus, character or track select): Left there
// moves the focus to it (only from the cursor's leftmost spot), Cross opens
// Settings, Back / Right goes back to the menu. Settings: resolution, fullscreen, ray tracing on/off and its
// samples per pixel, an FPS counter (platform/native_overlay.c), baked ray
// tracing (platform/native_rtx_bake.c: the GL
// renderer draws levels with their baked lighting) and baking it. They are
// saved to settings.ini (platform/native_settings.c) and stay in step with the
// F12 overlay / F7 / F11 (same state underneath).

#include "platform/native_rtx.h"
#include "platform/native_settings.h"

enum
{
	MT_SET_NONE,  // the menu has the input
	MT_SET_COG,   // the cog is focused
	MT_SET_PANEL, // Settings is open
	MT_SET_CONFIRM, // "Bake now": the warning, Yes / No
	MT_SET_BAKING,  // baking (Back stops it after the level being baked)
};

enum
{
	MT_ROW_RESOLUTION,
	MT_ROW_FULLSCREEN,
	MT_ROW_RTX,
	MT_ROW_SPP,
	MT_ROW_BAKED,
	MT_ROW_BAKE_NOW,
	MT_ROW_FPS,
	MT_ROW_COUNT,
};

#define MT_SET_MAX_MODES 32
#define MT_SET_PANEL_W   300
#define MT_COG_X         28 // (the main menu's screen is 512 x 216)
#define MT_COG_Y         198
#define MT_COG_R         10 // teeth (screen rows; a PS1 pixel is 0.5625 as wide as tall)
#define MT_COG_SPP_MIN   4  // (rtx_shared.h RTX_MIN_SPP .. RTX_MAX_SPP in RTX_SPP_STEP)
#define MT_COG_SPP_MAX   64
#define MT_COG_SPP_STEP  4

static struct
{
	int focus;
	int row;
	int numModes; // resolutions: 0 = the desktop's, then the display's modes
	int modeW[MT_SET_MAX_MODES + 1], modeH[MT_SET_MAX_MODES + 1];
	int pendingMode; // resolution picked, applied by Cross
	int confirmYes;  // the bake warning's choice
	int bakeTotal;   // after a bake: levels looked at (0: none ran)
} s_mtSet;

// ---- resolutions -------------------------------------------------------------

static void MT_Settings_ReadModes(void)
{
	s_mtSet.modeW[0] = s_mtSet.modeH[0] = 0; // (the desktop's)
	s_mtSet.numModes = 1 + Platform_GetDisplayModes(&s_mtSet.modeW[1], &s_mtSet.modeH[1], MT_SET_MAX_MODES);
}

// the list entry of the resolution in use (a saved one the display no longer
// has: the desktop's)
static int MT_Settings_CurrentMode(void)
{
	int w, h, fs;
	Platform_GetDisplay(&w, &h, &fs);
	for (int i = 0; i < s_mtSet.numModes; i++)
	{
		if ((s_mtSet.modeW[i] == w) && (s_mtSet.modeH[i] == h))
			return i;
	}
	return 0;
}

// ---- where Left goes to the cog ---------------------------------------------

// a RectMenu box's row has nothing to its left
static int MT_Settings_RectMenuLeftEdge(struct RectMenu *m)
{
	if (m->rowSelected < 0)
		return 0;
	return m->rows[m->rowSelected].rowOnPressLeft == m->rowSelected;
}

// the top level of the main menu (none of its boxes open under it): the only
// screen with the cog
static int MT_Settings_OnMainMenu(void)
{
	return (sdata->ptrActiveMenu == &MM_MENU_MAIN) && (MM_TITLE_MENU_STATE == TITLE_MENU_STATE_IN_MENU) &&
	       !(MM_MENU_MAIN.state & DRAW_NEXT_MENU_IN_HIERARCHY);
}

static int MT_Settings_LeftReachesCog(void)
{
	if (sdata->buttonHeldPerPlayer[0] & (BTN_L1 | BTN_R1))
		return 0; // (L1 + R1 held: a cheat code being typed in)
	return MT_Settings_OnMainMenu() && MT_Settings_RectMenuLeftEdge(sdata->ptrActiveMenu);
}

// ---- drawing ---------------------------------------------------------------

static int MT_Settings_Lerp(int a, int b, int t, int n)
{
	return a + (b - a) * t / n;
}

// the cog: a toothed ring of gouraud quads, coloured top to bottom like the
// menus' font (colour index), over a black rim
static void MT_DrawCog(int cx, int cy, int grow, const Color *top, const Color *bottom, u32 *ot, struct PrimMem *primMem)
{
	enum
	{
		SEGMENTS = 64,
		TEETH = 8
	};
	const int rTooth = MT_COG_R + grow, rBody = (MT_COG_R * 3) / 4 + grow, rHole = MT_COG_R / 3 - grow;
	for (int i = 0; i < SEGMENTS; i++)
	{
		POLY_G4 *q = (POLY_G4 *)primMem->cursor;
		if ((u8 *)(q + 1) > (u8 *)primMem->guardEnd)
			return;
		primMem->cursor = q + 1;

		// half of each tooth's arc is raised
		int phase = (i * TEETH * 4 / SEGMENTS) & 3;
		int r = ((phase == 1) || (phase == 2)) ? rTooth : rBody;
		int a0 = i * 4096 / SEGMENTS, a1 = (i + 1) * 4096 / SEGMENTS;
		int xy[4][2] = {
		    {cx + ((MATH_Cos(a0) * rHole * 16 / 9) >> 12), cy + ((MATH_Sin(a0) * rHole) >> 12)},
		    {cx + ((MATH_Cos(a1) * rHole * 16 / 9) >> 12), cy + ((MATH_Sin(a1) * rHole) >> 12)},
		    {cx + ((MATH_Cos(a0) * r * 16 / 9) >> 12), cy + ((MATH_Sin(a0) * r) >> 12)},
		    {cx + ((MATH_Cos(a1) * r * 16 / 9) >> 12), cy + ((MATH_Sin(a1) * r) >> 12)},
		};
		memset(q, 0, sizeof(*q));
		u8 *rgb[4] = {&q->r0, &q->r1, &q->r2, &q->r3};
		s16 *pos[4] = {&q->x0, &q->x1, &q->x2, &q->x3};
		for (int k = 0; k < 4; k++)
		{
			int t = xy[k][1] - (cy - rTooth);
			int n = 2 * rTooth;
			t = (t < 0) ? 0 : ((t > n) ? n : t);
			rgb[k][0] = (u8)MT_Settings_Lerp(top->r, bottom->r, t, n);
			rgb[k][1] = (u8)MT_Settings_Lerp(top->g, bottom->g, t, n);
			rgb[k][2] = (u8)MT_Settings_Lerp(top->b, bottom->b, t, n);
			pos[k][0] = (s16)xy[k][0];
			pos[k][1] = (s16)xy[k][1];
		}
		MT_AddPrim(ot, q, 8);
		q->code = 0x38;
	}
}

static void MT_Settings_DrawCog(struct GameTracker *gGT)
{
	u32 *ot = gGT->backBuffer->otMem.uiOT;
	struct PrimMem *primMem = &gGT->backBuffer->primMem;
	Color *orange = (Color *)data.ptrColor[ORANGE];
	static const Color black; // (zero)
	// (linked last first: the highlight under the cog under ... the rim)
	MT_DrawCog(MT_COG_X, MT_COG_Y, 0, &orange[0], &orange[2], ot, primMem);
	MT_DrawCog(MT_COG_X + 1, MT_COG_Y + 1, 1, &black, &black, ot, primMem);
	if (s_mtSet.focus != MT_SET_NONE)
	{
		RECT r = {MT_COG_X - MT_COG_R * 16 / 9 - 5, MT_COG_Y - MT_COG_R - 3, (MT_COG_R * 16 / 9 + 5) * 2, MT_COG_R * 2 + 6};
		CTR_Box_DrawClearBox(&r, &GAME_MENU_HIGHLIGHT, 1, ot, primMem);
	}
}

static void MT_Settings_ValueText(int row, char *buf, int size)
{
	switch (row)
	{
	case MT_ROW_RESOLUTION:
	{
		int i = s_mtSet.pendingMode;
		if (s_mtSet.modeW[i] <= 0)
			snprintf(buf, size, "DESKTOP");
		else
			snprintf(buf, size, "%dX%d", s_mtSet.modeW[i], s_mtSet.modeH[i]);
		break;
	}
	case MT_ROW_FULLSCREEN:
	{
		int w, h, fs;
		Platform_GetDisplay(&w, &h, &fs);
		snprintf(buf, size, "%s", fs ? "ON" : "OFF");
		break;
	}
	case MT_ROW_RTX:
		snprintf(buf, size, "%s", !Rtx_Supported() ? "NOT SUPPORTED" : (Rtx_Enabled() ? "ON" : "OFF (BAKED)"));
		break;
	case MT_ROW_SPP:
		snprintf(buf, size, "%d", Rtx_GetSpp());
		break;
	case MT_ROW_BAKED:
		snprintf(buf, size, "%s", Rtx_BakedEnabled() ? "ON" : "OFF");
		break;
	case MT_ROW_BAKE_NOW:
		snprintf(buf, size, "%s", s_mtSet.bakeTotal ? "DONE" : "");
		break;
	case MT_ROW_FPS:
		snprintf(buf, size, "%s", NativeSettings_GetInt("fps_counter", 0) ? "ON" : "OFF");
		break;
	default:
		buf[0] = 0;
	}
}

static int MT_Settings_RowLocked(int row)
{
	// (a ray tracing GPU never draws with the GL bake: ray tracing off is the lightmap)
	return (((row == MT_ROW_RTX) || (row == MT_ROW_SPP)) && !Rtx_Supported()) || ((row == MT_ROW_BAKED) && Rtx_Supported());
}

static void MT_Settings_DrawPanel(struct GameTracker *gGT)
{
	static char *labels[MT_ROW_COUNT] = {"RESOLUTION", "FULLSCREEN", "RAY TRACING", "RT SAMPLES", "BAKED RT", "BAKE NOW", "FPS COUNTER"};
	u32 *ot = gGT->backBuffer->otMem.uiOT;
	struct PrimMem *primMem = &gGT->backBuffer->primMem;
	int rowH = data.font_charPixHeight[FONT_BIG] + 3;
	int titleH = rowH + 6;
	int h = titleH + MT_ROW_COUNT * rowH + 4;
	int x = 256 - MT_SET_PANEL_W / 2, y = 108 - h / 2;

	// (text first: linked last first, it ends up over the highlight and box)
	DecalFont_DrawLine("SETTINGS", 256, y + 2, FONT_BIG, JUSTIFY_CENTER | ORANGE);
	for (int i = 0; i < MT_ROW_COUNT; i++)
	{
		char value[32];
		int ry = y + titleH + i * rowH + 2;
		int color = MT_Settings_RowLocked(i) ? 0x17 : ORANGE;
		MT_Settings_ValueText(i, value, sizeof(value));
		DecalFont_DrawLine(((i == MT_ROW_BAKE_NOW) && Rtx_Supported()) ? "REBAKE LIGHT" : labels[i], x + 8, ry, FONT_BIG, color);
		// (a resolution picked but not applied yet: in white)
		int pending = (i == MT_ROW_RESOLUTION) && (s_mtSet.pendingMode != MT_Settings_CurrentMode());
		DecalFont_DrawLine(value, x + MT_SET_PANEL_W - 8, ry, FONT_BIG, JUSTIFY_RIGHT | (pending ? WHITE : color));
	}
	RECT hl = {x + 3, y + titleH + s_mtSet.row * rowH + 1, MT_SET_PANEL_W - 6, rowH - 3};
	CTR_Box_DrawClearBox(&hl, &GAME_MENU_HIGHLIGHT, 1, ot, primMem);
	RECT line = {x + 3, y + titleH - 4, MT_SET_PANEL_W - 6, 2};
	Color border;
	memcpy(&border, &RECTMENU_BORDER_NORMAL, sizeof(border));
	RECTMENU_DrawOuterRect_Edge(&line, &border, 0x20, ot);
	RECT box = {x, y - 4, MT_SET_PANEL_W, h + 4};
	RECTMENU_DrawInnerRect(&box, 1, ot); // (solid: nothing behind shows through the text)
}

// a box over Settings: lines of small text, then (rows > 0) its choices in
// big text with the cursor on choice `selected`
static void MT_Settings_DrawMessage(struct GameTracker *gGT, char **lines, int numLines, char **choices, int numChoices, int selected)
{
	u32 *ot = gGT->backBuffer->otMem.uiOT;
	struct PrimMem *primMem = &gGT->backBuffer->primMem;
	int lineH = data.font_charPixHeight[FONT_SMALL] + 2, rowH = data.font_charPixHeight[FONT_BIG] + 3;
	int w = 0;
	for (int i = 0; i < numLines; i++)
	{
		int lw = DecalFont_GetLineWidth(lines[i], FONT_SMALL);
		w = (lw > w) ? lw : w;
	}
	w = ((w + 24) > 200) ? w + 24 : 200;
	int h = numLines * lineH + numChoices * rowH + 12;
	int x = 256 - w / 2, y = 108 - h / 2;
	for (int i = 0; i < numLines; i++)
	{
		DecalFont_DrawLine(lines[i], 256, y + 4 + i * lineH, FONT_SMALL, JUSTIFY_CENTER | (i == 0 ? ORANGE : WHITE));
	}
	int cy = y + 8 + numLines * lineH;
	for (int i = 0; i < numChoices; i++)
	{
		DecalFont_DrawLine(choices[i], 256, cy + i * rowH + 2, FONT_BIG, JUSTIFY_CENTER | ORANGE);
	}
	if (numChoices > 0)
	{
		RECT hl = {x + 3, cy + selected * rowH + 1, w - 6, rowH - 3};
		CTR_Box_DrawClearBox(&hl, &GAME_MENU_HIGHLIGHT, 1, ot, primMem);
	}
	RECT box = {x, y - 2, w, h + 4};
	RECTMENU_DrawInnerRect(&box, 1, ot);
}

static void MT_Settings_DrawConfirm(struct GameTracker *gGT)
{
	if (Rtx_Supported())
	{
		char *rebake[] = {"BAKE THE LIGHT AGAIN", "WITH RAY TRACING OFF EVERY LEVEL", "BAKES ITS LIGHT AGAIN", "THE NEXT TIME IT LOADS."};
		char *choices[] = {"NO", "YES"};
		MT_Settings_DrawMessage(gGT, rebake, 4, choices, 2, s_mtSet.confirmYes);
		return;
	}
	char *lines[] = {"BAKE RAY TRACED LIGHTING", "FOR EVERY LEVEL NOT BAKED YET.", "THIS CAN TAKE A LONG TIME",
	                 Rtx_Supported() ? "(ON THIS GPU: SOME MINUTES)" : "(NO RAY TRACING GPU: HOURS)", "BACK STOPS IT AFTER A LEVEL."};
	char *choices[] = {"NO", "YES"};
	MT_Settings_DrawMessage(gGT, lines, 5, choices, 2, s_mtSet.confirmYes);
}

static void MT_Settings_DrawBaking(struct GameTracker *gGT)
{
	int done, total;
	const char *name;
	char count[48], level[48];
	Rtx_BakeProgress(&done, &total, &name);
	snprintf(count, sizeof(count), "LEVEL %d OF %d", (done < total) ? done + 1 : total, total);
	snprintf(level, sizeof(level), "%s", name);
	for (char *c = level; *c; c++)
	{
		*c = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
	}
	char *lines[] = {"BAKING RAY TRACED LIGHTING", count, level, "BACK: STOP AFTER THIS LEVEL"};
	MT_Settings_DrawMessage(gGT, lines, 4, NULL, 0, 0);
}

// ---- input -----------------------------------------------------------------

static void MT_Settings_Change(int row, int dir)
{
	int w, h, fs;
	switch (row)
	{
	case MT_ROW_RESOLUTION:
		s_mtSet.pendingMode = (s_mtSet.pendingMode + dir + s_mtSet.numModes) % s_mtSet.numModes;
		break;
	case MT_ROW_FULLSCREEN:
		Platform_GetDisplay(&w, &h, &fs);
		Platform_SetDisplay(w, h, !fs);
		break;
	case MT_ROW_RTX:
		Rtx_SetEnabled(!Rtx_Enabled());
		break;
	case MT_ROW_SPP:
	{
		int spp = Rtx_GetSpp() + dir * MT_COG_SPP_STEP;
		Rtx_SetSpp((spp > MT_COG_SPP_MAX) ? MT_COG_SPP_MIN : ((spp < MT_COG_SPP_MIN) ? MT_COG_SPP_MAX : spp));
		break;
	}
	case MT_ROW_BAKED:
		Rtx_SetBakedEnabled(!Rtx_BakedEnabled());
		break;
	case MT_ROW_FPS:
		NativeSettings_SetInt("fps_counter", !NativeSettings_GetInt("fps_counter", 0));
		NativeSettings_Save();
		break;
	}
}

static void MT_Settings_PanelInput(int tap)
{
	if (tap & (BTN_TRIANGLE | BTN_SQUARE_one))
	{
		OtherFX_Play(2, 1);
		s_mtSet.focus = MT_SET_COG;
		NativeSettings_Save();
		return;
	}
	if (tap & (BTN_UP | BTN_DOWN))
	{
		s_mtSet.row = (s_mtSet.row + ((tap & BTN_UP) ? MT_ROW_COUNT - 1 : 1)) % MT_ROW_COUNT;
		s_mtSet.pendingMode = MT_Settings_CurrentMode(); // (an unapplied pick is dropped)
		OtherFX_Play(0, 1);
		return;
	}
	if (!(tap & (BTN_LEFT | BTN_RIGHT | BTN_CROSS_one | BTN_CIRCLE)))
		return;
	if (MT_Settings_RowLocked(s_mtSet.row))
	{
		OtherFX_Play(5, 1);
		return;
	}
	if ((s_mtSet.row == MT_ROW_RESOLUTION) && (tap & (BTN_CROSS_one | BTN_CIRCLE)))
	{
		int w, h, fs;
		Platform_GetDisplay(&w, &h, &fs);
		Platform_SetDisplay(s_mtSet.modeW[s_mtSet.pendingMode], s_mtSet.modeH[s_mtSet.pendingMode], fs);
		OtherFX_Play(1, 1);
		return;
	}
	if (s_mtSet.row == MT_ROW_BAKE_NOW)
	{
		if (tap & (BTN_CROSS_one | BTN_CIRCLE))
		{
			s_mtSet.confirmYes = 0; // (the warning starts on No)
			s_mtSet.focus = MT_SET_CONFIRM;
			OtherFX_Play(1, 1);
		}
		return;
	}
	MT_Settings_Change(s_mtSet.row, (tap & BTN_LEFT) ? -1 : 1);
	OtherFX_Play((tap & (BTN_CROSS_one | BTN_CIRCLE)) ? 1 : 0, 1);
}

static void MT_Settings_ConfirmInput(int tap)
{
	if (tap & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT))
	{
		s_mtSet.confirmYes ^= 1;
		OtherFX_Play(0, 1);
	}
	else if (tap & (BTN_TRIANGLE | BTN_SQUARE_one))
	{
		s_mtSet.focus = MT_SET_PANEL;
		OtherFX_Play(2, 1);
	}
	else if (tap & (BTN_CROSS_one | BTN_CIRCLE))
	{
		OtherFX_Play(1, 1);
		if (s_mtSet.confirmYes && Rtx_Supported())
		{
			Rtx_LightmapRebake();
			s_mtSet.bakeTotal = 1;
			s_mtSet.focus = MT_SET_PANEL;
		}
		else if (s_mtSet.confirmYes)
		{
			Rtx_BakeStart();
			s_mtSet.focus = Rtx_BakeRunning() ? MT_SET_BAKING : MT_SET_PANEL;
		}
		else
		{
			s_mtSet.focus = MT_SET_PANEL;
		}
	}
}

// Settings (or its warning, its progress) open: the menu underneath is
// neither updated nor drawn this frame, so Settings can be read
int MT_Settings_HidesMenu(void)
{
	return s_mtSet.focus >= MT_SET_PANEL;
}

// each main menu frame, before the menu takes its input (MainFrame_RenderFrame)
void MT_Settings_Tick(struct GameTracker *gGT)
{
	Rtx_BakeTick();
	if ((s_mtSet.focus == MT_SET_BAKING) && !Rtx_BakeRunning())
	{
		int done;
		const char *name;
		Rtx_BakeProgress(&done, &s_mtSet.bakeTotal, &name);
		s_mtSet.bakeTotal += (s_mtSet.bakeTotal == 0);
		s_mtSet.focus = MT_SET_PANEL;
	}

	int inMenu = (gGT->gameMode1 & MAIN_MENU) && (gGT->levelID == MAIN_MENU_LEVEL) && (sdata->ptrActiveMenu != NULL) &&
	             (sdata->Loading.stage == LOAD_IDLE) && !RaceFlag_IsTransitioning(); // (the flag is the menu's backdrop: only not while it moves)
	if (!inMenu)
	{
		if (Rtx_BakeRunning())
		{
			Rtx_BakeStop(); // (left the menu somehow: no baking behind a race)
		}
		s_mtSet.focus = MT_SET_NONE;
		return;
	}

	// test harness: CTR_TEST_BAKE=1 runs "Bake now" once the main menu is up
	static int s_testBake = -1;
	if (s_testBake < 0)
		s_testBake = getenv("CTR_TEST_BAKE") != NULL;
	if ((s_testBake == 1) && Rtx_Supported() && !Rtx_BakeRunning())
	{
		s_testBake = 2;
		Rtx_BakeStart();
		s_mtSet.focus = MT_SET_BAKING; // (its progress shown: elsewhere a bake stops)
	}
	// (the cog: on the main menu's top level only, once its menu is up)
	if ((s_mtSet.focus == MT_SET_NONE) && !MT_Settings_OnMainMenu())
		return;

	int tap = sdata->buttonTapPerPlayer[0];
	switch (s_mtSet.focus)
	{
	case MT_SET_NONE:
		if ((tap & BTN_LEFT) && MT_Settings_LeftReachesCog())
		{
			s_mtSet.focus = MT_SET_COG;
			OtherFX_Play(0, 1);
			RECTMENU_ClearInput();
		}
		break;
	case MT_SET_COG:
		if (tap & (BTN_CROSS_one | BTN_CIRCLE))
		{
			MT_Settings_ReadModes();
			s_mtSet.pendingMode = MT_Settings_CurrentMode();
			s_mtSet.row = 0;
			s_mtSet.focus = MT_SET_PANEL;
			OtherFX_Play(1, 1);
		}
		else if (tap & (BTN_RIGHT | BTN_TRIANGLE | BTN_SQUARE_one))
		{
			s_mtSet.focus = MT_SET_NONE;
			OtherFX_Play(tap & BTN_RIGHT ? 0 : 2, 1);
		}
		RECTMENU_ClearInput();
		break;
	case MT_SET_PANEL:
		MT_Settings_PanelInput(tap);
		RECTMENU_ClearInput();
		break;
	case MT_SET_CONFIRM:
		MT_Settings_ConfirmInput(tap);
		RECTMENU_ClearInput();
		break;
	case MT_SET_BAKING:
		if (tap & (BTN_TRIANGLE | BTN_SQUARE_one))
		{
			Rtx_BakeStop();
			OtherFX_Play(2, 1);
		}
		RECTMENU_ClearInput();
		break;
	}

	if (s_mtSet.focus != MT_SET_NONE)
	{
		gGT->demoCountdownTimer = TITLE_DEMO_IDLE_FRAMES; // (no demo race while in here)
	}
	if (s_mtSet.focus == MT_SET_CONFIRM)
	{
		MT_Settings_DrawConfirm(gGT);
	}
	else if (s_mtSet.focus == MT_SET_BAKING)
	{
		MT_Settings_DrawBaking(gGT);
	}
	if (s_mtSet.focus >= MT_SET_PANEL)
	{
		MT_Settings_DrawPanel(gGT);
	}
	MT_Settings_DrawCog(gGT);
}
