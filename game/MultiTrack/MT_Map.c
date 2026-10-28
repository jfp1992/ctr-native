// MultiTrack: Nitro-Fueled tracks' minimaps. scripts/nf_minimap.py draws each
// NF track's surface from above (assets/track_maps/<key>.png, a white border
// round it) and its world range (<key>.map: s16 x0, x1, z0, z1, w, h). The race
// HUD draws it where the stock minimap goes, with a UIMap made to match it for
// the karts' dots; the track select panel draws it in the map box.

#define MT_MAP_CACHE 48
#define MT_MAP_RACE_X 500 // (UI_RenderFrame: the map's bottom right, 1 player)
#define MT_MAP_RACE_Y 195

static struct
{
	char key[32];
	int tried;
	struct MTMap map;
} s_mtMaps[MT_MAP_CACHE];

static struct UIMapSpawnMetadata s_mtRaceMap;

const struct MTMap *MT_Map_Find(const struct MTEntry *e)
{
	char key[32];
	if (!MT_EntryKey(e, key, sizeof(key)))
		return NULL;
	int slot = -1;
	for (int i = 0; i < MT_MAP_CACHE; i++)
	{
		if (s_mtMaps[i].tried && !strcmp(s_mtMaps[i].key, key))
			return s_mtMaps[i].map.tex ? &s_mtMaps[i].map : NULL;
		if ((slot < 0) && !s_mtMaps[i].tried)
			slot = i;
	}
	if (slot < 0)
		return NULL;

	// (looked for once a session: none stays none)
	s_mtMaps[slot].tried = 1;
	snprintf(s_mtMaps[slot].key, sizeof(s_mtMaps[slot].key), "%s", key);
	char rel[64], path[1024];
	snprintf(rel, sizeof(rel), "track_maps/%s.map", key);
	s16 m[6];
	FILE *f = NativeAssets_OpenHost(rel, "rb");
	int ok = (f != NULL) && (fread(m, sizeof(m), 1, f) == 1) && (m[1] != m[0]) && (m[3] != m[2]) && (m[4] > 0) && (m[5] > 1);
	if (f != NULL)
		fclose(f);
	snprintf(rel, sizeof(rel), "track_maps/%s.png", key);
	int w = 0, h = 0;
	TextureID tex = (ok && NativeAssets_BuildPath(rel, path, sizeof(path))) ? NativeRenderer_LoadImageTexture(path, &w, &h) : 0;
	if (tex == 0)
		return NULL;
	struct MTMap *map = &s_mtMaps[slot].map;
	map->tex = tex;
	map->x0 = m[0];
	map->x1 = m[1];
	map->z0 = m[2];
	map->z1 = m[3];
	map->w = m[4];
	map->h = m[5];
	return map;
}

// the karts' dots on it: UI_Map_GetIconPos, mode 0 (screen x from world x,
// screen y from world z, north up), its origin at world 0
struct UIMap *MT_Map_RaceMap(void)
{
	const struct MTMap *m = MT_Map_Find(MT_ActiveEntry());
	if (m == NULL)
		return NULL;
	struct UIMap *u = &s_mtRaceMap.map;
	int left = MT_MAP_RACE_X - m->w, top = MT_MAP_RACE_Y - m->h;
	u->worldStartX = m->x0;
	u->worldEndX = m->x1;
	u->worldStartY = m->z1; // (z decreasing down the image)
	u->worldEndY = m->z0;
	u->iconSizeX = m->w;
	u->iconSizeY = m->h / 2; // (GetIconPos doubles it)
	u->iconStartX = (s16)(left - (s32)m->x0 * m->w / (m->x1 - m->x0));
	u->iconStartY = (s16)(top + (s32)m->z1 * m->h / (m->z1 - m->z0) + UI_MAP_ICON_Y_OFFSET);
	u->mode = UI_MAP_MODE_0_DEGREES;
	s_mtRaceMap.topHalfMode = 0;
	return u;
}

int MT_Map_DrawRace(int posX, int posY)
{
	const struct MTMap *m = MT_Map_Find(MT_ActiveEntry());
	if (m == NULL)
		return 0;
	struct GameTracker *gGT = sdata->gGT;
	MT_DrawPicture(m->tex, posX - m->w, posY - m->h, m->w, m->h, gGT->pushBuffer_UI.ptrOT, &gGT->backBuffer->primMem);
	return 1;
}
