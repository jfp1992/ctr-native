// MultiTrack: Nitro-Fueled collision. On an NF track the karts (and everything
// else the game collides: bots, camera, weapons, respawns) hit the Nitro-Fueled
// level's own triangles instead of the stock level's quadblocks; the stock
// level is still loaded and drawn (GL) as before, with its start grid, restart
// path, AI paths and instances.
//
// assets/rtx_remaster/<key>.coll (scripts/nf_collision.py) holds the mesh in
// the game's own structures, no PS1 limits:
//   * every NF triangle is a QuadBlock of kind 1: tested as that one exact
//     triangle (index 0,1,2; normal scale in triNormalVecDividend[LO_0] for the
//     full triangle) instead of the 4 hi-LOD sub-triangles of a stock quadblock;
//     kind 0 = a stock quadblock kept as it was (water, triggers, invisible walls)
//   * each quadblock's vertex indices are relative to its own base (a BSP leaf's
//     vertices), so u16 indices don't cap the vertex count
//   * BSP child indices are 32 bits (childID[0..1] low, [2..3] high halves,
//     leaf = the child's own BSP_NODE_FLAG_LEAF), searched here, not on the
//     scratchpad stack, so the node count isn't capped either
//   * version 2: the PS1 start grid's and restart points' heights on the NF
//     ground (the level's own are on the PS1 floor), set at level load
//   * version 3, a native NF track (no PS1 level of its own, played over a
//     stock host slot, scripts/nf_race.py): its own start grid, restart path
//     and AI nav paths, the host's crates and fruit moved onto it, every other
//     host instance out of the world; the host's minimap, fly-in / end of race
//     cameras, ghosts and hazards switched off (they belong to its own track)
// COLL_SearchBSP_CallbackPARAM swaps the level's BSP root for this one and the
// vertex loader asks MT_Coll_QuadVerts for each quadblock's vertices.
// CTR_NF_COLL=0: stock collision.

struct MTCollHeader
{
	char magic[4]; // "RTXC"
	u32 version;   // 1, 2 or 3
	u32 numVerts, numQuads, numNodes, numHitboxBytes;
	u32 offVerts, offQuads, offQuadBase, offQuadKind, offNodes, offHitbox;
	u32 levBodySize; // the stock level's body this was made for (hitbox InstDef offsets)
	// version 2: s16 heights of the start grid (8) and the restart points
	u32 numSpawnY, offSpawnY, numRestartY, offRestartY;
	// version 3: the race section of a native track (0 bytes: none)
	u32 raceSize, offRace;
};

static struct
{
	struct Level *level; // loaded for
	u8 *data;
	struct LevVertex *verts;
	struct QuadBlock *quads;
	u32 *quadBase;
	u8 *quadKind;
	struct BSP *nodes;
	u32 numQuads, numNodes;
	struct BSP *levelRoot; // the stock root it replaces
	struct BSP **stack;
	u32 stackSize;
	struct NavHeader *navTable[3]; // (a native track's Level.LevNavTable)
} s_mtColl;

// a native track's end of race cameras: none (ST1_CAMERA_EOR: an empty list)
static s16 s_mtNoEorCameras[2];

// a level's collision memory outlives it by two level loads: the game keeps
// pointers into the old level for a while (the camera's quadblock, which
// RainLogic reads through the next load; on the PS1 that memory just stays
// readable until overwritten)
static void *s_mtCollRetired[2][2];

static void MT_Coll_Free(void)
{
	free(s_mtCollRetired[1][0]);
	free(s_mtCollRetired[1][1]);
	s_mtCollRetired[1][0] = s_mtCollRetired[0][0];
	s_mtCollRetired[1][1] = s_mtCollRetired[0][1];
	s_mtCollRetired[0][0] = s_mtColl.data;
	s_mtCollRetired[0][1] = s_mtColl.stack;
	memset(&s_mtColl, 0, sizeof(s_mtColl));
}

// the renderer's key of the NF track being played ("nf-<slot>", or the key
// pack_tracks.py put in the author field), or 0
static int MT_Coll_Key(char *key, size_t size)
{
	struct MTEntry *e = MT_ActiveEntry();
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

// a native track's race section (scripts/nf_collision.py race_bytes, each part
// 4 aligned): u32 8, s16 DriverSpawn[8][6]; u32 n, CheckpointNode[n]; u32 paths,
// per path u32 frames, NavHeader, NavFrame[frames]; u32 moves, per move u32
// InstDef offset in the level body, s16 pos[3], s16 0. Checked whole before
// anything is changed; 0 if unusable.
static int MT_Coll_ApplyRace(struct Level *lev, u8 *race, u32 size, u32 bodySize)
{
	u32 at = 0, n;
#define MT_RACE_NEED(bytes)                      \
	if ((u64)at + (u64)(bytes) > (u64)size)      \
		return 0;
#define MT_RACE_U32(var)                         \
	MT_RACE_NEED(4);                             \
	memcpy(&(var), race + at, 4);                \
	at += 4;
#define MT_RACE_ALIGN() at = (at + 3u) & ~3u

	MT_RACE_U32(n);
	MT_RACE_NEED(8 * 12);
	if (n != 8)
		return 0;
	s16 *spawn = (s16 *)(race + at);
	at += 8 * 12;
	MT_RACE_ALIGN();

	u32 numNodes;
	MT_RACE_U32(numNodes);
	MT_RACE_NEED(numNodes * sizeof(struct CheckpointNode));
	if ((numNodes < 2) || (numNodes > 254))
		return 0;
	struct CheckpointNode *nodes = (struct CheckpointNode *)(race + at);
	at += numNodes * sizeof(struct CheckpointNode);
	MT_RACE_ALIGN();
	for (u32 i = 0; i < numNodes; i++)
	{
		u8 link[4] = {nodes[i].nextIndex_forward, nodes[i].nextIndex_left, nodes[i].nextIndex_backward, nodes[i].nextIndex_right};
		for (int k = 0; k < 4; k++)
		{
			if ((link[k] != 0xFF) && (link[k] >= numNodes))
				return 0;
		}
	}

	u32 numPaths;
	MT_RACE_U32(numPaths);
	if (numPaths > 3)
		return 0;
	struct NavHeader *paths[3] = {NULL, NULL, NULL};
	for (u32 p = 0; p < numPaths; p++)
	{
		u32 frames;
		MT_RACE_U32(frames);
		MT_RACE_NEED(sizeof(struct NavHeader) + frames * sizeof(struct NavFrame));
		paths[p] = (struct NavHeader *)(race + at);
		if (((u16)paths[p]->magicNumber != 0xECFDu) || (paths[p]->numPoints != (s16)frames) || (frames < 2) || (frames > 0x3FF))
			return 0;
		struct NavFrame *f = NAVHEADER_GETFRAME(paths[p]);
		for (u32 i = 0; i < frames; i++)
		{
			if ((f[i].goBackCount >= numNodes) || (f[i].distToNextNavXYZ <= 0) || (f[i].distToNextNavXZ <= 0))
				return 0;
		}
		at += sizeof(struct NavHeader) + frames * sizeof(struct NavFrame);
		MT_RACE_ALIGN();
	}

	u32 numMoves;
	MT_RACE_U32(numMoves);
	MT_RACE_NEED(numMoves * 12);
	u8 *moves = race + at;
	for (u32 i = 0; i < numMoves; i++)
	{
		u32 off;
		memcpy(&off, moves + i * 12, 4);
		if ((u64)off + sizeof(struct InstDef) > bodySize)
			return 0;
	}
#undef MT_RACE_NEED
#undef MT_RACE_U32
#undef MT_RACE_ALIGN

	for (int i = 0; i < 8; i++)
	{
		lev->DriverSpawn[i].pos.x = spawn[i * 6 + 0];
		lev->DriverSpawn[i].pos.y = spawn[i * 6 + 1];
		lev->DriverSpawn[i].pos.z = spawn[i * 6 + 2];
		lev->DriverSpawn[i].rot.x = spawn[i * 6 + 3];
		lev->DriverSpawn[i].rot.y = spawn[i * 6 + 4];
		lev->DriverSpawn[i].rot.z = spawn[i * 6 + 5];
	}
	lev->cnt_restart_points = (int)numNodes;
	lev->ptr_restart_points = nodes;
	for (int p = 0; p < 3; p++)
	{
		s_mtColl.navTable[p] = paths[p];
	}
	lev->LevNavTable = s_mtColl.navTable;
	for (u32 i = 0; i < numMoves; i++)
	{
		u32 off;
		s16 pos[3];
		memcpy(&off, moves + i * 12, 4);
		memcpy(pos, moves + i * 12 + 4, 6);
		struct InstDef *d = (struct InstDef *)((u8 *)lev + off);
		d->pos.x = pos[0];
		d->pos.y = pos[1];
		d->pos.z = pos[2];
	}
	// the host's own track's extras: minimap, fly-in path (CAM.c skips the
	// fly-in without one), end of race cameras (an empty list), ghosts, hazards
	if (lev->ptrSpawnType1 != NULL)
	{
		void **st1 = ST1_GETPOINTERS(lev->ptrSpawnType1);
		int count = lev->ptrSpawnType1->count;
		if (count > ST1_MAP)
			st1[ST1_MAP] = NULL;
		if (count > ST1_CAMERA_EOR)
			st1[ST1_CAMERA_EOR] = s_mtNoEorCameras;
		if (count > ST1_CAMERA_PATH)
			st1[ST1_CAMERA_PATH] = NULL;
		if (count > ST1_NTROPY)
			st1[ST1_NTROPY] = NULL;
		if (count > ST1_NOXIDE)
			st1[ST1_NOXIDE] = NULL;
	}
	lev->numSpawnType2 = 0;
	lev->numSpawnType2_PosRot = 0;
	printf("[MultiTrack] native NF track: %u restart points (track length %u), nav paths %d / %d / %d frames, %u instances moved\n", numNodes,
	       (u32)nodes[0].distToFinish * 8, paths[0] ? paths[0]->numPoints : 0, paths[1] ? paths[1]->numPoints : 0, paths[2] ? paths[2]->numPoints : 0,
	       numMoves);
	return 1;
}

// each level load (LOAD_TenStages): the NF collision of the track, if it has one
void MT_Coll_OnLevelLoaded(struct Level *lev)
{
	MT_Coll_Free();
	char key[32], rel[64];
	const char *env = getenv("CTR_NF_COLL");
	if ((lev == NULL) || (lev->ptr_mesh_info == NULL) || ((env != NULL) && (atoi(env) == 0)) || !MT_Coll_Key(key, sizeof(key)))
		return;
	snprintf(rel, sizeof(rel), "rtx_remaster/%s.coll", key);
	FILE *f = NativeAssets_OpenHost(rel, "rb");
	if (f == NULL)
		return;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *data = malloc(size);
	int ok = (data != NULL) && (size >= (long)offsetof(struct MTCollHeader, numSpawnY)) && (fread(data, 1, size, f) == (size_t)size);
	fclose(f);
	struct MTCollHeader *h = (struct MTCollHeader *)data;
	ok = ok && !memcmp(h->magic, "RTXC", 4) &&
	     ((h->version == 1) || ((h->version == 2) && (size >= (long)offsetof(struct MTCollHeader, raceSize))) ||
	      ((h->version == 3) && (size >= (long)sizeof(struct MTCollHeader)))) &&
	     (h->numNodes > 0) &&
	     ((u64)h->offVerts + (u64)h->numVerts * sizeof(struct LevVertex) <= (u64)size) &&
	     ((u64)h->offQuads + (u64)h->numQuads * sizeof(struct QuadBlock) <= (u64)size) && ((u64)h->offQuadBase + (u64)h->numQuads * 4 <= (u64)size) &&
	     ((u64)h->offQuadKind + h->numQuads <= (u64)size) && ((u64)h->offNodes + (u64)h->numNodes * sizeof(struct BSP) <= (u64)size) &&
	     ((u64)h->offHitbox + h->numHitboxBytes <= (u64)size) &&
	     ((h->version < 2) || (((u64)h->offSpawnY + (u64)h->numSpawnY * 2 <= (u64)size) && ((u64)h->offRestartY + (u64)h->numRestartY * 2 <= (u64)size))) &&
	     ((h->version < 3) || ((u64)h->offRace + (u64)h->raceSize <= (u64)size));
	if (!ok)
	{
		printf("[MultiTrack] %s: not a usable NF collision file\n", rel);
		free(data);
		return;
	}
	s_mtColl.data = data;
	s_mtColl.verts = (struct LevVertex *)(data + h->offVerts);
	s_mtColl.quads = (struct QuadBlock *)(data + h->offQuads);
	s_mtColl.quadBase = (u32 *)(data + h->offQuadBase);
	s_mtColl.quadKind = data + h->offQuadKind;
	s_mtColl.nodes = (struct BSP *)(data + h->offNodes);
	s_mtColl.numQuads = h->numQuads;
	s_mtColl.numNodes = h->numNodes;

	// relocate: leaves' quad ranges and hitbox lists, hitboxes' InstDefs (offsets
	// into the stock level, + 1; 0 = none)
	int bad = 0;
	for (u32 i = 0; i < h->numNodes; i++)
	{
		struct BSP *n = &s_mtColl.nodes[i];
		if (n->flag & BSP_NODE_FLAG_LEAF)
		{
			u32 first = (u32)(uintptr_t)n->data.leaf.ptrQuadBlockArray;
			u32 hb = (u32)(uintptr_t)n->data.leaf.bspHitboxArray;
			bad |= (first + (u32)n->data.leaf.numQuads > h->numQuads) || (hb > h->numHitboxBytes);
			n->data.leaf.ptrQuadBlockArray = &s_mtColl.quads[(first < h->numQuads) ? first : 0];
			n->data.leaf.bspHitboxArray = hb ? (struct BSP *)(data + h->offHitbox + hb - 1) : NULL;
		}
		else
		{
			for (int c = 0; c < 2; c++)
			{
				u32 child = (u16)n->data.branch.childID[c] | ((u32)(u16)n->data.branch.childID[2 + c] << 16);
				bad |= (child != 0xFFFFFFFFu) && (child >= h->numNodes);
			}
		}
	}
	for (u32 o = 0; o + sizeof(struct BSP) <= h->numHitboxBytes; o += sizeof(struct BSP))
	{
		struct BSP *hb = (struct BSP *)(data + h->offHitbox + o);
		u32 inst = (u32)(uintptr_t)hb->data.hitbox.instDef;
		bad |= (inst > h->levBodySize);
		hb->data.hitbox.instDef = inst ? (struct InstDef *)((u8 *)lev + inst - 1) : NULL;
	}
	for (u32 q = 0; q < h->numQuads; q++)
	{
		bad |= (s_mtColl.quadBase[q] >= h->numVerts);
	}
	if (bad)
	{
		printf("[MultiTrack] %s: indices out of range\n", rel);
		MT_Coll_Free();
		return;
	}
	// the start grid and restart points onto the NF ground (only if made for
	// this level's restart path)
	if (h->version >= 2)
	{
		s16 *spawnY = (s16 *)(data + h->offSpawnY);
		s16 *restartY = (s16 *)(data + h->offRestartY);
		for (u32 i = 0; (i < h->numSpawnY) && (i < 8); i++)
		{
			lev->DriverSpawn[i].pos.y = spawnY[i];
		}
		if ((lev->ptr_restart_points != NULL) && ((u32)lev->cnt_restart_points == h->numRestartY))
		{
			for (u32 i = 0; i < h->numRestartY; i++)
			{
				lev->ptr_restart_points[i].pos.y = restartY[i];
			}
		}
	}

	if ((h->version >= 3) && (h->raceSize > 0) && !MT_Coll_ApplyRace(lev, data + h->offRace, h->raceSize, h->levBodySize))
	{
		printf("[MultiTrack] %s: not a usable race section\n", rel);
		MT_Coll_Free();
		return;
	}

	s_mtColl.stackSize = 4096;
	s_mtColl.stack = malloc(s_mtColl.stackSize * sizeof(struct BSP *));
	s_mtColl.level = lev;
	s_mtColl.levelRoot = lev->ptr_mesh_info->bspRoot;
	printf("[MultiTrack] Nitro-Fueled collision %s: %u triangles / quadblocks, %u vertices, %u BSP nodes (%.0f MB)\n", rel, h->numQuads, h->numVerts,
	       h->numNodes, size / 1e6);
}

static int MT_Coll_Active(void)
{
	struct GameTracker *gGT = sdata->gGT;
	return (s_mtColl.data != NULL) && (gGT->level1 == s_mtColl.level) && (gGT->level1 != NULL) && (gGT->level1->ptr_mesh_info != NULL) &&
	       (gGT->level1->ptr_mesh_info->bspRoot == s_mtColl.levelRoot);
}

// whether the karts collide with a Nitro-Fueled collision mesh now
int MT_Coll_On(void)
{
	return MT_Coll_Active();
}

// the BSP collision searches: the NF one in place of the level's own
struct BSP *MT_Coll_Root(struct BSP *root)
{
	return ((root != NULL) && (root == s_mtColl.levelRoot) && MT_Coll_Active()) ? s_mtColl.nodes : root;
}

// COLL_SearchBSP_CallbackPARAM for the NF BSP (32-bit children, host stack);
// 0 for any other root
int MT_Coll_SearchWide(struct BSP *root, struct BoundingBox *bbox, void (*callback)(struct BSP *, struct ScratchpadStruct *), struct ScratchpadStruct *sps)
{
	if ((root == NULL) || (root != s_mtColl.nodes) || (s_mtColl.nodes == NULL))
		return 0;
	struct BoundingBox b = *bbox; // (callbacks may change sps->bbox)
	u32 top = 0;
	s_mtColl.stack[top++] = root;
	while (top > 0)
	{
		struct BSP *n = s_mtColl.stack[--top];
		if ((n->box.min.x > b.max.x) || (n->box.min.y > b.max.y) || (n->box.min.z > b.max.z) || (b.min.x > n->box.max.x) ||
		    (b.min.y > n->box.max.y) || (b.min.z > n->box.max.z))
			continue;
		if (n->flag & BSP_NODE_FLAG_LEAF)
		{
			if (n->data.leaf.numQuads > 0)
				callback(n, sps);
			continue;
		}
		for (int c = 1; c >= 0; c--)
		{
			u32 child = (u16)n->data.branch.childID[c] | ((u32)(u16)n->data.branch.childID[2 + c] << 16);
			if ((child == 0xFFFFFFFFu) || (top >= s_mtColl.stackSize))
				continue;
			s_mtColl.stack[top++] = &s_mtColl.nodes[child];
		}
	}
	return 1;
}

static int MT_Coll_QuadIndex(const struct QuadBlock *quad)
{
	if ((s_mtColl.quads == NULL) || (quad < s_mtColl.quads) || (quad >= s_mtColl.quads + s_mtColl.numQuads))
		return -1;
	return (int)(quad - s_mtColl.quads);
}

// the vertices a quadblock's indices count from
struct LevVertex *MT_Coll_QuadVerts(struct LevVertex *levelVerts, const struct QuadBlock *quad)
{
	int q = MT_Coll_QuadIndex(quad);
	return (q < 0) ? levelVerts : &s_mtColl.verts[s_mtColl.quadBase[q]];
}

// one exact Nitro-Fueled triangle (not a stock quadblock's 4 or 8)
int MT_Coll_IsTriangle(const struct QuadBlock *quad)
{
	int q = MT_Coll_QuadIndex(quad);
	return (q >= 0) && (s_mtColl.quadKind[q] == 1);
}
