// MultiTrack: baked ray traced lighting for the GL renderer (players with ray
// tracing off, or a GPU without it). Settings "Bake now" bakes every level once
// into the vertex colours the PS1 path draws with (LevVertex.color_hi): a still
// snapshot of the traced light (what moves, animated screens and glows, keeps
// the art's colours). On a ray tracing GPU the renderer does it (ctr_rtx
// bakeVertexLight: its own fitted sun, sky and lights, 64 rays a term);
// otherwise tools/rt_bake/rt_bake.c, built as ctr_bake, traces it on the CPU
// (scripts/rt_bake.py's lighting: a fixed sun, or ambient occlusion indoors).
//
// Each level's bake is cached as rtbake/<hash>.bin (base directory), the hash
// taken over its vertex positions: the same level is found again whichever
// track entry, mode or LOD loads it, and is baked only once. Included after
// native_rtx.c (uses its disc and upload helpers).

#include "platform/native_settings.h"

#define BAKE_DIR      "rtbake"
#define BAKE_MAGIC    0x42525443u // 'CTRB'
#define BAKE_VERSION  1u
#define BAKE_RAYS     64u
#define BAKE_MAX_JOBS 1024
#define BAKE_RECORDS  4

enum
{
	BAKE_METHOD_GPU = 1,
	BAKE_METHOD_CPU = 2,
};

struct BakeFileHeader
{
	u32 magic;
	u32 version;
	u32 method;
	u32 numVerts;
	u64 hash;
};

static u64 Bake_MeshHash(const struct mesh_info *mesh)
{
	u64 h = 0xcbf29ce484222325ull;
	const u8 *p = (const u8 *)&mesh->numVertex;
	for (int i = 0; i < 4; i++)
	{
		h = (h ^ p[i]) * 0x100000001b3ull;
	}
	for (int v = 0; v < mesh->numVertex; v++)
	{
		const u8 *q = (const u8 *)&mesh->ptrVertexArray[v].pos;
		for (int i = 0; i < 6; i++)
		{
			h = (h ^ q[i]) * 0x100000001b3ull;
		}
	}
	return h;
}

static void Bake_Path(u64 hash, char *path, int size)
{
	snprintf(path, size, BAKE_DIR "/%016llx.bin", (unsigned long long)hash);
}

// the cached colours of a level (RGB per vertex), NULL if it has none
static u8 *Bake_Load(u64 hash, int numVerts)
{
	char path[64];
	Bake_Path(hash, path, sizeof(path));
	FILE *f = fopen(path, "rb");
	if (f == NULL)
	{
		return NULL;
	}
	struct BakeFileHeader hd;
	u8 *rgb = NULL;
	if ((fread(&hd, sizeof(hd), 1, f) == 1) && (hd.magic == BAKE_MAGIC) && (hd.version == BAKE_VERSION) && (hd.hash == hash) &&
	    (hd.numVerts == (u32)numVerts))
	{
		rgb = malloc((size_t)numVerts * 3 + 1);
		if (fread(rgb, 3, numVerts, f) != (size_t)numVerts)
		{
			free(rgb);
			rgb = NULL;
		}
	}
	fclose(f);
	return rgb;
}

static int Bake_Exists(u64 hash, int numVerts)
{
	u8 *rgb = Bake_Load(hash, numVerts);
	free(rgb);
	return rgb != NULL;
}

static void Bake_Save(u64 hash, int numVerts, int method, const u8 *rgb)
{
	mkdir(BAKE_DIR, 0755);
	char path[64], tmp[72];
	Bake_Path(hash, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (f == NULL)
	{
		printf("[Bake] could not write %s\n", path);
		return;
	}
	struct BakeFileHeader hd = {BAKE_MAGIC, BAKE_VERSION, (u32)method, (u32)numVerts, hash};
	int ok = (fwrite(&hd, sizeof(hd), 1, f) == 1) && (fwrite(rgb, 3, numVerts, f) == (size_t)numVerts);
	ok &= fclose(f) == 0;
	if (ok)
	{
		rename(tmp, path);
	}
	else
	{
		remove(tmp);
	}
}

// ---- drawing with it ---------------------------------------------------------
// Each loaded level (level1, and the hub loaded beside it in level2) gets its
// baked colours written over color_hi while the GL renderer draws it, and the
// art's own back when ray tracing takes over (the renderer fits its lighting
// to the art: Rtx_AppendLevelMesh reads color_hi)

static struct
{
	const struct Level *level;
	const struct mesh_info *mesh;
	u32 fingerprint;
	int numVerts;
	u8 *baked; // NULL: no bake of it
	u8 *orig;  // the art's colours while the bake is written in
} s_bakeRec[BAKE_RECORDS];

// (cheap: tells a level loaded at the same address apart from the last one)
static u32 Bake_Fingerprint(const struct mesh_info *mesh)
{
	u32 h = (u32)mesh->numVertex * 0x9E3779B1u ^ (u32)mesh->numQuadBlock;
	int n = mesh->numVertex, step = (n > 64) ? n / 64 : 1;
	for (int v = 0; v < n; v += step)
	{
		const struct LevVertex *lv = &mesh->ptrVertexArray[v];
		h = (h ^ ((u32)(u16)lv->pos.x | ((u32)(u16)lv->pos.y << 16))) * 0x01000193u;
		h = (h ^ (u32)(u16)lv->pos.z) * 0x01000193u;
	}
	return h;
}

static void Bake_Forget(int i)
{
	free(s_bakeRec[i].baked);
	free(s_bakeRec[i].orig);
	memset(&s_bakeRec[i], 0, sizeof(s_bakeRec[i]));
}

static int Bake_RecordFor(const struct Level *level)
{
	const struct mesh_info *mesh = level->ptr_mesh_info;
	int freeSlot = -1;
	for (int i = 0; i < BAKE_RECORDS; i++)
	{
		if ((s_bakeRec[i].level == level) && (s_bakeRec[i].mesh == mesh))
		{
			if (s_bakeRec[i].fingerprint == Bake_Fingerprint(mesh))
			{
				return i;
			}
			Bake_Forget(i); // (another level in its memory: nothing to restore)
		}
		if ((freeSlot < 0) && (s_bakeRec[i].level == NULL))
		{
			freeSlot = i;
		}
	}
	if (freeSlot < 0)
	{
		Bake_Forget(0);
		freeSlot = 0;
	}
	struct LevVertex *va = mesh->ptrVertexArray;
	s_bakeRec[freeSlot].level = level;
	s_bakeRec[freeSlot].mesh = mesh;
	s_bakeRec[freeSlot].numVerts = mesh->numVertex;
	s_bakeRec[freeSlot].fingerprint = Bake_Fingerprint(mesh);
	s_bakeRec[freeSlot].baked = (va != NULL) ? Bake_Load(Bake_MeshHash(mesh), mesh->numVertex) : NULL;
	if (s_bakeRec[freeSlot].baked)
	{
		printf("[Bake] level %p: baked lighting found (%d vertices)\n", (void *)level, mesh->numVertex);
	}
	return freeSlot;
}

static void Bake_Write(int i, int baked)
{
	struct LevVertex *va = (struct LevVertex *)s_bakeRec[i].mesh->ptrVertexArray;
	int n = s_bakeRec[i].numVerts;
	if (baked && (s_bakeRec[i].orig == NULL))
	{
		s_bakeRec[i].orig = malloc((size_t)n * 3 + 1);
		for (int v = 0; v < n; v++)
		{
			memcpy(&s_bakeRec[i].orig[v * 3], va[v].color_hi, 3);
			memcpy(va[v].color_hi, &s_bakeRec[i].baked[v * 3], 3);
		}
	}
	else if (!baked && (s_bakeRec[i].orig != NULL))
	{
		for (int v = 0; v < n; v++)
		{
			memcpy(va[v].color_hi, &s_bakeRec[i].orig[v * 3], 3);
		}
		free(s_bakeRec[i].orig);
		s_bakeRec[i].orig = NULL;
	}
}

int Rtx_BakedEnabled(void)
{
	return NativeSettings_GetInt("baked_rt", 1) != 0;
}

void Rtx_SetBakedEnabled(int on)
{
	NativeSettings_SetInt("baked_rt", on != 0);
	NativeSettings_Save();
}

// each frame before anything draws (MainFrame_RenderFrame)
void Rtx_BakeFrame(void)
{
	struct GameTracker *gGT = sdata->gGT;
	if (sdata->Loading.stage != LOAD_IDLE)
	{
		// (levels being replaced: what was written into them goes with them)
		for (int i = 0; i < BAKE_RECORDS; i++)
		{
			Bake_Forget(i);
		}
		return;
	}
	int rtDraws = Rtx_Supported() && Rtx_Drawing() && Rtx_LevelTraced(gGT);
	int want = Rtx_BakedEnabled() && !rtDraws;
	int seen[BAKE_RECORDS] = {0};
	struct Level *levels[2] = {gGT->level1, gGT->level2};
	for (int k = 0; k < 2; k++)
	{
		struct Level *level = levels[k];
		if ((level == NULL) || (level->ptr_mesh_info == NULL) || (level->ptr_mesh_info->ptrVertexArray == NULL))
		{
			continue;
		}
		int i = Bake_RecordFor(level);
		seen[i] = 1;
		if (s_bakeRec[i].baked)
		{
			Bake_Write(i, want);
		}
	}
	for (int i = 0; i < BAKE_RECORDS; i++)
	{
		if (!seen[i] && (s_bakeRec[i].level != NULL))
		{
			Bake_Forget(i); // (not loaded any more: a hub swapped out)
		}
	}
}

// ---- Bake now ------------------------------------------------------------------

struct BakeJob
{
	int levIndex, ptrIndex, vramIndex; // bigfile entries (ptrIndex -1: the map is in the lev)
	char lightsKey[32];                // the renderer's lighting file for it
	char name[40];
};

enum
{
	BAKE_IDLE,
	BAKE_NEXT,      // read the next job's level
	BAKE_WAIT_GPU,  // the renderer is baking it
	BAKE_WAIT_CPU,  // ctr_bake is tracing (pass 1 outdoors, 2 indoors)
};

static struct
{
	int state;
	int stopping;
	struct BakeJob jobs[BAKE_MAX_JOBS];
	int numJobs, cur, baked, skipped, failed;
	// the job in progress
	u8 *lev;
	struct Level *level;
	u64 hash;
	u32 gpuReq;
	pid_t cpuPid;
	int cpuPass;
	float *cpuPos, *cpuNrm;
	u32 (*cpuTris)[3];
	int cpuNumTris;
	u8 *cpuLit, *cpuGround;
	float openness;
} s_bake;

static void Bake_AddJob(struct NativeDiscImageFile *big, int levIndex, int ptrIndex, int vramIndex, const char *key, const char *name)
{
	const struct BigEntry *e = BIG_GETENTRY(sdata->ptrBigfile1);
	int numEntries = ((const struct BigHeader *)sdata->ptrBigfile1)->numEntry;
	(void)big;
	if ((levIndex < 0) || (levIndex >= numEntries) || (e[levIndex].size < 8) || (s_bake.numJobs >= BAKE_MAX_JOBS))
	{
		return;
	}
	// (custom tracks hold one lev for every LOD: the same file once)
	for (int j = 0; j < s_bake.numJobs; j++)
	{
		if (e[s_bake.jobs[j].levIndex].offset == e[levIndex].offset)
		{
			return;
		}
	}
	struct BakeJob *job = &s_bake.jobs[s_bake.numJobs++];
	job->levIndex = levIndex;
	job->ptrIndex = ptrIndex;
	job->vramIndex = vramIndex;
	snprintf(job->lightsKey, sizeof(job->lightsKey), "%s", key);
	snprintf(job->name, sizeof(job->name), "%s", name);
}

// every level the game can draw: race and battle tracks at each LOD (1P, 2P,
// 3P/4P, relic), the adventure hubs and garage, and the custom tracks (NF
// tracks play their stock level: baked with it)
static void Bake_ListJobs(struct NativeDiscImageFile *big)
{
	static const char *lodNames[4] = {"", " 2P", " 4P", " RELIC"};
	static const int lods[4] = {LOAD_LEVEL_LOD_1P, LOAD_LEVEL_LOD_2P, LOAD_LEVEL_LOD_4P, LOAD_LEVEL_LOD_RELIC};
	char key[32], name[48];
	s_bake.numJobs = 0;
	for (int levelID = 0; levelID < NITRO_COURT + LOAD_BATTLE_TRACK_COUNT; levelID++)
	{
		int first = (levelID < NITRO_COURT) ? BI_ARCADETRACKS + levelID * LOAD_TRACK_FILES_PER_LOD_GROUP
		                                    : BI_BATTLETRACKS + (levelID - NITRO_COURT) * LOAD_TRACK_FILES_PER_LOD_GROUP;
		for (int l = 0; l < 4; l++)
		{
			int group = first + GAME_LEVEL_BIG_LOD_INDEX[lods[l] - 1];
			snprintf(key, sizeof(key), "stock-%d", levelID);
			snprintf(name, sizeof(name), "%s%s", sdata->lngStrings[data.metaDataLEV[levelID].name_LNG], lodNames[l]);
			Bake_AddJob(big, group + LVI_LEV, -1, group + LVI_VRAM, key, name);
		}
	}
	for (int h = 0; h < RTX_HUB_COUNT; h++)
	{
		int group = BI_ADVENTUREHUB + h * LOAD_CUTSCENE_FILES_PER_LEVEL;
		snprintf(key, sizeof(key), "stock-%d", GEM_STONE_VALLEY + h);
		Bake_AddJob(big, group + LVI_LEV, group + LVI_PTR, group + LVI_VRAM, key, sdata->lngStrings[data.metaDataLEV[GEM_STONE_VALLEY + h].name_LNG]);
	}
	{
		int group = BI_MAINMENUFILE + LOAD_MAIN_MENU_GARAGE_FILE_OFFSET;
		snprintf(key, sizeof(key), "stock-%d", ADVENTURE_GARAGE);
		Bake_AddJob(big, group + LVI_LEV, -1, group + LVI_VRAM, key, "ADVENTURE GARAGE");
	}
	struct MTHeader *table = MT_Table();
	for (int i = 0; i < table->count; i++)
	{
		struct MTEntry *e = &table->entries[i];
		if (e->flags & MT_FLAG_REMASTER)
		{
			continue;
		}
		snprintf(key, sizeof(key), "custom-%d", e->bigBase);
		for (int l = 0; l < 4; l++)
		{
			int group = e->bigBase + GAME_LEVEL_BIG_LOD_INDEX[lods[l] - 1];
			snprintf(name, sizeof(name), "%s%s", e->name, lodNames[l]);
			Bake_AddJob(big, group + LVI_LEV, -1, group + LVI_VRAM, key, name);
		}
	}
}

static void Bake_FreeJobData(void)
{
	free(s_bake.lev);
	s_bake.lev = NULL;
	s_bake.level = NULL;
	free(s_bake.cpuPos);
	free(s_bake.cpuNrm);
	free(s_bake.cpuTris);
	free(s_bake.cpuLit);
	free(s_bake.cpuGround);
	s_bake.cpuPos = s_bake.cpuNrm = NULL;
	s_bake.cpuTris = NULL;
	s_bake.cpuLit = s_bake.cpuGround = NULL;
}

// GPU: the disc level to the renderer as a level of its own (texture bank 1:
// the first hub world bank), then a bake request
static int Bake_StartGpu(const struct BakeJob *job, const u8 *vrm, u32 vrmSize)
{
	Rtx_HubWorldForget();
	u16 *bank = (u16 *)(s_rtx.base + RTX_OFF_HUB_VRAM);
	memcpy(bank, NativeRenderer_RtxVRAM(), RTX_VRAM_W * RTX_VRAM_H * 2);
	if (vrm != NULL)
	{
		Rtx_VrmToBank(bank, vrm, vrmSize);
	}
	struct RtxMeshBuild b;
	memset(&b, 0, sizeof(b));
	b.verts = Rtx_LevelVerts();
	b.tris = Rtx_LevelTris();
	s_rtx.numTexSlots = 0;
	Rtx_SharedTri(NULL, NULL, NULL, -1);
	Rtx_AppendLevelMesh(s_bake.level, 0, &b);
	if (b.numVerts != s_bake.level->ptr_mesh_info->numVertex)
	{
		return 0; // (bigger than the renderer takes)
	}
	RTX_HDR->numLevelVerts = (u32)b.numVerts;
	RTX_HDR->numLevelTris = (u32)b.numTris;
	RTX_HDR->levelID = -1;
	RTX_HDR->redirectBase = -1;
	RTX_HDR->vramBanks = 1;
	RTX_HDR->sunFitFirst = RTX_HDR->sunFitCount = 0;
	snprintf(RTX_HDR->lightsKey, sizeof(RTX_HDR->lightsKey), "%s", job->lightsKey);
	Rtx_RefreshTexSlots();
	Rtx_UploadSky(s_bake.level);
	RTX_HDR->bakeRays = BAKE_RAYS;
	RTX_HDR->levelGen++;
	// (the level being played is uploaded again when it is traced next)
	s_rtx.levelValid = 0;
	s_rtx.numTexSlots = 0;
	__sync_synchronize();
	s_bake.gpuReq = RTX_HDR->bakeReq + 1;
	RTX_HDR->bakeReq = s_bake.gpuReq;
	Rtx_FutexWake(&RTX_HDR->frameReq);
	return 1;
}

static void Bake_FinishGpu(void)
{
	const struct mesh_info *mesh = s_bake.level->ptr_mesh_info;
	const u32 *out = (const u32 *)(s_rtx.base + RTX_OFF_BAKE_OUT);
	int n = mesh->numVertex;
	u8 *rgb = malloc((size_t)n * 3 + 1);
	for (int v = 0; v < n; v++)
	{
		rgb[v * 3] = (u8)out[v];
		rgb[v * 3 + 1] = (u8)(out[v] >> 8);
		rgb[v * 3 + 2] = (u8)(out[v] >> 16);
	}
	Bake_Save(s_bake.hash, n, BAKE_METHOD_GPU, rgb);
	free(rgb);
}

// ---- CPU (ctr_bake = tools/rt_bake/rt_bake.c), as scripts/rt_bake.py bake() ----

// scripts/rt_bake.py DEFAULTS
static const float s_cpuSunColor[3] = {1.0f, 0.93f, 0.80f}, s_cpuSkyZenith[3] = {0.30f, 0.38f, 0.55f}, s_cpuSkyHorizon[3] = {0.45f, 0.48f, 0.55f},
                   s_cpuGround[3] = {0.10f, 0.09f, 0.08f}, s_cpuIndoorAmbient[3] = {0.40f, 0.42f, 0.48f}, s_cpuLampColor[3] = {1.0f, 0.86f, 0.66f};
#define CPU_SUN_ELEVATION  40.0f
#define CPU_SUN_AZIMUTH    35.0f
#define CPU_SUN_INTENSITY  2.6f
#define CPU_SUN_CONE       1.0f
#define CPU_AO_RADIUS      1500.0f
#define CPU_LAMP_SPACING   2000.0f
#define CPU_LAMP_HEIGHT    700.0f
#define CPU_LAMP_INTENSITY 2.2f
#define CPU_LAMP_FALLOFF   1100.0f
#define CPU_LAMP_SIZE      80.0f
#define CPU_EXPOSURE_FLOOR 0.7f
#define CPU_INDOOR_BELOW   0.25f
#define CPU_BOUNCES        2u
#define CPU_ALBEDO         0.5f
#define CPU_MIN_FACTOR     0.22f
#define CPU_MAX_FACTOR     1.6f
#define CPU_EPS            4.0f

static const float s_luma[3] = {0.299f, 0.587f, 0.114f};

static float Bake_Luma(const float *c)
{
	return c[0] * s_luma[0] + c[1] * s_luma[1] + c[2] * s_luma[2];
}

static int Bake_CmpFloat(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

// the level's hi-detail triangles (QuadBlock.index[9]), its normals, which
// vertices are lit and which are driving surface
static int Bake_CpuMesh(void)
{
	// (COLL_FIXED_QUADBLK_TestTriangles; the last 4 only for quads)
	static const int hiTris[8][3] = {{8, 6, 7}, {7, 3, 8}, {1, 7, 6}, {2, 6, 8}, {0, 4, 5}, {4, 6, 5}, {6, 4, 1}, {5, 6, 2}};
	const struct mesh_info *mesh = s_bake.level->ptr_mesh_info;
	int nv = mesh->numVertex, nq = mesh->numQuadBlock;
	s_bake.cpuPos = malloc(sizeof(float) * 3 * nv);
	s_bake.cpuNrm = calloc((size_t)nv * 3, sizeof(float));
	s_bake.cpuTris = malloc(sizeof(u32) * 3 * 8 * (nq ? nq : 1));
	s_bake.cpuLit = calloc(nv, 1);
	s_bake.cpuGround = calloc(nv, 1);
	s_bake.cpuNumTris = 0;
	for (int v = 0; v < nv; v++)
	{
		s_bake.cpuPos[v * 3] = mesh->ptrVertexArray[v].pos.x;
		s_bake.cpuPos[v * 3 + 1] = mesh->ptrVertexArray[v].pos.y;
		s_bake.cpuPos[v * 3 + 2] = mesh->ptrVertexArray[v].pos.z;
	}
	for (int q = 0; q < nq; q++)
	{
		const struct QuadBlock *block = &mesh->ptrQuadBlockArray[q];
		u16 flags = *(const u16 *)&block->quadFlags;
		const u16 *idx = block->index;
		int bad = 0;
		for (int k = 0; k < 9; k++)
		{
			bad |= idx[k] >= nv;
		}
		for (int k = 0; (k < 9) && !bad && (flags & 0x1000); k++)
		{
			s_bake.cpuGround[idx[k]] = 1; // (driving surface)
		}
		if (bad || (flags & (0x1 | 0x8000))) // (invisible, invisible trigger)
		{
			continue;
		}
		for (int t = 0; t < ((idx[2] != idx[3]) ? 8 : 4); t++)
		{
			u32 a = idx[hiTris[t][0]], b = idx[hiTris[t][1]], c = idx[hiTris[t][2]];
			if ((a == b) || (b == c) || (a == c))
			{
				continue;
			}
			u32 *o = s_bake.cpuTris[s_bake.cpuNumTris++];
			o[0] = a;
			o[1] = b;
			o[2] = c;
			// area-weighted normal: (C-A) x (B-A) (CTR winding; ground faces +Y)
			const float *A = &s_bake.cpuPos[a * 3], *B = &s_bake.cpuPos[b * 3], *C = &s_bake.cpuPos[c * 3];
			float e1[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]}, e2[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
			float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
			for (int k = 0; k < 3; k++)
			{
				s_bake.cpuNrm[a * 3 + k] += fn[k];
				s_bake.cpuNrm[b * 3 + k] += fn[k];
				s_bake.cpuNrm[c * 3 + k] += fn[k];
			}
		}
	}
	for (int v = 0; v < nv; v++)
	{
		float *n = &s_bake.cpuNrm[v * 3];
		float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		s_bake.cpuLit[v] = l > 1e-6f;
		for (int k = 0; k < 3; k++)
		{
			n[k] = (l > 1e-6f) ? n[k] / l : 0.0f;
		}
	}
	return s_bake.cpuNumTris > 0;
}

static int Bake_CpuWriteScene(const char *path, int indoor)
{
	const struct mesh_info *mesh = s_bake.level->ptr_mesh_info;
	int nv = mesh->numVertex, nq = mesh->numQuadBlock;
	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		return 0;
	}
	float el = CPU_SUN_ELEVATION * 3.14159265f / 180.0f, az = CPU_SUN_AZIMUTH * 3.14159265f / 180.0f;
	float fp[16] = {cosf(el) * cosf(az), sinf(el), cosf(el) * sinf(az)};
	for (int k = 0; k < 3; k++)
	{
		fp[3 + k] = indoor ? 0.0f : s_cpuSunColor[k] * CPU_SUN_INTENSITY;
		fp[7 + k] = indoor ? s_cpuIndoorAmbient[k] : s_cpuSkyZenith[k];
		fp[10 + k] = indoor ? s_cpuIndoorAmbient[k] : s_cpuSkyHorizon[k];
		fp[13 + k] = indoor ? s_cpuIndoorAmbient[k] : s_cpuGround[k];
	}
	fp[6] = CPU_SUN_CONE * 3.14159265f / 180.0f;
	u32 head[3] = {0x4B425452u, (u32)nv, (u32)s_bake.cpuNumTris};
	u32 counts[3] = {BAKE_RAYS, BAKE_RAYS, CPU_BOUNCES};
	float epsAo[2] = {CPU_EPS, indoor ? CPU_AO_RADIUS : 0.0f};
	fwrite(head, 4, 3, f);
	fwrite(fp, 4, 16, f);
	fwrite(counts, 4, 3, f);
	fwrite(epsAo, 4, 2, f);
	fwrite(s_bake.cpuPos, 4, (size_t)nv * 3, f);
	fwrite(s_bake.cpuNrm, 4, (size_t)nv * 3, f);
	for (int v = 0; v < nv; v++)
	{
		float a[3];
		for (int k = 0; k < 3; k++)
		{
			float tint = fminf(mesh->ptrVertexArray[v].color_hi[k] / 128.0f, 2.0f);
			a[k] = fminf(CPU_ALBEDO * tint, 0.9f);
		}
		fwrite(a, 4, 3, f);
	}
	fwrite(s_bake.cpuTris, 12, s_bake.cpuNumTris, f);
	// indoors: a lamp over the floor of every spacing^3 cube (scripts/rt_bake.py light_bases)
	float *lamps = NULL;
	u32 numLamps = 0;
	if (indoor)
	{
		lamps = malloc(sizeof(float) * 9 * (nq ? nq : 1));
		for (int q = 0; q < nq; q++)
		{
			const struct QuadBlock *block = &mesh->ptrQuadBlockArray[q];
			u16 flags = *(const u16 *)&block->quadFlags;
			int c = block->index[6];
			if ((flags & (0x1 | 0x8000)) || (c >= nv) || (s_bake.cpuNrm[c * 3 + 1] <= 0.7f))
			{
				continue;
			}
			const float *p = &s_bake.cpuPos[c * 3];
			int cell[3] = {(int)floorf(p[0] / CPU_LAMP_SPACING), (int)floorf(p[1] / CPU_LAMP_SPACING), (int)floorf(p[2] / CPU_LAMP_SPACING)};
			int dup = 0;
			for (u32 l = 0; (l < numLamps) && !dup; l++)
			{
				const float *o = &lamps[l * 9];
				dup = ((int)floorf(o[0] / CPU_LAMP_SPACING) == cell[0]) && ((int)floorf(o[1] / CPU_LAMP_SPACING) == cell[1]) &&
				      ((int)floorf(o[2] / CPU_LAMP_SPACING) == cell[2]);
			}
			if (dup)
			{
				continue;
			}
			float *o = &lamps[numLamps++ * 9];
			o[0] = p[0];
			o[1] = p[1];
			o[2] = p[2];
			o[3] = CPU_LAMP_HEIGHT;
			for (int k = 0; k < 3; k++)
			{
				o[4 + k] = s_cpuLampColor[k] * CPU_LAMP_INTENSITY;
			}
			o[7] = CPU_LAMP_FALLOFF;
			o[8] = CPU_LAMP_SIZE;
		}
	}
	u32 lh[2] = {numLamps, BAKE_RAYS};
	fwrite(lh, 4, 2, f);
	if (numLamps)
	{
		fwrite(lamps, 4, (size_t)numLamps * 9, f);
	}
	free(lamps);
	return fclose(f) == 0;
}

static int Bake_CpuSpawn(int indoor)
{
	mkdir(BAKE_DIR, 0755);
	if (!Bake_CpuWriteScene(BAKE_DIR "/scene.tmp", indoor))
	{
		return 0;
	}
	char exe[1024];
	snprintf(exe, sizeof(exe), "%sctr_bake", SDL_GetBasePath());
	char *argv[] = {exe, BAKE_DIR "/scene.tmp", BAKE_DIR "/irradiance.tmp", NULL};
	if (posix_spawn(&s_bake.cpuPid, exe, NULL, NULL, argv, environ) != 0)
	{
		printf("[Bake] could not start %s\n", exe);
		return 0;
	}
	s_bake.cpuPass = indoor ? 2 : 1;
	return 1;
}

static float *Bake_CpuReadIrradiance(int nv)
{
	FILE *f = fopen(BAKE_DIR "/irradiance.tmp", "rb");
	float *irr = f ? malloc(sizeof(float) * 3 * nv) : NULL;
	if (f && (fread(irr, 12, nv, f) != (size_t)nv))
	{
		free(irr);
		irr = NULL;
	}
	if (f)
	{
		fclose(f);
	}
	return irr;
}

// median of the driving surface's light (all lit vertices if it has none)
static float Bake_CpuMedianGround(const float *irr, int nv)
{
	float *l = malloc(sizeof(float) * (nv ? nv : 1));
	int n = 0, any = 0;
	for (int v = 0; v < nv; v++)
	{
		any |= s_bake.cpuGround[v] && s_bake.cpuLit[v];
	}
	for (int v = 0; v < nv; v++)
	{
		if (s_bake.cpuLit[v] && (!any || s_bake.cpuGround[v]))
		{
			l[n++] = Bake_Luma(&irr[v * 3]);
		}
	}
	qsort(l, n, sizeof(float), Bake_CmpFloat);
	float m = n ? l[n / 2] : 0.0f;
	free(l);
	return m;
}

// a ctr_bake pass done: openness from the outdoor pass (low: indoor, traced
// again with lamps and ambient occlusion), then the colours
static int Bake_CpuPassDone(void)
{
	const struct mesh_info *mesh = s_bake.level->ptr_mesh_info;
	int nv = mesh->numVertex;
	float *irr = Bake_CpuReadIrradiance(nv);
	if (irr == NULL)
	{
		return -1;
	}
	float open[3], sinEl = sinf(CPU_SUN_ELEVATION * 3.14159265f / 180.0f);
	for (int k = 0; k < 3; k++)
	{
		open[k] = s_cpuSunColor[k] * CPU_SUN_INTENSITY * sinEl + 0.2f * s_cpuSkyHorizon[k] + 0.8f * s_cpuSkyZenith[k];
	}
	float med = Bake_CpuMedianGround(irr, nv);
	int indoor = (s_bake.cpuPass == 2);
	if (s_bake.cpuPass == 1)
	{
		s_bake.openness = med / Bake_Luma(open);
		if (s_bake.openness < CPU_INDOOR_BELOW)
		{
			free(irr);
			return Bake_CpuSpawn(1) ? 0 : -1;
		}
	}
	// per channel against open flat ground (indoors: the typical floor)
	float ref[3];
	const float *base = indoor ? s_cpuIndoorAmbient : open;
	float baseL = Bake_Luma(base);
	float k = indoor ? med / baseL : fminf(1.0f, med / baseL / CPU_EXPOSURE_FLOOR);
	for (int c = 0; c < 3; c++)
	{
		ref[c] = base[c] * k;
	}
	u8 *rgb = malloc((size_t)nv * 3 + 1);
	for (int v = 0; v < nv; v++)
	{
		for (int c = 0; c < 3; c++)
		{
			float factor = s_bake.cpuLit[v] ? fminf(fmaxf(irr[v * 3 + c] / fmaxf(ref[c], 1e-6f), CPU_MIN_FACTOR), CPU_MAX_FACTOR) : 1.0f;
			float o = rintf(mesh->ptrVertexArray[v].color_hi[c] * factor);
			rgb[v * 3 + c] = (u8)(o < 0.0f ? 0.0f : (o > 255.0f ? 255.0f : o));
		}
	}
	Bake_Save(s_bake.hash, nv, BAKE_METHOD_CPU, rgb);
	free(rgb);
	free(irr);
	return 1;
}

// ---- the job queue ---------------------------------------------------------------

static int Bake_ReadJob(struct NativeDiscImageFile *big, const struct BakeJob *job, u8 **vrm, u32 *vrmSize)
{
	u32 levSize = 0, ptrSize = 0;
	u8 *ptr = NULL;
	s_bake.lev = Rtx_ReadBigfileEntry(big, job->levIndex, &levSize);
	if ((job->ptrIndex >= 0) && (s_bake.lev != NULL))
	{
		ptr = Rtx_ReadBigfileEntry(big, job->ptrIndex, &ptrSize);
		if (ptr == NULL)
		{
			return 0;
		}
	}
	s_bake.level = (s_bake.lev != NULL) ? Rtx_PatchDiscLevel(s_bake.lev, levSize, ptr, ptrSize) : NULL;
	free(ptr);
	const struct mesh_info *mesh = s_bake.level ? s_bake.level->ptr_mesh_info : NULL;
	u8 *levEnd = s_bake.lev + levSize;
	if ((mesh == NULL) || ((u8 *)mesh < s_bake.lev) || ((u8 *)mesh >= levEnd) || (mesh->numVertex <= 0) ||
	    ((u8 *)mesh->ptrVertexArray < s_bake.lev) || ((u8 *)(mesh->ptrVertexArray + mesh->numVertex) > levEnd) ||
	    ((u8 *)mesh->ptrQuadBlockArray < s_bake.lev) || ((u8 *)(mesh->ptrQuadBlockArray + mesh->numQuadBlock) > levEnd))
	{
		return 0;
	}
	*vrm = (job->vramIndex >= 0) ? Rtx_ReadBigfileEntry(big, job->vramIndex, vrmSize) : NULL;
	return 1;
}

void Rtx_BakeStart(void)
{
	struct NativeDiscImageFile big;
	if ((s_bake.state != BAKE_IDLE) || (sdata->ptrBigfile1 == NULL) || !NativeDiscImage_FindFile("BIGFILE.BIG", &big))
	{
		return;
	}
	Bake_ListJobs(&big);
	s_bake.cur = s_bake.baked = s_bake.skipped = s_bake.failed = 0;
	s_bake.stopping = 0;
	s_bake.state = BAKE_NEXT;
	printf("[Bake] %d levels to look at (%s)\n", s_bake.numJobs, Rtx_Supported() ? "GPU" : "CPU");
}

void Rtx_BakeStop(void)
{
	s_bake.stopping = 1;
}

static void Bake_JobDone(int result)
{
	if (result > 0)
	{
		s_bake.baked++;
		printf("[Bake] %s baked\n", s_bake.jobs[s_bake.cur].name);
	}
	else if (result < 0)
	{
		s_bake.failed++;
		printf("[Bake] %s failed\n", s_bake.jobs[s_bake.cur].name);
	}
	Bake_FreeJobData();
	s_bake.cur++;
	s_bake.state = BAKE_NEXT;
}

// a step of the queue each menu frame while it runs (the menu keeps drawing)
void Rtx_BakeTick(void)
{
	struct NativeDiscImageFile big;
	switch (s_bake.state)
	{
	case BAKE_IDLE:
		return;
	case BAKE_NEXT:
		if (!NativeDiscImage_FindFile("BIGFILE.BIG", &big))
		{
			s_bake.state = BAKE_IDLE;
			return;
		}
		// (several already-baked levels a frame: they only need reading)
		for (int tries = 0; (tries < 8) && (s_bake.state == BAKE_NEXT); tries++)
		{
			if (s_bake.stopping || (s_bake.cur >= s_bake.numJobs))
			{
				printf("[Bake] done: %d baked, %d already baked, %d failed\n", s_bake.baked, s_bake.skipped, s_bake.failed);
				s_bake.state = BAKE_IDLE;
				return;
			}
			const struct BakeJob *job = &s_bake.jobs[s_bake.cur];
			u8 *vrm = NULL;
			u32 vrmSize = 0;
			if (!Bake_ReadJob(&big, job, &vrm, &vrmSize))
			{
				free(vrm);
				Bake_JobDone(-1);
				continue;
			}
			s_bake.hash = Bake_MeshHash(s_bake.level->ptr_mesh_info);
			if (Bake_Exists(s_bake.hash, s_bake.level->ptr_mesh_info->numVertex))
			{
				free(vrm);
				s_bake.skipped++;
				Bake_FreeJobData();
				s_bake.cur++;
				continue;
			}
			if (Rtx_Supported() && RTX_HDR->rendererReady && Bake_StartGpu(job, vrm, vrmSize))
			{
				s_bake.state = BAKE_WAIT_GPU;
			}
			else if (Bake_CpuMesh() && Bake_CpuSpawn(0))
			{
				s_bake.state = BAKE_WAIT_CPU;
			}
			else
			{
				Bake_JobDone(-1);
			}
			free(vrm);
		}
		return;
	case BAKE_WAIT_GPU:
		if (RTX_HDR->bakeDone == s_bake.gpuReq)
		{
			__sync_synchronize();
			Bake_FinishGpu();
			Bake_JobDone(1);
		}
		else if (!Rtx_Supported())
		{
			// (the renderer went away: the CPU takes this level, and the rest)
			if (Bake_CpuMesh() && Bake_CpuSpawn(0))
			{
				s_bake.state = BAKE_WAIT_CPU;
			}
			else
			{
				Bake_JobDone(-1);
			}
		}
		return;
	case BAKE_WAIT_CPU:
	{
		int status;
		pid_t r = waitpid(s_bake.cpuPid, &status, WNOHANG);
		if (r == 0)
		{
			return;
		}
		if ((r != s_bake.cpuPid) || !WIFEXITED(status) || (WEXITSTATUS(status) != 0))
		{
			Bake_JobDone(-1);
			return;
		}
		int done = Bake_CpuPassDone();
		if (done != 0)
		{
			Bake_JobDone(done);
		}
		return;
	}
	}
}

int Rtx_BakeRunning(void)
{
	return s_bake.state != BAKE_IDLE;
}

// progress: jobs looked at / all, the one being baked
void Rtx_BakeProgress(int *done, int *total, const char **name)
{
	*done = s_bake.cur;
	*total = s_bake.numJobs;
	*name = (s_bake.cur < s_bake.numJobs) ? s_bake.jobs[s_bake.cur].name : "";
}

void Rtx_BakeShutdown(void)
{
	if ((s_bake.state == BAKE_WAIT_CPU) && (s_bake.cpuPid > 0))
	{
		kill(s_bake.cpuPid, SIGTERM);
	}
}
