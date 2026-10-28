#include <common.h>
#include <multitrack.h>
#include "platform/native_assets.h"
#include "platform/native_renderer.h"
#include "platform/native_rtx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct MTState gMTState;

static struct MTHeader sEmptyTable = {MT_MAGIC, 0};
static struct MTHeader *sTable;

struct MTHeader *MT_Table(void)
{
	if (sTable != NULL)
	{
		return sTable;
	}

	sTable = &sEmptyTable;
	FILE *f = NativeAssets_OpenHost("track_table.bin", "rb");
	if (f == NULL)
	{
		printf("[MultiTrack] assets/track_table.bin missing: stock tracks only\n");
		return sTable;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	struct MTHeader *table = malloc(size);
	if ((size >= (long)sizeof(struct MTHeader)) && (fread(table, 1, size, f) == (size_t)size) && (table->magic == MT_MAGIC) &&
	    (sizeof(struct MTHeader) + table->count * sizeof(struct MTEntry) <= (size_t)size))
	{
		sTable = table;
		printf("[MultiTrack] %d custom tracks\n", table->count);
	}
	else
	{
		printf("[MultiTrack] assets/track_table.bin invalid: stock tracks only\n");
		free(table);
	}
	fclose(f);
	return sTable;
}

struct MTEntry *MT_ActiveEntry(void)
{
	struct GameTracker *gGT = sdata->gGT;
	struct MTHeader *table = MT_Table();

	if ((gMTState.magic != MT_MAGIC) || (gMTState.redirectLevel != (int)gGT->levelID))
	{
		return NULL;
	}

	// (the entry picked: Nitro-Fueled tracks over a stock slot all use that slot's
	// own bigfile entries, so redirectBase alone doesn't tell them apart)
	int picked = gMTState.entry - 1;
	if ((picked >= 0) && (picked < table->count) && (table->entries[picked].slot == gMTState.redirectLevel) &&
	    (table->entries[picked].bigBase == gMTState.redirectBase))
	{
		return &table->entries[picked];
	}
	for (int i = 0; i < table->count; i++)
	{
		if (table->entries[i].bigBase == gMTState.redirectBase)
		{
			return &table->entries[i];
		}
	}
	return NULL;
}

// an NF entry's asset key ("nf-<slot>", or the key pack_tracks.py put in the
// author field, lower case): 0 for any other entry
int MT_EntryKey(const struct MTEntry *e, char *key, size_t size)
{
	if ((e == NULL) || !(e->flags & MT_FLAG_REMASTER))
		return 0;
	if (!strncmp(e->author, "NF-", 3))
	{
		size_t i = 0;
		for (; (i + 1 < size) && (i < MT_AUTHOR_LEN) && e->author[i]; i++)
			key[i] = (char)((e->author[i] >= 'A') && (e->author[i] <= 'Z') ? e->author[i] + 32 : e->author[i]);
		key[i] = 0;
	}
	else
	{
		snprintf(key, size, "nf-%d", e->slot);
	}
	return 1;
}

int MT_ShotClean(void)
{
	static int s_clean = -1;
	if (s_clean < 0)
	{
		const char *v = getenv("CTR_SHOT_CLEAN");
		s_clean = (v != NULL) && (atoi(v) != 0);
	}
	return s_clean;
}

// once a frame (RenderAllHUD): with ray tracing off, while the level's light
// bakes (the game standing still), how far it is
void MT_LightBakeTick(void)
{
	int pct = Rtx_LightmapBakeProgress();
	if (pct < 0)
	{
		return;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "BAKING LIGHT %d%%", pct);
	DecalFont_DrawLine(buf, 256, 100, FONT_BIG, JUSTIFY_CENTER | ORANGE);
}

// once a frame (RenderAllHUD): the intro fly-by over (the race HUD would
// show), a few seconds for the traced image to settle, then the screenshot
void MT_ShotTick(void)
{
	// test harness: CTR_BOOT_POS2=x,y,z,angle,frame moves player 1 there at that
	// frame (after CTR_BOOT_POS: a hub's tunnel load trigger, then its swap)
	static int s_moveFrames;
	static const char *s_move2 = (const char *)1;
	if (s_move2 == (const char *)1)
	{
		s_move2 = getenv("CTR_BOOT_POS2");
	}
	int mx, my, mz, ma, mf;
	s_moveFrames++;
	if ((s_move2 != NULL) && (sdata->gGT->drivers[0] != NULL) &&
	    (sscanf(s_move2, "%d,%d,%d,%d,%d", &mx, &my, &mz, &ma, &mf) == 5) && (s_moveFrames == mf))
	{
		struct Driver *d = sdata->gGT->drivers[0];
		d->posCurr.x = d->posPrev.x = mx << 8;
		d->posCurr.y = d->posPrev.y = my << 8;
		d->posCurr.z = d->posPrev.z = mz << 8;
		d->angle = (s16)ma;
		d->rotCurr.y = (s16)ma;
		printf("[MultiTrack] moved to %d %d %d at frame %d\n", mx, my, mz, mf);
	}

	// test harness: CTR_TEST_HUBSWAP=hub,frame streams that adventure hub in at
	// that frame and swaps to it 60 frames later (what a tunnel's load and swap
	// triggers do)
	static const char *s_swap = (const char *)1;
	if (s_swap == (const char *)1)
	{
		s_swap = getenv("CTR_TEST_HUBSWAP");
	}
	int hub, hf;
	if ((s_swap != NULL) && (sscanf(s_swap, "%d,%d", &hub, &hf) == 2))
	{
		struct GameTracker *gGT = sdata->gGT;
		if (s_moveFrames == hf)
		{
			LOAD_Hub_ReadFile(sdata->ptrBigfile1, hub, LOAD_HUB_MEMPACK_PAIR_INDEX_SUM - gGT->activeMempackIndex);
			printf("[MultiTrack] test: loading hub %d\n", hub);
		}
		if (s_moveFrames == hf + 60)
		{
			gGT->bool_AdvHub_NeedToSwapLEV = 1;
			printf("[MultiTrack] test: swapping to hub %d\n", hub);
		}
	}

	// test harness: CTR_TEST_TRACK_HOP=a,b,c,... (table indices; after
	// CTR_BOOT_TRACK=a) plays each for CTR_TEST_TRACK_HOP_FRAMES (default 600)
	// frames, then loads the next one in the same process (level switches as the
	// track list makes them), then quits
	static const char *s_hop = (const char *)1;
	static int s_hopAt, s_hopFrames;
	if (s_hop == (const char *)1)
	{
		s_hop = getenv("CTR_TEST_TRACK_HOP");
		const char *f = getenv("CTR_TEST_TRACK_HOP_FRAMES");
		s_hopFrames = f ? atoi(f) : 600;
	}
	static int s_inLevel;
	if (sdata->Loading.stage != LOAD_IDLE)
	{
		s_inLevel = 0; // (a level loading: count from when it's done)
	}
	else if ((s_hop != NULL) && (sdata->gGT->level1 != NULL))
	{
		if (++s_inLevel == s_hopFrames)
		{
			// the next index in the list
			const char *p = s_hop;
			for (int k = 0; (k <= s_hopAt) && (p != NULL); k++)
			{
				p = strchr(p, ',');
				p = p ? p + 1 : NULL;
			}
			struct MTHeader *table = MT_Table();
			int n = p ? atoi(p) : -1;
			if ((n < 0) || (n >= table->count))
			{
				printf("[MultiTrack] track hop: done after %d tracks\n", s_hopAt + 1);
				fflush(stdout);
				exit(0);
			}
			s_hopAt++;
			gMTState.magic = MT_MAGIC;
			gMTState.redirectLevel = table->entries[n].slot;
			gMTState.redirectBase = table->entries[n].bigBase;
			gMTState.entry = (short)(n + 1);
			printf("[MultiTrack] track hop %d: table entry %d '%s' on slot %d\n", s_hopAt, n, table->entries[n].name, table->entries[n].slot);
			fflush(stdout);
			MainRaceTrack_RequestLoad(table->entries[n].slot);
			s_inLevel = 0;
		}
	}

	static int s_frames, s_settled;
	if (!MT_ShotClean())
	{
		return;
	}
	s_frames++;
	if ((sdata->gGT->hudFlags & HUD_FLAG_RACE_HUD) != 0)
	{
		s_settled++;
	}
	if ((s_settled == 150) || (s_frames == 1800))
	{
		NativeRenderer_RequestShot();
	}
}

// CTR_BOOT_LEVEL=<levelID> boots into that stock level instead of the intro;
// CTR_BOOT_TRACK=<n> boots into custom track n of the table (on its host slot);
// CTR_BOOT_TT=1 either of them in Time Trial
int MT_BootLevel(int defaultLevel)
{
	const char *track = getenv("CTR_BOOT_TRACK");
	const char *level = getenv("CTR_BOOT_LEVEL");
	if (((track != NULL) || (level != NULL)) && (getenv("CTR_BOOT_TT") != NULL))
	{
		sdata->gGT->gameMode1 |= TIME_TRIAL; // CTR_BOOT_TT=1: in Time Trial (its ghosts)
	}

	if (track != NULL)
	{
		struct MTHeader *table = MT_Table();
		int n = atoi(track);
		if ((n >= 0) && (n < table->count))
		{
			gMTState.magic = MT_MAGIC;
			gMTState.redirectLevel = table->entries[n].slot;
			gMTState.redirectBase = table->entries[n].bigBase;
			gMTState.entry = (short)(n + 1);
			printf("[MultiTrack] boot: custom track %d '%s' on slot %d\n", n, table->entries[n].name, table->entries[n].slot);
			return table->entries[n].slot;
		}
		printf("[MultiTrack] CTR_BOOT_TRACK=%s out of range\n", track);
	}

	if (level != NULL)
	{
		// CTR_BOOT_PLAYERS=<n>: split screen; CTR_BOOT_BATTLE=1: in battle mode
		const char *players = getenv("CTR_BOOT_PLAYERS");
		if (players != NULL)
		{
			int n = atoi(players);
			sdata->gGT->numPlyrNextGame = (n < 1) ? 1 : ((n > 4) ? 4 : n);
		}
		if ((getenv("CTR_BOOT_BATTLE") != NULL) && (atoi(level) >= NITRO_COURT) && (atoi(level) <= LAB_BASEMENT))
		{
			sdata->gGT->gameMode1 |= BATTLE_MODE;
		}
		printf("[MultiTrack] boot: level %d, %d players%s\n", atoi(level), sdata->gGT->numPlyrNextGame,
		       (sdata->gGT->gameMode1 & BATTLE_MODE) ? ", battle" : "");
		return atoi(level);
	}

	return defaultLevel;
}
