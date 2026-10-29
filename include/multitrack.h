#ifndef MULTITRACK_H
#define MULTITRACK_H

/*
 * MultiTrack (native): every custom track of the CTR-MultiTrack ROM.
 * Track data is appended to BIGFILE.BIG by scripts/pack_tracks.py; each
 * track owns 8 entries laid out like a stock level slot. The track table
 * (MTHeader + MTEntry[count]) is assets/track_table.bin, the same bytes the
 * PS1 build compiles onto the end of overlay 230. Selecting a custom track
 * makes LOAD_GetBigfileIndex redirect its host slot to those entries.
 * Port of psx-modding-toolchain/games/CTR/mods/MultiTrack.
 */

#define MT_MAGIC 0x4B52544D // 'MTRK'

#define MT_NAME_LEN   28
#define MT_AUTHOR_LEN 16

enum MTFlags
{
	MT_FLAG_BATTLE = 1,
	MT_FLAG_NO_AI = 2, // lev has no nav paths: arcade bots would have nowhere to drive
	MT_FLAG_REMASTER = 4, // Nitro-Fueled track: the stock level, drawn by the native ray tracer from assets/rtx_remaster
};

struct MTEntry
{
	u8 slot;    // host levelID
	u8 flags;   // MTFlags
	u16 bigBase; // first of this track's 8 bigfile entries
	char name[MT_NAME_LEN];
	char author[MT_AUTHOR_LEN];
};

struct MTHeader
{
	int magic;
	int count;
	struct MTEntry entries[0];
};

struct MTState
{
	int magic;         // MT_MAGIC once the menu has initialised this struct
	int redirectLevel; // host levelID to redirect, -1 = none
	int redirectBase;  // bigfile index replacing levelID*8
	short cursor[2][3]; // list cursor for race / battle lists, per track set (MT_TrackSelect.c)
	short set[2];       // track set shown for race / battle: 0 stock, 1 Nitro-Fueled, 2 custom
	short entry;        // table index + 1 of the redirected track (0: find it by redirectBase; NF tracks on a stock slot share its entries)
};

extern struct MTState gMTState;
#define MT_STATE (&gMTState)

// track table, NULL-safe: count 0 when assets/track_table.bin is missing
struct MTHeader *MT_Table(void);

// custom track currently loaded (redirect active for gGT->levelID), or NULL
struct MTEntry *MT_ActiveEntry(void);
int MT_EntryKey(const struct MTEntry *e, char *key, size_t size); // an NF entry's asset key (nf-<slot> / nf-<id>), 0: not NF
void MT_Coll_Tick(void);                                          // each frame: karts through an NF track's portals

// NF minimaps (MT_Map.c, assets/track_maps/<key>.png + .map: scripts/nf_minimap.py)
struct MTMap
{
	unsigned int tex; // TextureID
	short x0, x1, z0, z1, w, h; // world range the image spans (PS1), its size in HUD pixels
};
const struct MTMap *MT_Map_Find(const struct MTEntry *e); // NULL: none
struct UIMap *MT_Map_RaceMap(void);                       // the race's NF minimap for the karts' dots, NULL: none
int MT_Map_DrawRace(int posX, int posY);                  // drew the NF minimap (bottom-right at posX, posY)

// CTR_BOOT_LEVEL / CTR_BOOT_TRACK env: boot straight into a level (debug, screenshots)
int MT_BootLevel(int defaultLevel);

// CTR_SHOT_CLEAN=1 (scripts/track_shots.py): no HUD (nor its models), no karts,
// no particles; the frame is
// written once the race-start camera has settled
int MT_ShotClean(void);
void MT_ShotTick(void);
void MT_LightBakeTick(void);
void MT_BakeTour_Start(int again); // Settings "Bake now": every track's light (again: the baked ones too)
int MT_BakeTour_Active(void);
struct GameTracker;
void MT_Settings_Tick(struct GameTracker *gGT); // main menu Settings (MT_Settings.c), before the menu's input
int MT_Settings_HidesMenu(void);                // Settings open: no menu under it this frame

// Nitro-Fueled collision (MT_NfColl.c): NF tracks collide with the NF level's triangles
struct Level;
struct BSP;
struct BoundingBox;
struct ScratchpadStruct;
struct QuadBlock;
struct LevVertex;
void MT_Coll_OnLevelLoaded(struct Level *lev);
struct BSP *MT_Coll_Root(struct BSP *root);
int MT_Coll_SearchWide(struct BSP *root, struct BoundingBox *bbox, void (*callback)(struct BSP *, struct ScratchpadStruct *), struct ScratchpadStruct *sps);
struct LevVertex *MT_Coll_QuadVerts(struct LevVertex *levelVerts, const struct QuadBlock *quad);
int MT_Coll_IsTriangle(const struct QuadBlock *quad);
int MT_Coll_On(void); // karts collide with an NF collision mesh (no kart seating then)

#endif
