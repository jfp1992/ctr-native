// MultiTrack: real-time ray tracing bridge (game side, 32-bit).
// See include/platform/native_rtx.h and rtx/rtx_shared.h.

#include "platform/native_rtx.h"
#include "platform/native_disc_image.h"
#include "platform/native_settings.h"
#include "../rtx/rtx_shared.h"

#include <SDL3/SDL.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
// not declared under -std=c99 without _POSIX_C_SOURCE (unity build includes headers first)
int ftruncate(int fd, off_t length);
int kill(pid_t pid, int sig);
long syscall(long number, ...);

// sleep while *addr == val (at most ns), wake the renderer's sleeper (rtx_shared.h)
static void Rtx_FutexWait(volatile u32 *addr, u32 val, long ns)
{
	struct
	{
		long sec, nsec; // (the kernel's timespec of this 32-bit futex call)
	} ts = {0, ns};
	syscall(SYS_futex, (u32 *)addr, FUTEX_WAIT, val, &ts, NULL, 0);
}

static void Rtx_FutexWake(volatile u32 *addr)
{
	syscall(SYS_futex, (u32 *)addr, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
}

const u16 *NativeRenderer_RtxVRAM(void);

int g_rtxCapture;

#define RTX_MAX_RANGES  16384
#define RTX_CAP_BITS    18
#define RTX_LEVEL_SERIAL 0xffff // projections of the level (instances count 1..0xfffe)
#define RTX_CAP_SIZE    (1 << RTX_CAP_BITS)
#define RTX_MAX_TEXSRC  RTX_MAX_TEX_SLOTS
#define RTX_MAX_EMITTERS 1024

struct RtxRange
{
	u32 start, end;
	u16 tag;
	u16 serial;
	u8 allowSemi; // wheels: semi-transparent code, but colour-keyed sprites
	u8 mode;      // RtxInstMode
	float z;      // RTX_TAG_DEPTH: view z of the whole range
};

enum RtxInstMode
{
	RTX_INST_CAPTURE, // solid model: ray traced geometry
	RTX_INST_GLOW,    // transparent glowing model (turbo flames, fire jets): GL draws it, it lights the scene
	RTX_INST_GL,      // ghosts and other transparent models: GL only
	RTX_INST_HIDE,    // PS1 scenery a remaster mesh replaces: neither traced nor drawn
};

// something glowing (additive blend) seen this frame; clustered into point lights
struct RtxEmitter
{
	float pos[3];
	float color[3]; // weighted sum
	float weight;
	float radius;
	const void *inst;  // the model instance it glows on (NULL: particles); a cluster: its first
	float instPos[3];  // where that instance stands
};

struct RtxCapEntry
{
	u32 sxy;
	u32 serial;
	u32 gen;
	float v[3];
};

static struct
{
	int fd;
	char shmName[64];
	u8 *base;
	struct RtxHeader *hdr;
	pid_t rendererPid;
	int rendererRan; // (it got ready once: started again if it dies)
	int enabled;   // F7
	int debugView; // F6
	struct RtxSettings settings; // F12 overlay keys 1-8, CTR_RTX_* env
	// last request (pipelined: its image is the one shown next)
	int prevPipelined, prevW, prevH;
	u32 prevGen;

	// per frame
	u32 gen;
	int cameraValid; // bit per view (split screen: one per player)
	int numViews;    // players' views this frame
	// cameras by view (pushBuffer index): view -> world of what each player's
	// view drew; view = the one being recorded (drawing) or decoded (OT walk)
	float rInv[RTX_MAX_VIEWS][9];
	float camT[RTX_MAX_VIEWS][3];
	int view;
	struct RtxRange ranges[RTX_MAX_RANGES];
	int numRanges;
	int rangesSorted;
	int tagStack[4];
	u32 tagStart[4];
	int tagDepth;
	u16 serial;
	float instScale; // true view / GTE view of the current instance
	int allowSemi;
	int instMode;
	int lightMode;
	int camForCapture; // bit per view: its camera is in camT / rInv
	int skyPhase;
	int numDyn;
	struct RtxEmitter emitters[RTX_MAX_EMITTERS];
	u8 emitterView[RTX_MAX_EMITTERS];
	int numEmitters;
	// glows that stay put (torches, braziers, statue fire) are remembered where
	// they were seen: the game only draws them while they are on screen, but
	// their light reaches past it (Rtx_BuildDynLights)
	struct RtxRememberedLight
	{
		float pos[3], color[3]; // smoothed, as RtxPointLight
		float anchor[3];        // where it was first seen
		float falloff, size;
		u32 lastGen;  // game frame last seen
		u32 levelGen; // level it belongs to
		u16 seen;     // frames seen in the same place
		u8 moving;    // moved since first seen (kart flames): never remembered
		u8 matched;   // seen this frame
		const void *inst;     // the model it glows on (save screens, warp pads): still while that stays put
		float instAnchor[3];  // where that model stood when first seen
	} remembered[128];
	int worldCapture; // record GTE projections outside tags (skidmarks, effects) for the overlay depth test
	int numLevelCap;  // level polygons of this frame's OT walk (the renderer gets them next frame)
	u16 capSerial;    // key of the projections being recorded: instance serial, 0 = world
	u32 otBase[RTX_MAX_VIEWS]; // each player's ordering table (0x400 buckets) and the UI one, this frame
	u32 uiOtStart, uiOtEnd;
	// split screen: every view draws every model it sees; the first view to
	// draw one traces it, the others only hand it over (GL skips it there too)
	struct
	{
		const void *key;
		u32 gen;
		int view;
	} drawnBy[1024];
	int dupDraw; // the instance being drawn was traced from another view this frame
	int rtxOnly; // the instance being drawn is outside the view: the PS1 draws none of it, the ray tracer all
	int tagDynStart; // dynamic triangles before the open instance tag (its culled triangles are added while it draws)

	// GL overlay depth: context of the packet being parsed
	int ctxActive;
	int ctxWorld;  // last OT entry walked was a player's view's
	int ctxBucket; // its index
	int ctxView;   // that view
	u16 ctxSerial;
	float ctxZ;
	// bucket -> view z, fitted from model packets (OT index ~ z / 64)
	float zPerBucket;
	double calZ, calB;
	int calN;

	// level
	void *levelPtr;
	void *meshPtr;
	int levelID;
	// one per slot: ptr_texture_mid value (IconGroup4, | 1 = AnimTex), or a plain
	// TextureLayout | 2 (the water env map)
	u32 texSrc[RTX_MAX_TEXSRC];
	u8 texBank[RTX_MAX_TEXSRC]; // its VRAM: 0 live, 1.. a hub of the hub world (RTX_TPAGE_BANK_SHIFT)
	int curTexBank;             // for the slots made now
	int numTexSlots;
	int levelValid;

	struct RtxCapEntry cap[RTX_CAP_SIZE];

	// packets captured into dynamic triangles this frame: word offsets (after the
	// tag) of the polygon commands GL skips, RTX_MASK_ALL = the whole packet
	u32 capturedKey[1 << 15];
	u32 capturedGen[1 << 15];
	u32 capturedMask[1 << 15];
	u32 ctxMask; // of the packet being parsed

	// smoke particles of this frame's OT walk as camera-facing quads (shadows, next frame)
	struct RtxDynTri smoke[2048];
	int numSmoke;
	float partOfx[RTX_MAX_VIEWS], partOfy[RTX_MAX_VIEWS], partH[RTX_MAX_VIEWS]; // the GTE projection particles are drawn with, by view

	// karts this frame: the dynamic triangles of each driver's body, so its
	// wheels' range can move it onto the traced floor with them (Rtx_EndTag)
	struct
	{
		const void *inst;
		int start, end;
	} kartBody[16];
	int numKartBody;
	const void *curKart;   // instance being captured is a driver's body
	const struct Instance *curInst; // instance being captured (its glows know their source)
	const struct Instance *emitInst; // source of the emitters being added now
	const void *curWheels; // wheel range being captured: of this driver instance
	struct
	{
		const void *inst;
		float dy;
		u32 gen;
	} kartLift[16]; // smoothed correction per driver
	double statGapSum, statGapAbs;
	int statGapN, statGapFar, statGapBody, statCapAnimated;

	// textures that animate (boosters): tpage, clut, uv rectangle of every frame,
	// to tell their polygons among the level as drawn (RTX_TRI_ANIMATED)
	struct
	{
		u16 tpage, clut;
		u8 u0, v0, u1, v1;
	} animRect[1024];
	int numAnimRect;

	// capture statistics (CTR_RTX_DEBUG), model packets this frame
	int statCaptured, statNoProj, statSemi, statInstances, statCulledTris;
	// emitter statistics (CTR_RTX_DEBUG): instances by mode, candidates by source / fate
	// (summed over the 300 frames between reports)
	int curModelId;            // Model.id of the instance being drawn (-1: none)
	int statGlLeftModel[256];  // (debug) instances by Model.id partly left GL drawn
	int statInstMode[4], statEmitGlow, statEmitSemi, statEmitOpaque, statEmitParticle, statEmitGrey, statEmitDim, statEmitNoProj;
	int statPartAdditive, statPartVivid, statPartWhite, statPartDull, statPartNoTex;
	int statPolyNoTex, statPolyNotGlow, statEmitAdded, statEmitFull, statLights;
	int statMemMoved, statMemGone, statMemOld;
} s_rtx;

// Ctrl+F6 (or CTR_RTX_PROBE_FRAME=n): one frame described in rtx_probe.txt
// next to the executable; the renderer adds what its camera rays hit there
static struct
{
	int want;    // asked for: the next published frame
	int pending; // published: its karts against the traced depth when the image is back
} s_probe;

#define RTX_MASK_ALL 0xffffffffu

#define RTX_HDR (s_rtx.hdr)

static struct RtxLevelVert *Rtx_LevelVerts(void) { return (struct RtxLevelVert *)(s_rtx.base + RTX_OFF_LEVEL_VERTS); }
static struct RtxLevelTri *Rtx_LevelTris(void) { return (struct RtxLevelTri *)(s_rtx.base + RTX_OFF_LEVEL_TRIS); }
static struct RtxTexSlot *Rtx_TexSlots(void) { return (struct RtxTexSlot *)(s_rtx.base + RTX_OFF_TEX_SLOTS); }
static struct RtxDynTri *Rtx_DynTris(void) { return (struct RtxDynTri *)(s_rtx.base + RTX_OFF_DYN_TRIS); }

static void Rtx_InitSettings(void);

// the renderer draws whenever it runs: ray tracing off is its baked mode
// (RtxSettings.baked: a PS1 level lit from its lightmap, a Nitro-Fueled track
// from its vertex bake), the GL path with baked vertex light is for GPUs
// without ray tracing
static int Rtx_Drawing(void)
{
	return s_rtx.enabled || (s_rtx.rendererPid > 0);
}

// starts the renderer process on the shared memory (again after it died:
// Rtx_RendererExited)
static int Rtx_SpawnRenderer(void)
{
	char exe[1024], shm[64];
	snprintf(exe, sizeof(exe), "%sctr_rtx", SDL_GetBasePath());
	// (the renderer unlinks the shared memory's name once it has it mapped: a
	// renderer started again opens it through this process' descriptor)
	if (s_rtx.rendererRan)
	{
		snprintf(shm, sizeof(shm), "/proc/%d/fd/%d", (int)getpid(), s_rtx.fd);
	}
	else
	{
		snprintf(shm, sizeof(shm), "%s", s_rtx.shmName);
	}
	char *argv[] = {exe, shm, NULL};
	pid_t pid;
	int err = posix_spawn(&pid, exe, NULL, NULL, argv, environ);
	if (err != 0)
	{
		printf("[RTX] could not start %s: %s (ray tracing off)\n", exe, strerror(err));
		return 0;
	}
	s_rtx.rendererPid = pid;
	printf("[RTX] renderer %s started (pid %d)\n", exe, (int)pid);
	return 1;
}

void Rtx_Init(void)
{
	Rtx_InitSettings();
	const char *env = SDL_getenv("CTR_RTX");
	if ((env != NULL) && (env[0] == '0'))
	{
		printf("[RTX] disabled (CTR_RTX=0)\n");
		return;
	}

	snprintf(s_rtx.shmName, sizeof(s_rtx.shmName), "%s-%d", RTX_SHM_NAME, (int)getpid());
	s_rtx.fd = shm_open(s_rtx.shmName, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (s_rtx.fd < 0)
	{
		printf("[RTX] shm_open failed: %s\n", strerror(errno));
		return;
	}
	if (ftruncate(s_rtx.fd, RTX_SHM_SIZE) != 0)
	{
		printf("[RTX] ftruncate %u failed: %s\n", (unsigned)RTX_SHM_SIZE, strerror(errno));
		return;
	}
	void *map = mmap(NULL, RTX_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, s_rtx.fd, 0);
	if (map == MAP_FAILED)
	{
		printf("[RTX] mmap failed: %s\n", strerror(errno));
		return;
	}
	s_rtx.base = (u8 *)map;
	s_rtx.hdr = (struct RtxHeader *)map;
	memset(s_rtx.hdr, 0, sizeof(*s_rtx.hdr));
	RTX_HDR->magic = RTX_MAGIC;
	RTX_HDR->version = RTX_VERSION;
	RTX_HDR->gamePid = (u32)getpid();
	RTX_HDR->redirectBase = -1;

	if (!Rtx_SpawnRenderer())
	{
		shm_unlink(s_rtx.shmName);
		return;
	}
	s_rtx.enabled = NativeSettings_GetInt("rtx", 1) != 0;
	const char *on = SDL_getenv("CTR_RTX_ON"); // (test harness: ray tracing on / off as F7, without touching settings.ini)
	s_rtx.enabled = on ? (atoi(on) != 0) : s_rtx.enabled;
	const char *view = SDL_getenv("CTR_RTX_VIEW"); // start in a debug view (as F6): 1 albedo, 2 normals, 3 direct, 4 indirect, 5 texture layer misses
	s_rtx.debugView = view ? (atoi(view) % 8) : 0;
	const char *mode = SDL_getenv("CTR_RTX_HYBRID"); // start in hybrid lighting (as F5)
	s_rtx.lightMode = (mode && mode[0] == '1');
	printf("[RTX] F7 ray tracing on/off, F5 hybrid/full lighting, F6 debug views\n");
}

void Rtx_Shutdown(void)
{
	Rtx_BakeShutdown();
	if (RTX_HDR != NULL)
	{
		RTX_HDR->quit = 1;
	}
	if (s_rtx.shmName[0])
	{
		shm_unlink(s_rtx.shmName);
	}
}

static int Rtx_EnvInt(const char *name, int fallback)
{
	const char *v = SDL_getenv(name);
	return v ? atoi(v) : fallback;
}

// renderer settings: defaults (the user's preferred set, native/preferred-defaults.png),
// then settings.ini (main menu Settings / F12 overlay), CTR_RTX_* env vars override
static int Rtx_Setting(const char *env, const char *key, int fallback)
{
	return Rtx_EnvInt(env, NativeSettings_GetInt(key, fallback));
}

static u32 Rtx_ClampSpp(int spp)
{
	spp = (spp + (int)RTX_SPP_STEP / 2) / (int)RTX_SPP_STEP * (int)RTX_SPP_STEP;
	return (u32)((spp < (int)RTX_MIN_SPP) ? (int)RTX_MIN_SPP : ((spp > (int)RTX_MAX_SPP) ? (int)RTX_MAX_SPP : spp));
}

static void Rtx_InitSettings(void)
{
	struct RtxSettings *S = &s_rtx.settings;
	S->renderScale = (u32)Rtx_Setting("CTR_RTX_SCALE", "rtx_scale", 67);
	S->renderScale = S->renderScale < 25 ? 25 : (S->renderScale > 100 ? 100 : S->renderScale);
	S->spp = Rtx_ClampSpp(Rtx_Setting("CTR_RTX_SPP", "rtx_spp", 4));
	const char *budget = SDL_getenv("CTR_RTX_BUDGET_MS");
	S->budgetMs = budget ? (float)atof(budget) : (float)NativeSettings_GetInt("rtx_budget", 0);
	S->taa = Rtx_Setting("CTR_RTX_TAA", "rtx_taa", 1) != 0;
	S->texFilter = Rtx_Setting("CTR_RTX_TEXFILTER", "rtx_texfilter", 1) != 0;
	S->specular = Rtx_Setting("CTR_RTX_SPEC", "rtx_spec", 1) != 0;
	S->fog = Rtx_Setting("CTR_RTX_FOG", "rtx_fog", 0) != 0;
	S->pipelined = Rtx_Setting("CTR_RTX_PIPELINE", "rtx_pipeline", 1) != 0;
}

// settings.ini gets what the overlay / menu / F7 changed
static void Rtx_SaveSettings(void)
{
	const struct RtxSettings *S = &s_rtx.settings;
	NativeSettings_SetInt("rtx", s_rtx.enabled);
	NativeSettings_SetInt("rtx_scale", (int)S->renderScale);
	NativeSettings_SetInt("rtx_spp", (int)S->spp);
	NativeSettings_SetInt("rtx_budget", (int)(S->budgetMs + 0.5f));
	NativeSettings_SetInt("rtx_taa", (int)S->taa);
	NativeSettings_SetInt("rtx_texfilter", (int)S->texFilter);
	NativeSettings_SetInt("rtx_spec", (int)S->specular);
	NativeSettings_SetInt("rtx_fog", (int)S->fog);
	NativeSettings_SetInt("rtx_pipeline", (int)S->pipelined);
	NativeSettings_Save();
}

static int Rtx_RendererExited(void);

// the renderer can trace: started and still running (it exits at once when
// the GPU has no ray queries)
int Rtx_Supported(void)
{
	if ((RTX_HDR == NULL) || (s_rtx.rendererPid <= 0))
	{
		return 0;
	}
	return !Rtx_RendererExited();
}

// whether the renderer process has exited (logged once); one that ran (was
// ready) is started again, a few times a session, and the level uploaded anew
// (one that never got ready: no ray tracing GPU, it stays off)
static int Rtx_RendererExited(void)
{
	static int s_restarts;
	int status;
	if ((s_rtx.rendererPid <= 0) || (waitpid(s_rtx.rendererPid, &status, WNOHANG) != s_rtx.rendererPid))
	{
		return s_rtx.rendererPid <= 0;
	}
	int ran = RTX_HDR->rendererReady || s_rtx.rendererRan;
	printf("[RTX] renderer exited (%s %d)%s\n", WIFEXITED(status) ? "code" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status),
	       (ran && s_restarts < 3) ? ", starting it again" : ": no ray tracing");
	s_rtx.rendererPid = -1;
	RTX_HDR->rendererReady = 0;
	s_rtx.levelValid = 0;
	s_rtx.rendererRan = ran;
	if (ran && (s_restarts < 3))
	{
		s_restarts++;
		Rtx_SpawnRenderer();
	}
	return s_rtx.rendererPid <= 0;
}

int Rtx_Enabled(void)
{
	return s_rtx.enabled;
}

void Rtx_SetEnabled(int on)
{
	s_rtx.enabled = on != 0;
	printf("[RTX] ray tracing %s\n", s_rtx.enabled ? "on" : "off (baked light)");
	Rtx_SaveSettings();
}

int Rtx_GetSpp(void)
{
	return (int)s_rtx.settings.spp;
}

void Rtx_SetSpp(int spp)
{
	s_rtx.settings.spp = Rtx_ClampSpp(spp);
	Rtx_SaveSettings();
}

static const char *s_rtxViewNames[] = {"final", "albedo", "normal", "direct", "indirect", "texture layer misses (red)", "sun x shadow", "sun, smooth normals"};

const char *Rtx_KeyStatus(int scancode)
{
	if (s_rtx.hdr == NULL)
	{
		return "unavailable";
	}
	switch (scancode)
	{
	case SDL_SCANCODE_F5:
		return s_rtx.lightMode ? "hybrid" : "full relight";
	case SDL_SCANCODE_F6:
		return s_rtxViewNames[s_rtx.debugView & 7];
	case SDL_SCANCODE_F7:
		return !Rtx_Supported() ? "not supported (no ray tracing GPU)" : (RTX_HDR->rendererReady ? (s_rtx.enabled ? "on" : "off (baked)") : "renderer not ready");
	}
	return "";
}

// overlay: the renderer settings (keys 1-8) and its timings
int Rtx_OverlayText(char *buf, int size)
{
	if (s_rtx.hdr == NULL)
	{
		return snprintf(buf, size, "ray tracing unavailable (renderer not started)\n");
	}
	const struct RtxSettings *S = &s_rtx.settings;
	const float *ms = RTX_HDR->passMs;
	char budgetText[24];
	if (S->budgetMs > 0.0f)
	{
		snprintf(budgetText, sizeof(budgetText), "%.0f ms", S->budgetMs);
	}
	else
	{
		snprintf(budgetText, sizeof(budgetText), "off");
	}
	return snprintf(buf, size,
	                "-- ray tracing: keys 1-8 change, Shift+key back --\n"
	                "1 render scale    %u%% (%ux%u -> %ux%u)\n"
	                "2 samples         %u spp, now %u\n"
	                "3 TAA             %s\n"
	                "4 texture filter  %s\n"
	                "5 specular        %s\n"
	                "6 fog / shafts    %s\n"
	                "7 pipelined       %s\n"
	                "8 trace budget    %s\n"
	                "-- renderer --\n"
	                "GPU %5.2f ms  CPU %5.2f ms  frame %5.2f ms\n"
	                " build %.2f  trace %.2f  cache %.2f\n"
	                " a-trous %.2f  final %.2f  copy %.2f\n",
	                S->renderScale, RTX_HDR->inW, RTX_HDR->inH, RTX_HDR->outW, RTX_HDR->outH, S->spp, RTX_HDR->traceSpp,
	                S->taa ? "on" : "off", S->texFilter ? "bilinear" : "nearest (PS1)", S->specular ? "on" : "off", S->fog ? "on" : "off",
	                S->pipelined ? "on (+1 frame latency)" : "off", budgetText, ms[RTX_PASS_GPU], ms[RTX_PASS_CPU], RTX_HDR->renderMs,
	                ms[RTX_PASS_BUILD], ms[RTX_PASS_TRACE], ms[RTX_PASS_CACHE], ms[RTX_PASS_ATROUS], ms[RTX_PASS_FINAL], ms[RTX_PASS_COPY]);
}

// overlay key 1-8: step that setting (back: the other way)
void Rtx_OverlayKey(int digit, int back)
{
	struct RtxSettings *S = &s_rtx.settings;
	int dir = back ? -1 : 1;
	static const u32 scales[] = {50, 67, 75, 100};
	static const float budgets[] = {0.0f, 8.0f, 10.0f, 14.0f, 18.0f, 22.0f, 28.0f, 33.0f, 50.0f}; // (0: off)
	const int numBudgets = (int)(sizeof(budgets) / sizeof(budgets[0]));
	switch (digit)
	{
	case 1:
	{
		int i = 0;
		while ((i < 3) && (scales[i] < S->renderScale))
		{
			i++;
		}
		i = (scales[i] == S->renderScale) ? (i + dir + 4) % 4 : (back ? (i + 3) % 4 : i);
		S->renderScale = scales[i];
		break;
	}
	case 2:
		S->spp = (S->spp == RTX_MAX_SPP && dir > 0) ? RTX_MIN_SPP
		         : ((S->spp == RTX_MIN_SPP && dir < 0) ? RTX_MAX_SPP : Rtx_ClampSpp((int)S->spp + dir * (int)RTX_SPP_STEP));
		break;
	case 3:
		S->taa ^= 1;
		break;
	case 4:
		S->texFilter ^= 1;
		break;
	case 5:
		S->specular ^= 1;
		break;
	case 6:
		S->fog ^= 1;
		break;
	case 7:
		S->pipelined ^= 1;
		break;
	case 8:
	{
		int i = 0;
		while ((i < numBudgets - 1) && (budgets[i] < S->budgetMs - 0.5f))
		{
			i++;
		}
		S->budgetMs = budgets[(i + dir + numBudgets) % numBudgets];
		break;
	}
	}
	Rtx_SaveSettings();
}

void Rtx_HandleKey(int scancode)
{
	if (scancode == SDL_SCANCODE_F7)
	{
		Rtx_SetEnabled(!s_rtx.enabled);
	}
	else if (scancode == SDL_SCANCODE_F5)
	{
		s_rtx.lightMode ^= 1;
		printf("[RTX] lighting: %s\n", s_rtx.lightMode ? "hybrid (track's own lighting + ray traced extras)" : "full relight");
	}
	else if ((scancode == SDL_SCANCODE_F6) && ((SDL_GetModState() & SDL_KMOD_CTRL) != 0))
	{
		s_probe.want = 1;
		printf("[RTX] probe: this frame goes to %srtx_probe.txt\n", SDL_GetBasePath());
	}
	else if (scancode == SDL_SCANCODE_F6)
	{
		s_rtx.debugView = (s_rtx.debugView + 1) % 6;
		printf("[RTX] debug view: %s\n", s_rtxViewNames[s_rtx.debugView]);
	}
}

// ---- per-frame capture -----------------------------------------------------

void Rtx_FrameBegin(void)
{
	s_rtx.gen++;
	if (s_rtx.gen == 0)
	{
		s_rtx.gen = 1;
	}
	s_rtx.cameraValid = 0;
	s_rtx.numViews = 0;
	s_rtx.view = 0;
	s_rtx.ctxView = 0;
	s_rtx.dupDraw = 0;
	memset(s_rtx.otBase, 0, sizeof(s_rtx.otBase));
	s_rtx.numRanges = 0;
	s_rtx.rangesSorted = 1;
	s_rtx.tagDepth = 0;
	s_rtx.skyPhase = 1;
	s_rtx.numDyn = 0;
	s_rtx.numKartBody = 0;
	s_rtx.curKart = s_rtx.curWheels = NULL;
	s_rtx.numEmitters = 0;
	s_rtx.camForCapture = 0;
	if (s_rtx.calN >= 16)
	{
		float k = (float)(s_rtx.calZ / s_rtx.calB);
		s_rtx.zPerBucket = k < 8.0f ? 8.0f : (k > 1024.0f ? 1024.0f : k);
	}
	else if (s_rtx.zPerBucket <= 0.0f)
	{
		s_rtx.zPerBucket = 64.0f;
	}
	s_rtx.calZ = s_rtx.calB = 0.0;
	s_rtx.calN = 0;
	s_rtx.worldCapture = (RTX_HDR != NULL) && Rtx_Drawing() && RTX_HDR->rendererReady;
	s_rtx.statCaptured = s_rtx.statNoProj = s_rtx.statSemi = s_rtx.statInstances = s_rtx.statCulledTris = 0;
	s_rtx.capSerial = 0;
	s_rtx.instScale = 1.0f;
	g_rtxCapture = s_rtx.worldCapture;
}

// which projections the GTE hook records, from the innermost open tag
static void Rtx_UpdateCapture(void)
{
	int top = (s_rtx.tagDepth > 0) ? s_rtx.tagStack[s_rtx.tagDepth - 1] : -1;
	if (top == RTX_TAG_INSTANCE)
	{
		s_rtx.capSerial = s_rtx.serial;
		g_rtxCapture = 1;
	}
	else if (top < 0)
	{
		s_rtx.capSerial = 0;
		s_rtx.instScale = 1.0f;
		g_rtxCapture = s_rtx.worldCapture;
	}
	else if (top == RTX_TAG_LEVEL)
	{
		// the level as the PS1 draws it (LOD, hi-detail ground, animated textures):
		// its polygons are decoded in the OT walk (Rtx_ClassifyPacket)
		s_rtx.capSerial = RTX_LEVEL_SERIAL;
		s_rtx.instScale = 1.0f;
		g_rtxCapture = s_rtx.worldCapture;
	}
	else
	{
		g_rtxCapture = 0; // level, sky, shadows, HUD models
	}
}

void Rtx_BeginTag(int tag, struct PrimMem *primMem)
{
	if ((RTX_HDR == NULL) || (s_rtx.tagDepth >= 4))
	{
		return;
	}
	s_rtx.tagStack[s_rtx.tagDepth] = tag;
	s_rtx.tagStart[s_rtx.tagDepth] = (u32)primMem->cursor;
	s_rtx.tagDepth++;
	if (tag == RTX_TAG_INSTANCE)
	{
		s_rtx.tagDynStart = s_rtx.numDyn;
		s_rtx.serial++;
		if ((s_rtx.serial == 0) || (s_rtx.serial == RTX_LEVEL_SERIAL))
		{
			s_rtx.serial = 1;
		}
	}
	Rtx_UpdateCapture();
}

static u32 Rtx_CapturePacketTo(u32 *words, int numWords, u16 serial, int allowSemi, struct RtxDynTri *out, int *num, int max);
static void Rtx_SeatKart(const void *inst, int wheelStart, int wheelEnd);
static void Rtx_SeatInstance(int start, int end);
#define Rtx_CapturePacket(words, numWords, serial, allowSemi) \
	Rtx_CapturePacketTo(words, numWords, serial, allowSemi, Rtx_DynTris(), &s_rtx.numDyn, (int)RTX_MAX_DYN_TRIS)
static void Rtx_EmitterPacket(u32 *words, int numWords, u16 serial, int anyBlend);
static void Rtx_MarkCaptured(u32 packet, u32 mask);

void Rtx_EndTag(struct PrimMem *primMem)
{
	if ((RTX_HDR == NULL) || (s_rtx.tagDepth <= 0))
	{
		return;
	}
	s_rtx.tagDepth--;
	int tag = s_rtx.tagStack[s_rtx.tagDepth];
	u32 start = s_rtx.tagStart[s_rtx.tagDepth];
	u32 end = (u32)primMem->cursor;
	Rtx_UpdateCapture();

	if ((end <= start) || (s_rtx.numRanges >= RTX_MAX_RANGES))
	{
		return;
	}
	struct RtxRange *r = &s_rtx.ranges[s_rtx.numRanges++];
	if ((s_rtx.numRanges > 1) && (start < r[-1].start))
	{
		s_rtx.rangesSorted = 0;
	}
	r->start = start;
	r->end = end;
	r->tag = (u16)tag;
	r->serial = (tag == RTX_TAG_INSTANCE) ? s_rtx.serial : 0;
	r->allowSemi = (u8)s_rtx.allowSemi;
	r->mode = (u8)s_rtx.instMode;
	r->z = 0.0f;
	s_rtx.allowSemi = 0;
	s_rtx.instMode = RTX_INST_CAPTURE;

	// model packets are allocated back to back: decode them now, so the ray
	// traced image can be rendered before the GL overlay pass draws over it
	// (its culled triangles, Rtx_CaptureCulledTri, came in while it drew)
	int dynStart = (tag == RTX_TAG_INSTANCE) ? s_rtx.tagDynStart : s_rtx.numDyn;
	const void *kart = s_rtx.curKart, *wheelsOf = s_rtx.curWheels;
	s_rtx.curKart = s_rtx.curWheels = NULL;
	int dup = s_rtx.dupDraw;
	s_rtx.dupDraw = 0;
	s_rtx.emitInst = (tag == RTX_TAG_INSTANCE) ? s_rtx.curInst : NULL;
	s_rtx.curInst = NULL;
	int glLeft = 0; // some of the model stays GL drawn
	if ((tag == RTX_TAG_INSTANCE) && dup && Rtx_Drawing() && RTX_HDR->rendererReady)
	{
		// split screen, a model another view traced: GL skips what that view's
		// copy hands over (decoded only for which commands those are)
		u32 *p = (u32 *)start;
		while ((u32)p < end)
		{
			int len = getlen(p);
			if ((len == 0) || (len > 32) || ((u32)(p + 1 + len) > end))
			{
				break;
			}
			if (r->mode == RTX_INST_HIDE)
			{
				Rtx_MarkCaptured((u32)p, RTX_MASK_ALL);
			}
			else if (r->mode == RTX_INST_CAPTURE)
			{
				static struct RtxDynTri scratch[64];
				int n = 0;
				u32 mask = Rtx_CapturePacketTo(p + 1, len, r->serial, r->allowSemi, scratch, &n, 64);
				if (mask != 0)
				{
					Rtx_MarkCaptured((u32)p, mask);
				}
			}
			p += 1 + len;
		}
	}
	else if ((tag == RTX_TAG_INSTANCE) && Rtx_Drawing() && RTX_HDR->rendererReady)
	{
		u32 *p = (u32 *)start;
		while ((u32)p < end)
		{
			int len = getlen(p);
			if ((len == 0) || (len > 32) || ((u32)(p + 1 + len) > end))
			{
				break;
			}
			if (r->mode == RTX_INST_GLOW)
			{
				Rtx_EmitterPacket(p + 1, len, r->serial, 1);
			}
			else if (r->mode == RTX_INST_GL)
			{
			}
			else if (r->mode == RTX_INST_HIDE)
			{
				Rtx_MarkCaptured((u32)p, RTX_MASK_ALL); // (GL skips it, nothing traced)
			}
			else
			{
				// opaque polygons are traced; the rest (glows, lamp cones) stays GL drawn and lights the scene
				u32 mask = Rtx_CapturePacket(p + 1, len, r->serial, r->allowSemi);
				if (mask != 0)
				{
					Rtx_MarkCaptured((u32)p, mask);
				}
				if (mask != RTX_MASK_ALL)
				{
					Rtx_EmitterPacket(p + 1, len, r->serial, 0);
					glLeft = 1;
				}
			}
			p += 1 + len;
		}
		if ((kart != NULL) && (s_rtx.numKartBody < 16))
		{
			s_rtx.kartBody[s_rtx.numKartBody].inst = kart;
			s_rtx.kartBody[s_rtx.numKartBody].start = dynStart;
			s_rtx.kartBody[s_rtx.numKartBody].end = s_rtx.numDyn;
			s_rtx.numKartBody++;
		}
		if (wheelsOf != NULL)
		{
			Rtx_SeatKart(wheelsOf, dynStart, s_rtx.numDyn);
		}
		else if ((kart == NULL) && (r->mode == RTX_INST_CAPTURE) && !glLeft)
		{
			// (only models traced whole: the GL drawn rest, a piranha plant's
			// semi-transparent mouth, would stay at the PS1 height)
			Rtx_SeatInstance(dynStart, s_rtx.numDyn);
		}
		if (glLeft && (s_rtx.curModelId >= 0) && (s_rtx.curModelId < 256))
		{
			s_rtx.statGlLeftModel[s_rtx.curModelId]++;
		}
	}
}

static u32 Rtx_PtrHash(u32 p)
{
	u32 h = p * 0x9E3779B1u;
	return (h ^ (h >> 16)) & ((1 << 15) - 1);
}

static void Rtx_MarkCaptured(u32 packet, u32 mask)
{
	u32 h = Rtx_PtrHash(packet);
	for (int probe = 0; probe < 64; probe++)
	{
		u32 i = (h + probe) & ((1 << 15) - 1);
		if (s_rtx.capturedGen[i] != s_rtx.gen)
		{
			s_rtx.capturedGen[i] = s_rtx.gen;
			s_rtx.capturedKey[i] = packet;
			s_rtx.capturedMask[i] = mask;
			return;
		}
	}
}

static u32 Rtx_CapturedMask(u32 packet)
{
	u32 h = Rtx_PtrHash(packet);
	for (int probe = 0; probe < 64; probe++)
	{
		u32 i = (h + probe) & ((1 << 15) - 1);
		if (s_rtx.capturedGen[i] != s_rtx.gen)
		{
			return 0;
		}
		if (s_rtx.capturedKey[i] == packet)
		{
			return s_rtx.capturedMask[i];
		}
	}
	return 0;
}

static int Rtx_PolyWords(u32 cmd)
{
	u32 code = cmd >> 24;
	int n = (code & 0x08) ? 4 : 3;
	return 1 + n * (1 + ((code & 0x04) != 0)) + ((code & 0x10) ? n - 1 : 0);
}

// GL parse of a partly captured packet: words to skip at cmd (0 = draw it)
int Rtx_SkipCommand(u32 *packet, u32 *cmd)
{
	if (s_rtx.ctxMask == 0)
	{
		return 0;
	}
	u32 offset = (u32)(cmd - (packet + 1));
	if ((offset >= 32) || !(s_rtx.ctxMask & (1u << offset)))
	{
		return 0;
	}
	return Rtx_PolyWords(*cmd);
}

static void Rtx_Invert3(const float *m, float *out)
{
	float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
	float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	float det = a * A + b * B + c * C;
	float inv = (fabsf(det) > 1e-12f) ? 1.0f / det : 0.0f;
	out[0] = A * inv;
	out[1] = -(b * i - c * h) * inv;
	out[2] = (b * f - c * e) * inv;
	out[3] = B * inv;
	out[4] = (a * i - c * g) * inv;
	out[5] = -(a * f - c * d) * inv;
	out[6] = C * inv;
	out[7] = -(a * h - b * g) * inv;
	out[8] = (a * e - b * d) * inv;
}

// a player's view (pushBuffer index), -1 for any other push buffer (HUD, menus)
static int Rtx_PbView(const struct PushBuffer *pb)
{
	struct GameTracker *gGT = sdata->gGT;
	for (int v = 0; v < (int)RTX_MAX_VIEWS; v++)
	{
		if (pb == &gGT->pushBuffer[v])
		{
			return (v < gGT->numPlyrCurrGame) ? v : -1;
		}
	}
	return -1;
}

// camera for view -> world of everything this view captures this frame (models
// are drawn before the level, whose hook sets the same camera again); makes it
// the view being recorded
static void Rtx_CaptureCamera(struct PushBuffer *pb)
{
	int v = Rtx_PbView(pb);
	v = (v < 0) ? 0 : v;
	s_rtx.view = v;
	if (s_rtx.camForCapture & (1 << v))
	{
		return;
	}
	const MATRIX *m = &pb->matrix_ViewProj;
	float camR[9];
	for (int r = 0; r < 3; r++)
	{
		for (int c = 0; c < 3; c++)
		{
			camR[r * 3 + c] = m->m[r][c] / 4096.0f;
		}
		s_rtx.camT[v][r] = (float)m->t[r];
	}
	Rtx_Invert3(camR, s_rtx.rInv[v]);
	s_rtx.camForCapture |= 1 << v;
}

// split screen: whether key (an instance, a kart's wheels) was traced from
// another view this frame; else this view traces it
static int Rtx_DrawnByOtherView(const void *key)
{
	const int size = (int)(sizeof(s_rtx.drawnBy) / sizeof(s_rtx.drawnBy[0]));
	u32 h = ((u32)key * 0x9E3779B1u) >> 22;
	for (int probe = 0; probe < 64; probe++)
	{
		int i = (int)((h + (u32)probe) & (u32)(size - 1));
		if (s_rtx.drawnBy[i].gen != s_rtx.gen)
		{
			s_rtx.drawnBy[i].gen = s_rtx.gen;
			s_rtx.drawnBy[i].key = key;
			s_rtx.drawnBy[i].view = s_rtx.view;
			return 0;
		}
		if (s_rtx.drawnBy[i].key == key)
		{
			return s_rtx.drawnBy[i].view != s_rtx.view;
		}
	}
	return 0;
}

// one of the live level's own object instances (its InstDefs): their memory stays valid
static int Rtx_IsLevelInstance(const void *inst)
{
	const struct Level *level = sdata->gGT->level1;
	for (u32 i = 0; (level != NULL) && (level->ptrInstDefs != NULL) && (i < level->numInstances) && (i < 512); i++)
	{
		if (level->ptrInstDefs[i].ptrInstance == inst)
		{
			return 1;
		}
	}
	return 0;
}

// whether a model instance was drawn this frame (in any view)
static int Rtx_InstDrawnThisFrame(const void *key)
{
	const int size = (int)(sizeof(s_rtx.drawnBy) / sizeof(s_rtx.drawnBy[0]));
	u32 h = ((u32)key * 0x9E3779B1u) >> 22;
	for (int probe = 0; probe < 64; probe++)
	{
		int i = (int)((h + (u32)probe) & (u32)(size - 1));
		if (s_rtx.drawnBy[i].gen != s_rtx.gen)
		{
			return 0;
		}
		if (s_rtx.drawnBy[i].key == key)
		{
			return 1;
		}
	}
	return 0;
}

// the level overlays' viewport switch (DrawLevelOvr1P_SetViewportScratchContext):
// split screen draws every player's level in one pass, view by view
void Rtx_SetCaptureView(struct PushBuffer *pb)
{
	if (RTX_HDR != NULL)
	{
		int v = Rtx_PbView(pb);
		s_rtx.view = (v < 0) ? 0 : v;
	}
}

// Model instances project through an MVP whose view space is uniformly scaled
// (near instances x4, DRAW_HUGE /4, see RenderBucket_AdjustViewPositionForMvp);
// the scale falls out of the MVP translation vs the instance's true view position
// one of the level file's own models (its scenery, crates, fruit): the remaster's
// hide list never touches karts, drivers, weapons (their models come from elsewhere)
static int Rtx_LevelOwnsModel(const struct Model *model)
{
	const struct Level *level = sdata->gGT->level1;
	if ((level == NULL) || (level->ptrModelsPtrArray == NULL))
	{
		return 0;
	}
	for (u32 i = 0; (i < level->numModels) && (i < 256); i++)
	{
		if (level->ptrModelsPtrArray[i] == model)
		{
			return 1;
		}
	}
	return 0;
}

// instances of the models the renderer draws as remaster models
// (RtxHeader.remasterProxy, hidden from the PS1 path): their world matrices,
// whether or not the game draws them this frame (the renderer's view is wider)
static void Rtx_PublishProxies(void)
{
	u32 n = 0;
	const struct Level *level = sdata->gGT->level1;
	if (RTX_HDR->remasterActive && (RTX_HDR->remasterProxy[0] != 0) && (level != NULL) && (level->ptrInstDefs != NULL))
	{
		for (u32 i = 0; (i < level->numInstances) && (n < RTX_MAX_PROXY); i++)
		{
			const struct Instance *inst = level->ptrInstDefs[i].ptrInstance;
			if ((inst == NULL) || (inst->model == NULL) || (inst->flags & HIDE_MODEL))
			{
				continue;
			}
			int want = 0;
			for (int k = 0; k < 4; k++)
			{
				want |= (RTX_HDR->remasterProxy[k] != 0) && (RTX_HDR->remasterProxy[k] == inst->model->id);
			}
			if (!want)
			{
				continue;
			}
			struct RtxProxyInst *p = &RTX_HDR->proxy[n++];
			p->model = inst->model->id;
			const float scale[3] = {inst->scale.x / 4096.0f, inst->scale.y / 4096.0f, inst->scale.z / 4096.0f};
			for (int r = 0; r < 3; r++)
			{
				for (int c = 0; c < 3; c++)
				{
					p->m[r * 3 + c] = (float)inst->matrix.m[r][c] / 4096.0f * scale[c];
				}
				p->t[r] = (float)inst->matrix.t[r];
			}
		}
	}
	RTX_HDR->numProxy = n;
}

void Rtx_BeginInstance(struct PrimMem *primMem, struct Instance *inst, struct PushBuffer *pb, const s32 *mvpT)
{
	if ((RTX_HDR == NULL) || (Rtx_PbView(pb) < 0) || (inst->flags & SCREENSPACE_INSTANCE))
	{
		Rtx_BeginTag(RTX_TAG_NONE, primMem); // HUD models: GL
		return;
	}
	// transparent models stay GL-drawn (as on PS1); glowing ones also light the scene
	if ((inst->flags & GHOST_DRAW_TRANSPARENT) == GHOST_DRAW_TRANSPARENT)
	{
		s_rtx.instMode = RTX_INST_GL;
	}
	else if (inst->flags & (DRAW_TRANSPARENT | DEPTH_FADE))
	{
		s_rtx.instMode = RTX_INST_GLOW;
	}
	else
	{
		s_rtx.instMode = RTX_INST_CAPTURE;
	}
	if (RTX_HDR->remasterActive && (inst->model != NULL) && Rtx_LevelOwnsModel(inst->model))
	{
		for (int i = 0; i < 8; i++)
		{
			if ((RTX_HDR->remasterHide[i] != 0) && (RTX_HDR->remasterHide[i] == inst->model->id))
			{
				s_rtx.instMode = RTX_INST_HIDE;
			}
		}
	}
	// track select screenshots (CTR_SHOT_CLEAN): no karts
	if (MT_ShotClean() && (inst->thread != NULL) && ((inst->thread->modelIndex == DYNAMIC_PLAYER) || (inst->thread->modelIndex == DYNAMIC_ROBOT_CAR)))
	{
		s_rtx.instMode = RTX_INST_HIDE;
	}
	s_rtx.statInstMode[s_rtx.instMode]++;
	s_rtx.curModelId = (inst->model != NULL) ? inst->model->id : -1;

	const MATRIX *m = &pb->matrix_ViewProj;
	float rel[3] = {(float)(inst->matrix.t[0] - pb->pos.x), (float)(inst->matrix.t[1] - pb->pos.y), (float)(inst->matrix.t[2] - pb->pos.z)};
	float vTrue[3], dotTT = 0.0f, dotGT = 0.0f;
	for (int r = 0; r < 3; r++)
	{
		vTrue[r] = (m->m[r][0] * rel[0] + m->m[r][1] * rel[1] + m->m[r][2] * rel[2]) / 4096.0f;
		dotTT += vTrue[r] * vTrue[r];
		dotGT += vTrue[r] * (float)mvpT[r];
	}
	s_rtx.instScale = (dotTT > 1.0f && dotGT > 0.0f) ? dotTT / dotGT : 1.0f;
	Rtx_CaptureCamera(pb);
	s_rtx.dupDraw = Rtx_DrawnByOtherView(inst);
	s_rtx.curInst = inst;
	s_rtx.statInstances++;
	s_rtx.curKart = ((inst->thread != NULL) && ((inst->thread->modelIndex == DYNAMIC_PLAYER) || (inst->thread->modelIndex == DYNAMIC_ROBOT_CAR))) ? inst : NULL;
	Rtx_BeginTag(RTX_TAG_INSTANCE, primMem);
}

void Rtx_BeginScaled(struct PrimMem *primMem, struct PushBuffer *pb, float viewScale, const struct Instance *wheelsOf)
{
	if ((RTX_HDR == NULL) || (Rtx_PbView(pb) < 0))
	{
		Rtx_BeginTag(RTX_TAG_NONE, primMem);
		return;
	}
	Rtx_CaptureCamera(pb);
	s_rtx.dupDraw = Rtx_DrawnByOtherView((const u8 *)wheelsOf + 1); // (its wheels: a key apart from the body's)
	s_rtx.curWheels = wheelsOf;
	s_rtx.curModelId = -1;
	s_rtx.instScale = viewScale;
	s_rtx.allowSemi = 1;
	s_rtx.instMode = MT_ShotClean() ? RTX_INST_HIDE : RTX_INST_CAPTURE; // (screenshots: no kart wheels)
	Rtx_BeginTag(RTX_TAG_INSTANCE, primMem);
}

static u32 Rtx_CapHash(u32 sxy, u32 serial)
{
	u32 h = sxy * 0x9E3779B1u ^ serial * 0x85EBCA77u;
	h ^= h >> 15;
	return h & (RTX_CAP_SIZE - 1);
}

// Screen XY -> view position of what was projected there. Several vertices of
// one model can land on the same pixel (front and back of a character, a
// distant seal): each is kept, and polygons pick the set that fits (Rtx_CapLookupAll).
#define RTX_CAP_PROBES 32
#define RTX_CAP_CANDIDATES 4

// projections are kept by (screen xy, serial, view): split screen views project
// to their own viewport's coordinates
#define RTX_CAP_KEY(serial) ((u32)(serial) | ((u32)s_rtx.view << 16))

void Rtx_OnProject(u32 sxy, s32 mac1, s32 mac2, s32 mac3, int sf)
{
	u32 key = RTX_CAP_KEY(s_rtx.capSerial);
	u32 h = Rtx_CapHash(sxy, key);
	float scale = (sf ? 1.0f : (1.0f / 4096.0f)) * s_rtx.instScale;
	float v[3] = {mac1 * scale, mac2 * scale, mac3 * scale};

	for (int probe = 0; probe < RTX_CAP_PROBES; probe++)
	{
		struct RtxCapEntry *e = &s_rtx.cap[(h + probe) & (RTX_CAP_SIZE - 1)];
		if (e->gen != s_rtx.gen)
		{
			e->gen = s_rtx.gen;
			e->sxy = sxy;
			e->serial = key;
			memcpy(e->v, v, sizeof(v));
			return;
		}
		if ((e->sxy == sxy) && (e->serial == key) && (fabsf(e->v[0] - v[0]) + fabsf(e->v[1] - v[1]) + fabsf(e->v[2] - v[2]) < 1.0f))
		{
			return; // the same vertex again
		}
	}
}

// every view position projected to sxy (up to RTX_CAP_CANDIDATES), oldest first
static int Rtx_CapLookupAll(u32 sxy, u32 serial, const float **out)
{
	serial = RTX_CAP_KEY(serial);
	u32 h = Rtx_CapHash(sxy, serial);
	int n = 0;

	for (int probe = 0; (probe < RTX_CAP_PROBES) && (n < RTX_CAP_CANDIDATES); probe++)
	{
		struct RtxCapEntry *e = &s_rtx.cap[(h + probe) & (RTX_CAP_SIZE - 1)];
		if (e->gen != s_rtx.gen)
		{
			break;
		}
		if ((e->sxy == sxy) && (e->serial == serial))
		{
			out[n++] = e->v;
		}
	}
	return n;
}

static const float *Rtx_CapLookup(u32 sxy, u32 serial)
{
	const float *c[RTX_CAP_CANDIDATES];
	return Rtx_CapLookupAll(sxy, serial, c) ? c[0] : NULL;
}

// a polygon's vertices among their candidates: the set closest together in depth
// (a polygon's corners are neighbours on the model). 0 when a vertex wasn't projected
static int Rtx_CapPolygon(const u32 *xy, int n, u32 serial, const float **out)
{
	const float *cand[4][RTX_CAP_CANDIDATES];
	int count[4], combos = 1;
	for (int v = 0; v < n; v++)
	{
		count[v] = Rtx_CapLookupAll(xy[v], serial, cand[v]);
		if (count[v] == 0)
		{
			return 0;
		}
		combos *= count[v];
	}
	float best = 1e30f;
	for (int c = 0; c < combos; c++)
	{
		int k = c;
		const float *pick[4];
		float zmin = 1e30f, zmax = -1e30f;
		for (int v = 0; v < n; v++)
		{
			pick[v] = cand[v][k % count[v]];
			k /= count[v];
			zmin = fminf(zmin, pick[v][2]);
			zmax = fmaxf(zmax, pick[v][2]);
		}
		if (zmax - zmin < best)
		{
			best = zmax - zmin;
			memcpy(out, pick, sizeof(pick[0]) * n);
		}
	}
	return 1;
}

// ---- level upload ----------------------------------------------------------

// QuadBlock hi-LOD faces as (TL, TR, BL, BR) quadblock vertex slots, from
// overlay 226 (sDrawLevelOvr1PGridFaceIndices)
static const int sRtxGridFace[4][4] = {
    {0, 4, 5, 6},
    {4, 1, 6, 7},
    {5, 6, 2, 8},
    {6, 7, 8, 3},
};

static int Rtx_TexSlotFor(u32 ptr)
{
	if (ptr == 0)
	{
		return -1;
	}
	// small levels: linear search on a hash of recent slots
	static u32 hashKey[4096];
	static u16 hashVal[4096];
	static u32 hashGen[4096];
	static u32 gen;
	if (s_rtx.numTexSlots == 0)
	{
		gen++;
	}
	u32 h = (ptr * 0x9E3779B1u) >> 20;
	for (int probe = 0; probe < 4096; probe++)
	{
		u32 i = (h + probe) & 4095;
		if (hashGen[i] != gen)
		{
			if (s_rtx.numTexSlots >= (int)RTX_MAX_TEXSRC)
			{
				return -1;
			}
			hashGen[i] = gen;
			hashKey[i] = ptr;
			hashVal[i] = (u16)s_rtx.numTexSlots;
			s_rtx.texSrc[s_rtx.numTexSlots] = ptr;
			s_rtx.texBank[s_rtx.numTexSlots] = (u8)s_rtx.curTexBank;
			return s_rtx.numTexSlots++;
		}
		if (hashKey[i] == ptr)
		{
			return hashVal[i];
		}
	}
	return -1;
}

static void Rtx_RefreshTexSlots(void)
{
	struct RtxTexSlot *slots = Rtx_TexSlots();

	for (int i = 0; i < s_rtx.numTexSlots; i++)
	{
		u32 ptr = s_rtx.texSrc[i];
		const struct IconGroup4 *group;
		const struct TextureLayout *t;
		if (ptr & 2)
		{
			t = (const struct TextureLayout *)(ptr & ~3u);
			goto haveLayout;
		}
		if (ptr & 1)
		{
			group = *(const struct IconGroup4 **)(ptr - 1); // AnimTex: active frame
		}
		else
		{
			group = (const struct IconGroup4 *)ptr;
		}
		if (group == NULL)
		{
			continue;
		}
		t = &group->near;
	haveLayout:;
		struct RtxTexSlot *s = &slots[i];
		s->uv[0][0] = t->u0;
		s->uv[0][1] = t->v0;
		s->uv[1][0] = t->u1;
		s->uv[1][1] = t->v1;
		s->uv[2][0] = t->u2;
		s->uv[2][1] = t->v2;
		s->uv[3][0] = t->u3;
		s->uv[3][1] = t->v3;
		s->clut = t->clut;
		s->tpage = (u16)(t->tpage | (s_rtx.texBank[i] << RTX_TPAGE_BANK_SHIFT));
	}
	RTX_HDR->numTexSlots = (u32)s_rtx.numTexSlots;
}

// the level's sky colours by direction (8 azimuths x 8 elevations, top row up),
// from its skybox mesh vertex colours, else its clear colours
static void Rtx_UploadSky(struct Level *level)
{
	float sum[RTX_SKY_H][RTX_SKY_W][4];
	memset(sum, 0, sizeof(sum));
	const struct Skybox *sky = level->ptr_skybox;
	int n = 0;
	if ((sky != NULL) && (sky->ptrVertex != NULL) && (sky->numVertex > 0) && (sky->numVertex < 65536))
	{
		for (int i = 0; i < sky->numVertex; i++)
		{
			const struct ShortVertex *v = &sky->ptrVertex[i];
			float x = v->Position.vx, y = v->Position.vy, z = v->Position.vz;
			float len = sqrtf(x * x + y * y + z * z);
			if (len < 1.0f)
			{
				continue;
			}
			// CTR world is Y up
			float el = asinf(fmaxf(-1.0f, fminf(1.0f, y / len)));
			int row = (int)((0.5f - el / 3.14159265f) * RTX_SKY_H);
			int col = (int)((atan2f(z, x) / 6.2831853f + 0.5f) * RTX_SKY_W);
			row = row < 0 ? 0 : (row >= (int)RTX_SKY_H ? (int)RTX_SKY_H - 1 : row);
			col = col < 0 ? 0 : (col >= (int)RTX_SKY_W ? (int)RTX_SKY_W - 1 : col);
			sum[row][col][0] += v->Color.r / 255.0f;
			sum[row][col][1] += v->Color.g / 255.0f;
			sum[row][col][2] += v->Color.b / 255.0f;
			sum[row][col][3] += 1.0f;
			n++;
		}
	}
	if (n == 0)
	{
		// clear colours: 0 top half, 1 bottom half
		for (int half = 0; half < 2; half++)
		{
			if (!level->clearColor[half].enable)
			{
				continue;
			}
			for (int row = half * RTX_SKY_H / 2; row < (half + 1) * (int)RTX_SKY_H / 2; row++)
			{
				for (int col = 0; col < (int)RTX_SKY_W; col++)
				{
					for (int k = 0; k < 3; k++)
					{
						sum[row][col][k] = (u8)level->clearColor[half].rgb[k] / 255.0f;
					}
					sum[row][col][3] = 1.0f;
					n++;
				}
			}
		}
	}
	RTX_HDR->skyValid = (n > 0);
	if (n == 0)
	{
		return;
	}
	// empty cells: nearest filled cell (rows first, then around the row)
	for (int row = 0; row < (int)RTX_SKY_H; row++)
	{
		for (int col = 0; col < (int)RTX_SKY_W; col++)
		{
			const float *src = NULL;
			for (int d = 0; (d < (int)(RTX_SKY_H + RTX_SKY_W)) && (src == NULL); d++)
			{
				for (int dr = -d; (dr <= d) && (src == NULL); dr++)
				{
					int r = row + dr, c = (col + (d - abs(dr))) % (int)RTX_SKY_W;
					if ((r >= 0) && (r < (int)RTX_SKY_H) && (sum[r][c][3] > 0.0f))
					{
						src = sum[r][c];
					}
				}
			}
			for (int k = 0; k < 3; k++)
			{
				RTX_HDR->sky[row][col][k] = src ? src[k] / src[3] : 0.0f;
			}
		}
	}
}

// ---- the traced floor under the karts -----------------------------------------
// The PS1 draws a kart over the floor it stands on (no depth buffer), so where
// the physics puts it a little off the drawn floor nobody sees it. Traced, the
// wheels sink into the floor in one place and float over it in the next: each
// kart is set onto the traced level mesh by its wheels (Rtx_SeatKart). A 2D
// grid of the level's floor-like triangles answers "how high is the floor here"

#define RTX_FLOOR_CELL_MAX  (512 * 512)
#define RTX_FLOOR_LIST_MAX  (1 << 21)
static struct
{
	float minX, minZ, cell;
	int dimX, dimZ;
	u32 start[RTX_FLOOR_CELL_MAX + 1];
	u32 list[RTX_FLOOR_LIST_MAX];
	const struct RtxLevelVert *verts;
	const struct RtxLevelTri *tris;
} s_floor;

static void Rtx_BuildFloorGrid(const struct RtxLevelVert *verts, int numVerts, const struct RtxLevelTri *tris, int numTris)
{
	s_floor.verts = verts;
	s_floor.tris = tris;
	s_floor.dimX = s_floor.dimZ = 0;
	if (numVerts <= 0)
	{
		return;
	}
	float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
	for (int i = 0; i < numVerts; i++)
	{
		minX = fminf(minX, verts[i].pos[0]);
		maxX = fmaxf(maxX, verts[i].pos[0]);
		minZ = fminf(minZ, verts[i].pos[2]);
		maxZ = fmaxf(maxZ, verts[i].pos[2]);
	}
	float cell = 256.0f;
	while (((maxX - minX) / cell + 1.0f) * ((maxZ - minZ) / cell + 1.0f) > (float)RTX_FLOOR_CELL_MAX)
	{
		cell *= 2.0f;
	}
	s_floor.minX = minX;
	s_floor.minZ = minZ;
	s_floor.cell = cell;
	s_floor.dimX = (int)((maxX - minX) / cell) + 1;
	s_floor.dimZ = (int)((maxZ - minZ) / cell) + 1;
	int cells = s_floor.dimX * s_floor.dimZ;
	memset(s_floor.start, 0, sizeof(u32) * (size_t)(cells + 1));
	// two passes: count per cell, then fill (floor-like triangles only)
	for (int pass = 0; pass < 2; pass++)
	{
		for (int t = 0; t < numTris; t++)
		{
			const float *a = verts[tris[t].v[0]].pos, *b = verts[tris[t].v[1]].pos, *c = verts[tris[t].v[2]].pos;
			float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
			float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			if (fabsf(ny) < 0.35f * sqrtf(nx * nx + ny * ny + nz * nz))
			{
				continue; // walls
			}
			int x0 = (int)((fminf(a[0], fminf(b[0], c[0])) - minX) / cell), x1 = (int)((fmaxf(a[0], fmaxf(b[0], c[0])) - minX) / cell);
			int z0 = (int)((fminf(a[2], fminf(b[2], c[2])) - minZ) / cell), z1 = (int)((fmaxf(a[2], fmaxf(b[2], c[2])) - minZ) / cell);
			for (int z = z0; z <= z1; z++)
			{
				for (int x = x0; x <= x1; x++)
				{
					u32 k = (u32)(z * s_floor.dimX + x);
					if (pass == 0)
					{
						s_floor.start[k + 1]++;
					}
					else if (s_floor.start[k] < RTX_FLOOR_LIST_MAX)
					{
						s_floor.list[s_floor.start[k]++] = (u32)t;
					}
				}
			}
		}
		if (pass == 0)
		{
			for (int k = 0; k < cells; k++)
			{
				s_floor.start[k + 1] += s_floor.start[k];
			}
		}
		else
		{
			// (filling advanced each start to the next cell's: shift back)
			for (int k = cells; k > 0; k--)
			{
				s_floor.start[k] = s_floor.start[k - 1];
			}
			s_floor.start[0] = 0;
		}
	}
}

// Nitro-Fueled tracks: the remaster's drivable surface (scripts/nf_level.py
// .floor), a height field of up to `layers` surfaces per cell, loaded once the
// renderer has the remaster mesh up (RtxHeader.remasterActive)
static struct
{
	char key[64];
	float minX, minZ, cell;
	int dimX, dimZ, layers;
	s16 *h;
} s_nfFloor;

static int Rtx_NfFloorReady(void)
{
	if (!RTX_HDR->remasterActive)
	{
		return 0;
	}
	if (strcmp(s_nfFloor.key, RTX_HDR->lightsKey) != 0)
	{
		free(s_nfFloor.h);
		memset(&s_nfFloor, 0, sizeof(s_nfFloor));
		snprintf(s_nfFloor.key, sizeof(s_nfFloor.key), "%s", RTX_HDR->lightsKey);
		char path[1024];
		snprintf(path, sizeof(path), "%sassets/rtx_remaster/%s.floor", SDL_GetBasePath(), s_nfFloor.key);
		FILE *f = fopen(path, "rb");
		struct
		{
			char magic[4];
			u32 version;
			float minX, minZ, cell;
			u32 dimX, dimZ, layers;
		} hd;
		if (f && (fread(&hd, sizeof(hd), 1, f) == 1) && !memcmp(hd.magic, "RTXF", 4) && (hd.version == 1) && (hd.cell > 0.0f) && (hd.dimX > 0) &&
		    (hd.dimZ > 0) && (hd.layers > 0) && (hd.layers <= 8) && ((u64)hd.dimX * hd.dimZ * hd.layers < (64u << 20)))
		{
			size_t n = (size_t)hd.dimX * hd.dimZ * hd.layers;
			s_nfFloor.h = malloc(n * sizeof(s16));
			if (s_nfFloor.h && (fread(s_nfFloor.h, sizeof(s16), n, f) == n))
			{
				s_nfFloor.minX = hd.minX;
				s_nfFloor.minZ = hd.minZ;
				s_nfFloor.cell = hd.cell;
				s_nfFloor.dimX = (int)hd.dimX;
				s_nfFloor.dimZ = (int)hd.dimZ;
				s_nfFloor.layers = (int)hd.layers;
				printf("[RTX] Nitro-Fueled floor %s: %dx%d cells of %.0f\n", path, s_nfFloor.dimX, s_nfFloor.dimZ, s_nfFloor.cell);
			}
			else
			{
				free(s_nfFloor.h);
				s_nfFloor.h = NULL;
			}
		}
		if (f)
		{
			fclose(f);
		}
	}
	return s_nfFloor.h != NULL;
}

// one cell's surface nearest to y within range
static int Rtx_NfFloorCell(int cx, int cz, float y, float range, float *h)
{
	if ((cx < 0) || (cz < 0) || (cx >= s_nfFloor.dimX) || (cz >= s_nfFloor.dimZ))
	{
		return 0;
	}
	const s16 *c = &s_nfFloor.h[((size_t)cz * s_nfFloor.dimX + cx) * s_nfFloor.layers];
	int found = 0;
	float best = range;
	for (int i = 0; i < s_nfFloor.layers; i++)
	{
		if ((c[i] != -32768) && (fabsf(c[i] - y) < best))
		{
			best = fabsf(c[i] - y);
			*h = c[i];
			found = 1;
		}
	}
	return found;
}

// the remaster floor at (x, z) nearest to y: bilinear over the cell centres
// around it, or the nearest one found
static int Rtx_NfFloorHeight(float x, float y, float z, float range, float *h)
{
	float fx = (x - s_nfFloor.minX) / s_nfFloor.cell - 0.5f, fz = (z - s_nfFloor.minZ) / s_nfFloor.cell - 0.5f;
	int x0 = (int)floorf(fx), z0 = (int)floorf(fz);
	float wx = fx - x0, wz = fz - z0;
	float c[4], w[4] = {(1 - wx) * (1 - wz), wx * (1 - wz), (1 - wx) * wz, wx * wz};
	float sum = 0.0f, wsum = 0.0f;
	for (int i = 0; i < 4; i++)
	{
		if (Rtx_NfFloorCell(x0 + (i & 1), z0 + (i >> 1), y, range * 2.0f, &c[i]))
		{
			sum += c[i] * w[i];
			wsum += w[i];
		}
	}
	if (wsum < 1e-4f)
	{
		return 0;
	}
	float hy = sum / wsum;
	if (fabsf(hy - y) >= range)
	{
		return 0;
	}
	*h = hy;
	return 1;
}

static int Rtx_Ps1FloorHeight(float x, float y, float z, float range, float *h);

// height of the traced floor at (x, z) nearest to y within range; 0 = none
// (Nitro-Fueled tracks: the remaster's floor where it has one)
static int Rtx_FloorHeight(float x, float y, float z, float range, float *h)
{
	if (Rtx_NfFloorReady() && Rtx_NfFloorHeight(x, y, z, range, h))
	{
		return 1;
	}
	return Rtx_Ps1FloorHeight(x, y, z, range, h);
}

static int Rtx_InstDrawnThisFrame(const void *inst);

static FILE *Rtx_ProbeFile(void)
{
	char path[1024];
	snprintf(path, sizeof(path), "%srtx_probe.txt", SDL_GetBasePath());
	return fopen(path, "a");
}

// world -> traced image pixel and view z (view 0), as trace.comp casts its camera rays
static int Rtx_ProbeProject(const float *p, u32 outW, u32 outH, int *px, int *py, float *z)
{
	const struct RtxView *V = &RTX_HDR->views[0];
	float v[3];
	for (int r = 0; r < 3; r++)
	{
		v[r] = V->camR[r * 3] * p[0] + V->camR[r * 3 + 1] * p[1] + V->camR[r * 3 + 2] * p[2] + V->camT[r];
	}
	*z = v[2];
	if (v[2] < 1.0f)
	{
		return 0;
	}
	float sx = V->vpX + 0.5f * V->vpW + v[0] * V->projH / v[2], sy = V->vpY + 0.5f * V->vpH + v[1] * V->projH / v[2];
	*px = (int)(sx / RTX_HDR->dispW * (float)outW);
	*py = (int)(sy / RTX_HDR->dispH * (float)outH);
	return (*px >= 0) && (*py >= 0) && (*px < (int)outW) && (*py < (int)outH);
}

static const char *s_probeBucketName[] = {"player", "robot", "ghost"};

// the game's side of a probe frame: camera, karts and ghosts (drawn or not, why
// not), the traced floor under them, dynamic triangles that are huge or broken
static void Rtx_ProbeGame(void)
{
	FILE *f = Rtx_ProbeFile();
	if (f == NULL)
	{
		return;
	}
	struct GameTracker *gGT = sdata->gGT;
	const struct RtxView *V = &RTX_HDR->views[0];
	float cam[3];
	for (int c = 0; c < 3; c++)
	{
		cam[c] = -(V->camR[c] * V->camT[0] + V->camR[3 + c] * V->camT[1] + V->camR[6 + c] * V->camT[2]);
	}
	fprintf(f, "\n==== probe: game frame %u, level %d, lights key %s, remaster %u, gameMode1 %08x, camera %.0f %.0f %.0f, debug view %d\n", RTX_HDR->frame + 1,
	        RTX_HDR->levelID, RTX_HDR->lightsKey, RTX_HDR->remasterActive, (u32)gGT->gameMode1, cam[0], cam[1], cam[2], s_rtx.debugView);
	fprintf(f, "models traced %d (dyn tris), level polygons captured %d, instance modes capture/glow/GL/hide %d/%d/%d/%d\n", s_rtx.numDyn, s_rtx.numLevelCap,
	        s_rtx.statInstMode[RTX_INST_CAPTURE], s_rtx.statInstMode[RTX_INST_GLOW], s_rtx.statInstMode[RTX_INST_GL], s_rtx.statInstMode[RTX_INST_HIDE]);
	for (int b = PLAYER; b <= GHOST; b++)
	{
		for (struct Thread *t = gGT->threadBuckets[b].thread; t != NULL; t = t->siblingThread)
		{
			struct Instance *inst = t->inst;
			if (inst == NULL)
			{
				continue;
			}
			const struct InstDrawPerPlayer *idpp = &inst->idpp[0];
			float p[3] = {(float)inst->matrix.t[0], (float)inst->matrix.t[1], (float)inst->matrix.t[2]};
			float h = 0.0f, ps1 = 0.0f;
			int haveNf = Rtx_FloorHeight(p[0], p[1], p[2], 200.0f, &h), havePs1 = Rtx_Ps1FloorHeight(p[0], p[1], p[2], 200.0f, &ps1);
			float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
			fprintf(f, "%s model %d at %.0f %.0f %.0f (%.0f from the camera): flags %08x, view 0 flags %08x, header %s, alpha %d, drawn %d; floor traced %s ps1 %s\n",
			        s_probeBucketName[b], (inst->model != NULL) ? inst->model->id : -1, p[0], p[1], p[2], sqrtf(dx * dx + dy * dy + dz * dz), (u32)inst->flags,
			        (u32)idpp->instFlags, (idpp->mh != NULL) ? "yes" : "none (not queued)", idpp->alphaScale, Rtx_InstDrawnThisFrame(inst),
			        haveNf ? "" : "none", havePs1 ? "" : "none");
			if (haveNf || havePs1)
			{
				fprintf(f, "    floor heights: traced %.1f, ps1 %.1f\n", h, ps1);
			}
		}
	}
	int big = 0;
	for (int i = 0; i < s_rtx.numDyn; i++)
	{
		const struct RtxDynTri *d = &Rtx_DynTris()[i];
		float m = 0.0f;
		int bad = 0;
		for (int a = 0; a < 3; a++)
		{
			for (int k = 0; k < 3; k++)
			{
				bad |= !isfinite(d->p[a][k]);
				float e = fabsf(d->p[a][k] - d->p[(a + 1) % 3][k]);
				m = (e > m) ? e : m;
			}
		}
		if ((bad || (m > 6000.0f)) && (big++ < 20))
		{
			fprintf(f, "huge dyn tri %d obj %d flags %x extent %.0f: %.0f %.0f %.0f / %.0f %.0f %.0f / %.0f %.0f %.0f\n", i, d->object, d->flags, m, d->p[0][0], d->p[0][1],
			        d->p[0][2], d->p[1][0], d->p[1][1], d->p[1][2], d->p[2][0], d->p[2][1], d->p[2][2]);
		}
	}
	fprintf(f, "huge or broken dyn tris: %d\n", big);
	fclose(f);
}

// once the probe frame's image is back: each kart / ghost against the traced
// depth round its spot (GL draws ghosts, discarded where z > depth x 1.02 + 8)
static void Rtx_ProbeDepth(const float *depth, u32 outW, u32 outH)
{
	FILE *f = Rtx_ProbeFile();
	if ((f == NULL) || (depth == NULL))
	{
		if (f != NULL)
		{
			fclose(f);
		}
		return;
	}
	struct GameTracker *gGT = sdata->gGT;
	for (int b = PLAYER; b <= GHOST; b++)
	{
		for (struct Thread *t = gGT->threadBuckets[b].thread; t != NULL; t = t->siblingThread)
		{
			struct Instance *inst = t->inst;
			if (inst == NULL)
			{
				continue;
			}
			// its body: a little over its origin (the wheels' bottom)
			float p[3] = {(float)inst->matrix.t[0], (float)inst->matrix.t[1] + 20.0f, (float)inst->matrix.t[2]};
			int px, py;
			float z;
			if (!Rtx_ProbeProject(p, outW, outH, &px, &py, &z))
			{
				fprintf(f, "%s: off screen (view z %.0f)\n", s_probeBucketName[b], z);
				continue;
			}
			float dmin = 1e30f, dmax = 0.0f;
			int pass = 0, n = 0;
			for (int y = py - 3; y <= py + 3; y++)
			{
				for (int x = px - 3; x <= px + 3; x++)
				{
					if ((x < 0) || (y < 0) || (x >= (int)outW) || (y >= (int)outH))
					{
						continue;
					}
					float d = depth[y * (int)outW + x];
					dmin = (d < dmin) ? d : dmin;
					dmax = (d > dmax) ? d : dmax;
					pass += (z <= d * 1.02f + 8.0f);
					n++;
				}
			}
			fprintf(f, "%s at pixel %d,%d view z %.0f: traced depth there %.0f..%.0f, overlay depth test passes %d of %d pixels\n", s_probeBucketName[b], px, py, z, dmin,
			        dmax, pass, n);
		}
	}
	fclose(f);
}

// Nitro-Fueled tracks: a captured model other than a kart (crates, fruit,
// obstacles, the level's other objects) stands on the PS1 floor; it is moved by
// how much higher or lower the remaster's floor is under it (as karts are)
static void Rtx_SeatInstance(int start, int end)
{
	if ((end <= start) || !Rtx_NfFloorReady())
	{
		return;
	}
	struct RtxDynTri *dyn = Rtx_DynTris();
	float low[3] = {0.0f, 1e30f, 0.0f}, cx = 0.0f, cz = 0.0f;
	int n = 0;
	for (int t = start; t < end; t++)
	{
		for (int c = 0; c < 3; c++)
		{
			const float *p = dyn[t].p[c];
			if (p[1] < low[1])
			{
				low[1] = p[1];
			}
			cx += p[0];
			cz += p[2];
			n++;
		}
	}
	low[0] = cx / n;
	low[2] = cz / n;
	float ps1, nf;
	if (!Rtx_Ps1FloorHeight(low[0], low[1], low[2], 150.0f, &ps1) || !Rtx_NfFloorHeight(low[0], ps1, low[2], 60.0f, &nf))
	{
		return;
	}
	float d = nf - ps1;
	if ((fabsf(d) < 0.5f) || (fabsf(d) > 60.0f))
	{
		return;
	}
	for (int t = start; t < end; t++)
	{
		for (int c = 0; c < 3; c++)
		{
			dyn[t].p[c][1] += d;
		}
	}
}

// the PS1 level's floor (s_floor) at (x, z) nearest to y within range; 0 = none
static int Rtx_Ps1FloorHeight(float x, float y, float z, float range, float *h)
{
	if (s_floor.dimX <= 0)
	{
		return 0;
	}
	int cx = (int)((x - s_floor.minX) / s_floor.cell), cz = (int)((z - s_floor.minZ) / s_floor.cell);
	if ((cx < 0) || (cz < 0) || (cx >= s_floor.dimX) || (cz >= s_floor.dimZ))
	{
		return 0;
	}
	u32 k = (u32)(cz * s_floor.dimX + cx);
	int found = 0;
	float best = range;
	for (u32 i = s_floor.start[k]; (i < s_floor.start[k + 1]) && (i < RTX_FLOOR_LIST_MAX); i++)
	{
		const struct RtxLevelTri *t = &s_floor.tris[s_floor.list[i]];
		const float *a = s_floor.verts[t->v[0]].pos, *b = s_floor.verts[t->v[1]].pos, *c = s_floor.verts[t->v[2]].pos;
		// barycentrics in the xz plane
		float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
		if (fabsf(d) < 1e-6f)
		{
			continue;
		}
		float wa = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
		float wb = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
		float wc = 1.0f - wa - wb;
		if ((wa < -1e-3f) || (wb < -1e-3f) || (wc < -1e-3f))
		{
			continue;
		}
		float hy = wa * a[1] + wb * b[1] + wc * c[1];
		if (fabsf(hy - y) < best)
		{
			best = fabsf(hy - y);
			*h = hy;
			found = 1;
		}
	}
	return found;
}

// a kart's wheels were just captured (dynamic triangles [wheelStart, wheelEnd),
// two per wheel quad): the lowest point of each wheel against the traced floor
// under it; grounded, the kart (wheels and body) moves by their mean gap
static void Rtx_SeatKart(const void *inst, int wheelStart, int wheelEnd)
{
	// (karts on the Nitro-Fueled collision already sit on the NF surfaces)
	if (MT_Coll_On())
	{
		return;
	}
	struct RtxDynTri *dyn = Rtx_DynTris();
	float gapSum = 0.0f;
	int n = 0, wheels = 0;
	for (int t = wheelStart; t + 1 < wheelEnd; t += 2)
	{
		const float *low = dyn[t].p[0];
		for (int q = t; q < t + 2; q++)
		{
			for (int c = 0; c < 3; c++)
			{
				low = (dyn[q].p[c][1] < low[1]) ? dyn[q].p[c] : low;
			}
		}
		float h;
		wheels++;
		if (Rtx_FloorHeight(low[0], low[1], low[2], 60.0f, &h))
		{
			gapSum += low[1] - h;
			n++;
		}
	}
	float target = 0.0f;
	if (n >= 2)
	{
		float gap = gapSum / (float)n;
		s_rtx.statGapN++;
		s_rtx.statGapSum += gap;
		s_rtx.statGapAbs += fabsf(gap);
		target = (fabsf(gap) < 40.0f) ? -gap : 0.0f; // (off the ground: as it is)
	}
	s_rtx.statGapFar += (n < 2) && (wheels > 0);

	// smoothed per kart (a step in the floor doesn't make it jump)
	int slot = -1, oldest = 0;
	for (int i = 0; i < 16; i++)
	{
		if (s_rtx.kartLift[i].inst == inst)
		{
			slot = i;
			break;
		}
		oldest = (s_rtx.kartLift[i].gen < s_rtx.kartLift[oldest].gen) ? i : oldest;
	}
	if ((slot < 0) || (s_rtx.kartLift[slot].gen + 1u != s_rtx.gen))
	{
		slot = (slot < 0) ? oldest : slot;
		s_rtx.kartLift[slot].inst = inst;
		s_rtx.kartLift[slot].dy = target;
	}
	else
	{
		s_rtx.kartLift[slot].dy += (target - s_rtx.kartLift[slot].dy) * 0.5f;
	}
	s_rtx.kartLift[slot].gen = s_rtx.gen;
	float dy = s_rtx.kartLift[slot].dy;
	if (dy == 0.0f)
	{
		return;
	}
	for (int t = wheelStart; t < wheelEnd; t++)
	{
		dyn[t].p[0][1] += dy;
		dyn[t].p[1][1] += dy;
		dyn[t].p[2][1] += dy;
	}
	for (int b = 0; b < s_rtx.numKartBody; b++)
	{
		if (s_rtx.kartBody[b].inst != inst)
		{
			continue;
		}
		s_rtx.statGapBody++;
		for (int t = s_rtx.kartBody[b].start; t < s_rtx.kartBody[b].end; t++)
		{
			dyn[t].p[0][1] += dy;
			dyn[t].p[1][1] += dy;
			dyn[t].p[2][1] += dy;
		}
	}
}

// every frame of every animated texture (boosters, lava, screens): their
// polygons among the level as the PS1 drew it are told by tpage, clut and uvs
static void Rtx_FindAnimatedTextures(void)
{
	s_rtx.numAnimRect = 0;
	for (int i = 0; i < s_rtx.numTexSlots; i++)
	{
		u32 ptr = s_rtx.texSrc[i];
		if ((ptr & 3) != 1)
		{
			continue;
		}
		const struct AnimTex *anim = (const struct AnimTex *)(ptr - 1);
		struct IconGroup4 **frames = ANIMTEX_GETARRAY(anim);
		for (int f = 0; (f < anim->numFrames) && (f < 64); f++)
		{
			const struct IconGroup4 *g = frames[f];
			if (g == NULL)
			{
				continue;
			}
			const struct TextureLayout *lods[4] = {&g->far, &g->middle, &g->near, &g->mosaic};
			for (int l = 0; (l < 4) && (s_rtx.numAnimRect < 1024); l++)
			{
				const struct TextureLayout *t = lods[l];
				u8 us[4] = {t->u0, t->u1, t->u2, t->u3}, vs[4] = {t->v0, t->v1, t->v2, t->v3};
				u8 u0 = 255, v0 = 255, u1 = 0, v1 = 0;
				for (int k = 0; k < 4; k++)
				{
					u0 = (us[k] < u0) ? us[k] : u0;
					u1 = (us[k] > u1) ? us[k] : u1;
					v0 = (vs[k] < v0) ? vs[k] : v0;
					v1 = (vs[k] > v1) ? vs[k] : v1;
				}
				if ((u1 <= u0) || (v1 <= v0))
				{
					continue;
				}
				s_rtx.animRect[s_rtx.numAnimRect].tpage = t->tpage;
				s_rtx.animRect[s_rtx.numAnimRect].clut = t->clut;
				s_rtx.animRect[s_rtx.numAnimRect].u0 = u0;
				s_rtx.animRect[s_rtx.numAnimRect].v0 = v0;
				s_rtx.animRect[s_rtx.numAnimRect].u1 = u1;
				s_rtx.animRect[s_rtx.numAnimRect].v1 = v1;
				s_rtx.numAnimRect++;
			}
		}
	}
}

static int Rtx_IsAnimatedTexture(u16 tpage, u16 clut, const u8 (*uv)[2])
{
	int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
	for (int k = 0; k < 3; k++)
	{
		u0 = (uv[k][0] < u0) ? uv[k][0] : u0;
		u1 = (uv[k][0] > u1) ? uv[k][0] : u1;
		v0 = (uv[k][1] < v0) ? uv[k][1] : v0;
		v1 = (uv[k][1] > v1) ? uv[k][1] : v1;
	}
	for (int i = 0; i < s_rtx.numAnimRect; i++)
	{
		// (blend bits aside: the PS1 may draw a page semi-transparent)
		if ((s_rtx.animRect[i].clut == clut) && ((s_rtx.animRect[i].tpage & 0xff9f) == (tpage & 0xff9f)) && (u0 + 1 >= s_rtx.animRect[i].u0) &&
		    (u1 <= s_rtx.animRect[i].u1 + 1) && (v0 + 1 >= s_rtx.animRect[i].v0) && (v1 <= s_rtx.animRect[i].v1 + 1))
		{
			return 1;
		}
	}
	return 0;
}

static int Rtx_HubWorldOn(void);

// the renderer's key for the level being played: stock-<levelID>, custom-<first
// bigfile entry>, nf-<slot> (Nitro-Fueled track: the stock level, remaster
// mesh assets/rtx_remaster/nf-<slot>.rtxm), nf-<id> (a Nitro-Fueled only track
// on a fan remake: pack_tracks.py puts the key in its author field, upper
// case), or hubs (the adventure hub world)
static void Rtx_LevelKey(char *key, size_t size)
{
	struct MTEntry *custom = MT_ActiveEntry();
	if (Rtx_HubWorldOn())
	{
		snprintf(key, size, "hubs");
	}
	else if (custom && (custom->flags & MT_FLAG_REMASTER) && !strncmp(custom->author, "NF-", 3))
	{
		size_t i = 0;
		for (; (i + 1 < size) && (i < MT_AUTHOR_LEN) && custom->author[i]; i++)
			key[i] = (char)tolower((unsigned char)custom->author[i]);
		key[i] = 0;
	}
	else if (custom && (custom->flags & MT_FLAG_REMASTER))
	{
		snprintf(key, size, "nf-%d", custom->slot);
	}
	else if (custom)
	{
		snprintf(key, size, "custom-%d", custom->bigBase);
	}
	else
	{
		snprintf(key, size, "stock-%d", sdata->gGT->levelID);
	}
}

// one level's mesh appended to the upload: its visible quadblocks' faces as
// the PS1 draws them (overlay 226's tables). hub >= 0: an adventure hub of the
// hub world (its texture bank hub + 1; triangles another hub already has, the
// tunnels neighbours share vertex for vertex, are left out)
struct RtxMeshBuild
{
	struct RtxLevelVert *verts;
	struct RtxLevelTri *tris;
	int numVerts, numTris;
	int skippedInvisible, waterQuads, waterTerrain, shared;
};

static int Rtx_SharedTri(const struct LevVertex *va, const struct LevVertex *vb, const struct LevVertex *vc, int hub)
{
	enum
	{
		SIZE = 1 << 18
	};
	static u64 key[SIZE];
	static u8 owner[SIZE];
	static u32 gen[SIZE], curGen;
	if (hub < 0)
	{
		curGen++; // (a new build)
		return 0;
	}
	// the corners' positions, in order
	u64 k[3];
	const struct LevVertex *v[3] = {va, vb, vc};
	for (int i = 0; i < 3; i++)
	{
		k[i] = ((u64)(u16)v[i]->pos.x << 32) | ((u64)(u16)v[i]->pos.y << 16) | (u64)(u16)v[i]->pos.z;
	}
	for (int i = 0; i < 2; i++)
	{
		for (int j = 0; j < 2 - i; j++)
		{
			if (k[j] > k[j + 1])
			{
				u64 t = k[j];
				k[j] = k[j + 1];
				k[j + 1] = t;
			}
		}
	}
	u64 h = (k[0] * 0x9E3779B97F4A7C15ull) ^ (k[1] * 0xC2B2AE3D27D4EB4Full) ^ (k[2] * 0x165667B19E3779F9ull);
	h = (h ^ (h >> 29)) | 1;
	for (u32 probe = 0; probe < 64; probe++)
	{
		u32 i = (u32)((h >> 20) + probe) & (SIZE - 1);
		if (gen[i] != curGen)
		{
			gen[i] = curGen;
			key[i] = h;
			owner[i] = (u8)hub;
			return 0;
		}
		if (key[i] == h)
		{
			return owner[i] != (u8)hub;
		}
	}
	return 0;
}

static void Rtx_AppendLevelMesh(const struct Level *level, int hub, struct RtxMeshBuild *b)
{
	const struct mesh_info *mesh = level->ptr_mesh_info;
	int base = b->numVerts;
	int numVerts = mesh->numVertex;
	if (base + numVerts > (int)RTX_MAX_LEVEL_VERTS)
	{
		numVerts = (int)RTX_MAX_LEVEL_VERTS - base;
	}
	for (int i = 0; i < numVerts; i++)
	{
		const struct LevVertex *v = &mesh->ptrVertexArray[i];
		struct RtxLevelVert *o = &b->verts[base + i];
		o->pos[0] = v->pos.x;
		o->pos[1] = v->pos.y;
		o->pos[2] = v->pos.z;
		o->rgba = (u32)v->color_hi[0] | ((u32)v->color_hi[1] << 8) | ((u32)v->color_hi[2] << 16) | (0xffu << 24);
	}
	b->numVerts = base + numVerts;

	// water: the quadblocks of BSP leaves flagged BSP_LEAF_FLAG_WATER
	static u8 isWater[1 << 16];
	int numQuads = mesh->numQuadBlock < (1 << 16) ? mesh->numQuadBlock : (1 << 16);
	memset(isWater, 0, sizeof(isWater));
	for (int bn = 0; (mesh->bspRoot != NULL) && (bn < mesh->numBspNodes); bn++)
	{
		const struct BSP *bsp = &mesh->bspRoot[bn];
		if (((bsp->flag & 1) != 0) && ((bsp->flag & BSP_LEAF_FLAG_WATER) != 0))
		{
			int first = (int)(bsp->data.leaf.ptrQuadBlockArray - mesh->ptrQuadBlockArray);
			for (int k = 0; k < bsp->data.leaf.numQuads; k++)
			{
				if ((first + k >= 0) && (first + k < numQuads))
				{
					isWater[first + k] = 1;
				}
			}
		}
	}

	s_rtx.curTexBank = hub + 1; // (texture slots made from here on: this level's VRAM)
	int waterSlot = (level->ptr_tex_waterEnvMap != NULL) ? Rtx_TexSlotFor((u32)level->ptr_tex_waterEnvMap | 2) : -1;

	for (int q = 0; q < numQuads; q++)
	{
		const struct QuadBlock *block = &mesh->ptrQuadBlockArray[q];
		int terrain = block->terrain_type;
		b->waterQuads += isWater[q];
		b->waterTerrain += (terrain == TERRAIN_WATER) || (terrain == TERRAIN_FASTWATER);
		// invisible walls and triggers: quadFlags 0x8000 (ctr-tools "invisible";
		// such blocks also have no draw order), or no texture and not water (water
		// is drawn with the level's env map instead of its own textures)
		int untextured = !block->ptr_texture_mid[0] && !block->ptr_texture_mid[1] && !block->ptr_texture_mid[2] && !block->ptr_texture_mid[3];
		if ((*(const u16 *)&block->quadFlags & 0x8000) || (untextured && !isWater[q]))
		{
			b->skippedInvisible++;
			continue;
		}
		u32 drawOrderLow = block->draw_order_low;

		for (int f = 0; f < 4; f++)
		{
			u32 faceBits = (drawOrderLow >> (8 + f * 5)) & 0x1f;
			u32 tableWord = R226.scratchInitTable[faceBits < OVR226_SCRATCH_INIT_WORD_COUNT ? faceBits : 0];
			// as DrawLevelOvr1P_WriteProjectedUv: bit 23 mirrors the texture (uv 0<->1, 2<->3),
			// bit 7 makes the face a triangle (no 4th vertex)
			int mirror = (tableWord & 0x00800000u) != 0;
			int triangle = (tableWord & 0x80u) != 0;
			int slotVert[4];
			for (int sl = 0; sl < 4; sl++)
			{
				u32 shift = (tableWord >> ((3 - sl) * 8)) & 0x1f;
				int k = (24 - (int)shift) / 8;
				slotVert[sl] = block->index[sRtxGridFace[f][k & 3]];
			}

			int texSlot = isWater[q] && (waterSlot >= 0) ? waterSlot : Rtx_TexSlotFor((u32)block->ptr_texture_mid[f]);
			int animated = !isWater[q] && (((u32)block->ptr_texture_mid[f] & 1) != 0);
			// (river asphalt: the shallow water karts drive through, drawn as a plain
			// texture, e.g. Sewer Speedway's: wet, so it reflects like these)
			int glossy = (terrain == TERRAIN_ICY_ROAD) || (terrain == TERRAIN_METAL) || (terrain == TERRAIN_RIVERASPHALT);
			int ice = (terrain == TERRAIN_ICE);
			static const int triSlots[2][3] = {{0, 1, 2}, {1, 3, 2}};
			static const u8 mirrorCorner[4] = {1, 0, 3, 2};
			for (int t = 0; t < (triangle ? 1 : 2); t++)
			{
				if (b->numTris >= (int)RTX_MAX_LEVEL_TRIS)
				{
					break;
				}
				struct RtxLevelTri *tri = &b->tris[b->numTris];
				int ok = 1, vi[3];
				for (int c = 0; c < 3; c++)
				{
					vi[c] = slotVert[triSlots[t][c]];
					ok &= (vi[c] < numVerts);
					tri->v[c] = (u16)(base + vi[c]);
					tri->corner[c] = mirror ? mirrorCorner[triSlots[t][c]] : (u8)triSlots[t][c];
				}
				if (!ok || (vi[0] == vi[1]) || (vi[1] == vi[2]) || (vi[0] == vi[2]))
				{
					continue;
				}
				if ((hub >= 0) &&
				    Rtx_SharedTri(&mesh->ptrVertexArray[vi[0]], &mesh->ptrVertexArray[vi[1]], &mesh->ptrVertexArray[vi[2]], hub))
				{
					b->shared++;
					continue; // (the neighbour's copy of a tunnel both hubs hold)
				}
				tri->texSlot = (u16)(texSlot < 0 ? 0xffff : texSlot);
				tri->flags = (texSlot >= 0 ? RTX_TRI_TEXTURED : 0) | ((drawOrderLow & 0x80000000u) ? RTX_TRI_DOUBLE_SIDED : 0) |
				             (isWater[q] ? RTX_TRI_WATER : 0) | (animated ? RTX_TRI_ANIMATED : 0) | (glossy ? RTX_TRI_GLOSSY : 0) |
				             (ice ? RTX_TRI_ICE : 0);
				b->numTris++;
			}
		}
	}
	s_rtx.curTexBank = 0;
}

// ---- adventure hub world ------------------------------------------------------
// The five hubs share one world frame (neighbours hold the tunnel between them
// vertex for vertex) but the game keeps two at a time (their textures use two
// alternating VRAM layouts) and swaps them in the tunnels. The renderer traces
// all five at once, from copies read off the disc here with each hub's own
// VRAM (RTX_OFF_HUB_VRAM): every hub is seen from anywhere, nothing pops in at
// a swap, and one level preparation covers them all (key "hubs"). The game's
// own hub swapping goes on underneath, for collision, objects and scripts.
#define RTX_HUB_COUNT 5
static struct
{
	int state; // 0 not read yet, 1 ready, -1 failed
	struct Level *level[RTX_HUB_COUNT];
} s_hub;

static int Rtx_IsHub(int levelID)
{
	return (levelID >= GEM_STONE_VALLEY) && (levelID <= CITADEL_CITY);
}

// in a hub, with the hub world read
static int Rtx_HubWorldOn(void)
{
	return Rtx_IsHub(sdata->gGT->levelID) && (s_hub.state > 0);
}

static u8 *Rtx_ReadBigfileEntry(const struct NativeDiscImageFile *big, int index, u32 *size)
{
	const struct BigEntry *e = BIG_GETENTRY(sdata->ptrBigfile1) + index;
	u32 sectors = ((u32)e->size + 2047) / 2048;
	u8 *buf = malloc(sectors * 2048 + 4);
	if ((buf == NULL) || !NativeDiscImage_ReadDataSectors(big, (u32)e->offset, sectors, buf))
	{
		free(buf);
		return NULL;
	}
	*size = (u32)e->size;
	return buf;
}

// a level file read off the disc made usable: the level struct starts after
// the file's size word; the pointer map (u32 bytes, then offsets from the
// level: the hubs' own file, or after the level in the same file) turns its
// offsets into pointers. NULL when the map doesn't fit
static struct Level *Rtx_PatchDiscLevel(u8 *lev, u32 levSize, const u8 *ptr, u32 ptrSize)
{
	if (levSize < 8)
	{
		return NULL;
	}
	u8 *origin = lev + 4;
	u32 bodySize = levSize - 4;
	if (ptr == NULL)
	{
		bodySize = *(const u32 *)lev;
		if ((bodySize > levSize - 8) || (bodySize & 3))
		{
			return NULL;
		}
		ptr = origin + bodySize;
		ptrSize = levSize - 4 - bodySize;
	}
	u32 numPtrs = (ptrSize >= 4) ? *(const u32 *)ptr / 4 : 0;
	if ((ptrSize < 4) || (4 + numPtrs * 4 > ptrSize))
	{
		return NULL;
	}
	for (u32 i = 0; i < numPtrs; i++)
	{
		u32 off = ((const u32 *)(ptr + 4))[i] & ~3u;
		if (off + 4 <= bodySize)
		{
			*(u32 *)(origin + off) += (u32)origin;
		}
	}
	return (struct Level *)origin;
}

// a level's texture file (.vrm: one TIM, or packed TIMs) written over a VRAM bank
static void Rtx_VrmToBank(u16 *bank, const u8 *vrm, u32 vrmSize)
{
	const u8 *vrmEnd = vrm + vrmSize;
	int packed = (vrmSize >= 8) && (*(const u32 *)vrm == 0x20);
	u32 recSize = packed ? *(const u32 *)(vrm + 4) : vrmSize;
	const u8 *rec = packed ? vrm + 8 : vrm;
	while ((recSize != 0) && (rec + sizeof(struct VramHeader) <= vrmEnd))
	{
		const struct VramHeader *vh = (const struct VramHeader *)rec;
		const u16 *pix = (const u16 *)VRAMHEADER_GETPIXLES(vh);
		for (int y = 0; y < vh->rect.h; y++)
		{
			int vy = vh->rect.y + y;
			if ((vy < 0) || (vy >= (int)RTX_VRAM_H) || ((const u8 *)(pix + (y + 1) * vh->rect.w) > vrmEnd))
			{
				break;
			}
			for (int x = 0; (x < vh->rect.w) && (vh->rect.x + x < (int)RTX_VRAM_W); x++)
			{
				bank[vy * RTX_VRAM_W + vh->rect.x + x] = pix[y * vh->rect.w + x];
			}
		}
		if (!packed)
		{
			break;
		}
		rec += recSize;
		recSize = (rec + 4 <= vrmEnd) ? *(const u32 *)rec : 0;
		rec += 4;
	}
}

// the hub world's copies dropped (their VRAM banks get reused: Rtx_UploadBakeLevel);
// read again on the next hub
static void Rtx_HubWorldForget(void)
{
	for (int h = 0; h < RTX_HUB_COUNT; h++)
	{
		if (s_hub.level[h] != NULL)
		{
			free((u8 *)s_hub.level[h] - 4);
			s_hub.level[h] = NULL;
		}
	}
	s_hub.state = 0;
	if (s_rtx.levelValid && Rtx_IsHub(s_rtx.levelID))
	{
		s_rtx.levelValid = 0;
	}
}

static int Rtx_HubWorldReady(void)
{
	if (s_hub.state != 0)
	{
		return s_hub.state > 0;
	}
	s_hub.state = -1;
	const char *env = SDL_getenv("CTR_RTX_HUBWORLD"); // 0: each hub on its own, as the game loads them
	if ((env != NULL) && (atoi(env) == 0))
	{
		return 0;
	}
	struct NativeDiscImageFile big;
	if ((sdata->ptrBigfile1 == NULL) || !NativeDiscImage_FindFile("BIGFILE.BIG", &big))
	{
		printf("[RTX] hub world: no BIGFILE.BIG\n");
		return 0;
	}
	u16 *banks = (u16 *)(s_rtx.base + RTX_OFF_HUB_VRAM);
	const u16 *live = NativeRenderer_RtxVRAM();
	for (int h = 0; h < RTX_HUB_COUNT; h++)
	{
		int first = BI_ADVENTUREHUB + h * LOAD_CUTSCENE_FILES_PER_LEVEL;
		u32 levSize, ptrSize, vrmSize;
		u8 *lev = Rtx_ReadBigfileEntry(&big, first + LVI_LEV, &levSize);
		u8 *ptr = Rtx_ReadBigfileEntry(&big, first + LVI_PTR, &ptrSize);
		u8 *vrm = Rtx_ReadBigfileEntry(&big, first + LVI_VRAM, &vrmSize);
		if ((lev == NULL) || (ptr == NULL) || (vrm == NULL) || (levSize < 8) || (ptrSize < 4))
		{
			printf("[RTX] hub world: hub %d unreadable\n", GEM_STONE_VALLEY + h);
			return 0;
		}
		s_hub.level[h] = Rtx_PatchDiscLevel(lev, levSize, ptr, ptrSize);
		free(ptr);
		if (s_hub.level[h] == NULL)
		{
			printf("[RTX] hub world: hub %d pointer map broken\n", GEM_STONE_VALLEY + h);
			free(lev);
			free(vrm);
			return 0;
		}
		// its VRAM: what is loaded now (shared pages) with its own texture file on top
		u16 *bank = banks + (size_t)h * RTX_VRAM_W * RTX_VRAM_H;
		memcpy(bank, live, RTX_VRAM_W * RTX_VRAM_H * 2);
		Rtx_VrmToBank(bank, vrm, vrmSize);
		free(vrm);
	}
	s_hub.state = 1;
	printf("[RTX] hub world: the %d adventure hubs read for tracing\n", RTX_HUB_COUNT);
	return 1;
}

static void Rtx_UploadLevel(struct Level *level)
{
	struct GameTracker *gGT = sdata->gGT;
	struct RtxMeshBuild b;
	memset(&b, 0, sizeof(b));
	b.verts = Rtx_LevelVerts();
	b.tris = Rtx_LevelTris();
	s_rtx.numTexSlots = 0; // also resets Rtx_TexSlotFor's lookup
	int hubWorld = Rtx_IsHub(gGT->levelID) && Rtx_HubWorldReady();
	if (hubWorld)
	{
		Rtx_SharedTri(NULL, NULL, NULL, -1);
		for (int h = 0; h < RTX_HUB_COUNT; h++)
		{
			int first = b.numVerts;
			Rtx_AppendLevelMesh(s_hub.level[h], h, &b);
			if (GEM_STONE_VALLEY + h == N_SANITY_BEACH)
			{
				// one sun for the whole world: each hub's art was lit with its own,
				// none fits all five; the beach's (outdoors, where adventure starts)
				RTX_HDR->sunFitFirst = (u32)first;
				RTX_HDR->sunFitCount = (u32)(b.numVerts - first);
			}
		}
	}
	else
	{
		RTX_HDR->sunFitFirst = RTX_HDR->sunFitCount = 0;
		Rtx_AppendLevelMesh(level, -1, &b);
	}

	Rtx_BuildFloorGrid(b.verts, b.numVerts, b.tris, b.numTris);
	Rtx_FindAnimatedTextures();

	struct MTEntry *custom = MT_ActiveEntry();
	RTX_HDR->numLevelVerts = (u32)b.numVerts;
	RTX_HDR->numLevelTris = (u32)b.numTris;
	RTX_HDR->levelID = gGT->levelID;
	RTX_HDR->redirectBase = custom ? custom->bigBase : -1;
	RTX_HDR->vramBanks = hubWorld ? RTX_VRAM_BANKS : 0;
	Rtx_LevelKey(RTX_HDR->lightsKey, sizeof(RTX_HDR->lightsKey));
	Rtx_RefreshTexSlots();
	Rtx_UploadSky(level);
	RTX_HDR->levelGen++;

	s_rtx.levelPtr = level;
	s_rtx.meshPtr = level->ptr_mesh_info;
	s_rtx.levelID = gGT->levelID;
	s_rtx.levelValid = 1;
	printf("[RTX] level %d (%s): %d verts, %d tris, %d texture slots, %d invisible quads skipped; water: %d quads (%d water terrain), %d vertices, "
	       "config 0x%x%s\n",
	       gGT->levelID, RTX_HDR->lightsKey, b.numVerts, b.numTris, s_rtx.numTexSlots, b.skippedInvisible, b.waterQuads, b.waterTerrain,
	       level->numWaterVertices, (unsigned)level->configFlags, hubWorld ? "; hub world: all 5 hubs" : "");
	if (hubWorld)
	{
		printf("[RTX] hub world: %d triangles of tunnels two hubs share left out\n", b.shared);
	}
}

// levels the renderer traces: race / battle tracks, the Adventure hubs (boss
// intros and the podium play there too) and the Adventure character select
// garage (a MAIN_MENU level), the intro movie's levels; the end of race too.
// Menus and the other story cutscenes keep the PS1 look
static int Rtx_LevelTraced(struct GameTracker *gGT)
{
	if (gGT->levelID == ADVENTURE_GARAGE)
	{
		return (gGT->gameMode1 & (GAME_CUTSCENE | END_OF_RACE)) == 0;
	}
	if ((gGT->levelID >= INTRO_RACE_TODAY) && (gGT->levelID <= INTRO_OXIDE))
	{
		return 1; // the intro movie (Oxide's arrival)
	}
	// (the end of race too: the AI drives on over the traced track)
	return (gGT->levelID <= CITADEL_CITY) && ((gGT->gameMode1 & (MAIN_MENU | GAME_CUTSCENE)) == 0);
}

void Rtx_OnLevelCamera(struct PushBuffer *pb)
{
	struct GameTracker *gGT = sdata->gGT;
	struct Level *level = gGT->level1;

	int view = Rtx_PbView(pb);
	if ((RTX_HDR == NULL) || (level == NULL) || (level->ptr_mesh_info == NULL) || (view < 0) || !Rtx_LevelTraced(gGT) || (sdata->pause_state != 0))
	{
		return;
	}
	// (a Nitro-Fueled track and its stock level, or two custom tracks on one slot,
	// load the same level into the same memory: the key tells them apart)
	if (Rtx_IsHub(gGT->levelID))
	{
		Rtx_HubWorldReady(); // (read once, on the first hub)
	}
	char key[sizeof(RTX_HDR->lightsKey)];
	Rtx_LevelKey(key, sizeof(key));
	int sameLevel = (s_rtx.levelPtr == level) && (s_rtx.meshPtr == level->ptr_mesh_info) && (s_rtx.levelID == gGT->levelID);
	if (s_rtx.levelValid && !sameLevel && Rtx_HubWorldOn() && !strcmp(key, (const char *)RTX_HDR->lightsKey))
	{
		// a hub swap inside the hub world: the traced world stays as it is; only
		// the live level's animated textures (the texture layer's boosters) change
		s_rtx.levelPtr = level;
		s_rtx.meshPtr = level->ptr_mesh_info;
		s_rtx.levelID = gGT->levelID;
		RTX_HDR->levelID = gGT->levelID;
		Rtx_FindAnimatedTextures();
		printf("[RTX] hub world: now in hub %d (no re-upload)\n", gGT->levelID);
	}
	else if (!sameLevel || !s_rtx.levelValid || strcmp(key, (const char *)RTX_HDR->lightsKey))
	{
		Rtx_UploadLevel(level);
	}

	// this player's view (split screen: each player's, every frame)
	struct RtxView *rv = &RTX_HDR->views[view];
	const MATRIX *m = &pb->matrix_ViewProj;
	for (int r = 0; r < 3; r++)
	{
		for (int c = 0; c < 3; c++)
		{
			rv->camR[r * 3 + c] = m->m[r][c] / 4096.0f;
		}
		rv->camT[r] = (float)m->t[r];
		s_rtx.camT[view][r] = (float)m->t[r];
	}
	Rtx_Invert3(rv->camR, s_rtx.rInv[view]);
	s_rtx.camForCapture |= 1 << view;
	rv->projH = (float)pb->distanceToScreen_PREV;
	rv->vpX = pb->rect.x;
	rv->vpY = pb->rect.y;
	rv->vpW = pb->rect.w;
	rv->vpH = pb->rect.h;
	rv->texNearDepth = 0.0f; // (1P: Rtx_SetTextureNearDepth)
	s_rtx.view = view;
	s_rtx.otBase[view] = (u32)pb->ptrOT;
	s_rtx.uiOtStart = (u32)gGT->pushBuffer_UI.ptrOT;
	s_rtx.uiOtEnd = (u32)gGT->pushBuffer_UI.renderBucketOTRangeEnd;
	s_rtx.cameraValid |= 1 << view;
	s_rtx.numViews = gGT->numPlyrCurrGame;
	RTX_HDR->numViews = (u32)s_rtx.numViews;
}

// ---- GPU side ----------------------------------------------------------------

int Rtx_FrameActive(void)
{
	// (every player's view: their cameras all came this frame)
	return (RTX_HDR != NULL) && Rtx_Drawing() && RTX_HDR->rendererReady && (s_rtx.numViews > 0) && (s_rtx.cameraValid == (1 << s_rtx.numViews) - 1) &&
	       s_rtx.levelValid;
}

// The level's LOD distances (texture detail, the far low-detail list) are tuned
// for the PS1's 512 pixel wide screen: at the output resolution a texel covers
// fewer pixels, so the same detail per pixel reaches out that much further.
// The texture layer takes its texels from what the PS1 drew, so this is what
// sharpens the distance (CTR_RTX_LOD_SCALE overrides, 1 = as the PS1)
// the level's texture detail threshold this frame (view z; MainFrame_RenderFrame):
// past it the PS1 draws its middle / far textures, the renderer takes the
// level mesh's near ones there instead of the texture layer (no texture pop-in)
void Rtx_SetTextureNearDepth(int depth)
{
	if (RTX_HDR != NULL)
	{
		RTX_HDR->views[0].texNearDepth = (float)depth;
	}
}

int Rtx_LevelLodScale(int depth)
{
	if ((RTX_HDR == NULL) || !Rtx_Drawing() || !RTX_HDR->rendererReady || !s_rtx.levelValid)
	{
		return depth;
	}
	static float scale;
	if (scale == 0.0f)
	{
		const char *env = SDL_getenv("CTR_RTX_LOD_SCALE");
		scale = env ? (float)atof(env) : -1.0f;
	}
	float k = scale;
	if (k <= 0.0f)
	{
		int w, h;
		NativeRenderer_RtxViewportSize(&w, &h);
		k = (float)w / 512.0f;
	}
	k = fminf(fmaxf(k, 1.0f), 4.0f);
	float d = (float)depth * k;
	return (d > 2.0e9f) ? 2000000000 : (int)d;
}

static int Rtx_RangeCmp(const void *a, const void *b)
{
	const struct RtxRange *ra = a, *rb = b;
	return (ra->start > rb->start) - (ra->start < rb->start);
}

static const struct RtxRange *Rtx_FindRange(u32 addr)
{
	if (!s_rtx.rangesSorted)
	{
		qsort(s_rtx.ranges, s_rtx.numRanges, sizeof(s_rtx.ranges[0]), Rtx_RangeCmp);
		s_rtx.rangesSorted = 1;
	}
	int lo = 0, hi = s_rtx.numRanges - 1;
	while (lo <= hi)
	{
		int mid = (lo + hi) >> 1;
		const struct RtxRange *r = &s_rtx.ranges[mid];
		if (addr < r->start)
		{
			hi = mid - 1;
		}
		else if (addr >= r->end)
		{
			lo = mid + 1;
		}
		else
		{
			return r;
		}
	}
	return NULL;
}

static void Rtx_ViewToWorld(const float *v, float *out)
{
	const float *t = s_rtx.camT[s_rtx.view], *m = s_rtx.rInv[s_rtx.view];
	float x = v[0] - t[0], y = v[1] - t[1], z = v[2] - t[2];
	out[0] = m[0] * x + m[1] * y + m[2] * z;
	out[1] = m[3] * x + m[4] * y + m[5] * z;
	out[2] = m[6] * x + m[7] * y + m[8] * z;
}

// Decodes the GPU polygon commands of one packet into world-space triangles.
// Returns which commands were captured (bits = word offsets, RTX_MASK_ALL when
// GL can skip the whole packet); semi-transparent ones stay with GL.
static u32 Rtx_CapturePacketTo(u32 *words, int numWords, u16 serial, int allowSemi, struct RtxDynTri *out, int *num, int max)
{
	int i = 0;
	int polys = 0;
	int left = 0; // commands GL still has to draw
	u32 mask = 0;

	while (i < numWords)
	{
		u32 cmd = words[i];
		u32 code = cmd >> 24;

		if ((code == 0) || ((code >= 0xe1) && (code <= 0xe6)))
		{
			i++;
			continue;
		}
		if ((code < 0x20) || (code >= 0x40))
		{
			left++;
			break; // lines, sprites: GL draws the rest of the packet
		}

		int gouraud = (code & 0x10) != 0;
		int quad = (code & 0x08) != 0;
		int textured = (code & 0x04) != 0;
		int raw = (code & 0x01) != 0;
		int n = quad ? 4 : 3;
		int size = 1 + n * (1 + textured) + (gouraud ? n - 1 : 0);
		if (i + size > numWords)
		{
			left++;
			break;
		}
		if (((code & 2) && !(allowSemi && (code & 4))) || (i >= 32))
		{
			s_rtx.statSemi += (serial != RTX_LEVEL_SERIAL);
			left++; // semi-transparent: GL (depth tested against the traced image)
			i += size;
			continue;
		}
		int startDyn = *num;

		float pos[4][3];
		u8 uv[4][2];
		u8 col[4][4];
		u16 clut = 0, tpage = 0;
		u32 xys[4];
		const float *views[4];
		for (int v = 0, w2 = i + 1; v < n; v++)
		{
			w2 += (v == 0 || !gouraud) ? 0 : 1;
			xys[v] = words[w2++];
			w2 += textured;
		}
		if (!Rtx_CapPolygon(xys, n, serial, views))
		{
			s_rtx.statNoProj += (serial != RTX_LEVEL_SERIAL);
			goto skip;
		}
		int w = i + 1;
		for (int v = 0; v < n; v++)
		{
			u32 c = (v == 0 || !gouraud) ? cmd : words[w++];
			col[v][0] = raw ? 0x80 : (u8)c;
			col[v][1] = raw ? 0x80 : (u8)(c >> 8);
			col[v][2] = raw ? 0x80 : (u8)(c >> 16);
			col[v][3] = 0xff;

			w++; // xy: views[v]
			Rtx_ViewToWorld(views[v], pos[v]);

			if (textured)
			{
				u32 t = words[w++];
				uv[v][0] = (u8)t;
				uv[v][1] = (u8)(t >> 8);
				if (v == 0)
				{
					clut = (u16)(t >> 16);
				}
				else if (v == 1)
				{
					tpage = (u16)(t >> 16);
				}
			}
			else
			{
				uv[v][0] = uv[v][1] = 0;
			}
		}

		static const int triIdx[2][3] = {{0, 1, 2}, {1, 3, 2}};
		for (int t = 0; t < (quad ? 2 : 1); t++)
		{
			if (*num >= max)
			{
				goto skip;
			}
			struct RtxDynTri *d = &out[(*num)++];
			for (int c = 0; c < 3; c++)
			{
				int v = triIdx[t][c];
				memcpy(d->p[c], pos[v], sizeof(d->p[c]));
				d->uv[c][0] = uv[v][0];
				d->uv[c][1] = uv[v][1];
				memcpy(d->col[c], col[v], 4);
			}
			d->clut = clut;
			d->tpage = tpage;
			d->flags = (textured ? RTX_TRI_TEXTURED : 0) | ((code & 2) ? RTX_TRI_SEMI : 0);
			d->flags |= (textured && (serial == RTX_LEVEL_SERIAL) && Rtx_IsAnimatedTexture(tpage, clut, d->uv)) ? RTX_TRI_ANIMATED : 0;
			s_rtx.statCapAnimated += (d->flags & RTX_TRI_ANIMATED) != 0;
			d->object = serial;
		}
		polys++;
		s_rtx.statCaptured += (serial != RTX_LEVEL_SERIAL);
		mask |= 1u << i;
		i += size;
		continue;

	skip:
		*num = startDyn; // not projected (or full): GL draws this one
		left++;
		i += size;
	}
	if (polys == 0)
	{
		return 0;
	}
	return (left == 0) ? RTX_MASK_ALL : mask;
}

// ---- model triangles the PS1 culls ---------------------------------------------
// The PS1 draws a model's front faces in view only: back faces (NCLIP), the
// triangles off screen or behind the camera, whole models outside the view
// never become packets. The ray tracer wants them all (shadows of what is
// behind the camera, reflections, the far side of a kart a light falls on,
// glows out of view): the model renderer hands each such triangle here, from
// its model-space corners and the GTE's model -> view transform; GL draws none

int Rtx_WantsCulledTris(void)
{
	return (RTX_HDR != NULL) && Rtx_Drawing() && RTX_HDR->rendererReady && s_rtx.levelValid;
}

void Rtx_SetRtxOnlyInstance(int on)
{
	s_rtx.rtxOnly = on;
}

int Rtx_RtxOnlyInstance(void)
{
	return s_rtx.rtxOnly;
}

static int Rtx_AverageTexels(u16 tpage, u16 clut, const u8 (*uv)[2], int n, float *avg);
static void Rtx_AddEmitter(const float *pos, const float *color, float weight, float radius, int glowModel);

void Rtx_CaptureCulledTri(const u32 *gteRT, const u32 *packedXY, const u32 *packedZ, int textured, const u32 *texWords, const int *colors)
{
	if (!Rtx_WantsCulledTris() || (s_rtx.tagDepth <= 0) || (s_rtx.tagStack[s_rtx.tagDepth - 1] != RTX_TAG_INSTANCE) || s_rtx.dupDraw)
	{
		return;
	}
	int mode = s_rtx.instMode;
	if ((mode == RTX_INST_GL) || (mode == RTX_INST_HIDE))
	{
		return;
	}
	// GTE rotation (RT11..RT33, 4.12) and translation; view = TR + RT . v / 4096
	s16 rt[9] = {(s16)gteRT[0], (s16)(gteRT[0] >> 16), (s16)gteRT[1], (s16)(gteRT[1] >> 16), (s16)gteRT[2], (s16)(gteRT[2] >> 16),
	             (s16)gteRT[3], (s16)(gteRT[3] >> 16), (s16)gteRT[4]};
	float pos[3][3];
	for (int v = 0; v < 3; v++)
	{
		float m[3] = {(float)(s16)packedXY[v], (float)(s16)(packedXY[v] >> 16), (float)(s16)packedZ[v]};
		float view[3];
		for (int r = 0; r < 3; r++)
		{
			view[r] = ((float)(s32)gteRT[5 + r] + (rt[r * 3] * m[0] + rt[r * 3 + 1] * m[1] + rt[r * 3 + 2] * m[2]) / 4096.0f) * s_rtx.instScale;
		}
		Rtx_ViewToWorld(view, pos[v]);
	}
	u8 uv[3][2] = {{0, 0}, {0, 0}, {0, 0}};
	u16 clut = 0, tpage = 0;
	if (textured)
	{
		for (int v = 0; v < 3; v++)
		{
			uv[v][0] = (u8)texWords[v];
			uv[v][1] = (u8)(texWords[v] >> 8);
		}
		clut = (u16)(texWords[0] >> 16);
		tpage = (u16)(texWords[1] >> 16);
	}
	// (as the PS1 would: semi-transparent when its texture page blends)
	int semi = textured && (((tpage >> 5) & 3) != 3);
	if ((mode == RTX_INST_GLOW) || semi)
	{
		// a glow out of view still lights (as Rtx_EmitterPacket)
		float tex[3] = {1, 1, 1}, c[3] = {0, 0, 0}, centre[3] = {0, 0, 0}, radius = 0.0f;
		if (textured && !Rtx_AverageTexels(tpage, clut, (const u8(*)[2])uv, 3, tex))
		{
			return;
		}
		for (int v = 0; v < 3; v++)
		{
			for (int k = 0; k < 3; k++)
			{
				c[k] += (u8)(colors[v] >> (k * 8)) / 128.0f / 3.0f;
				centre[k] += pos[v][k] / 3.0f;
			}
		}
		for (int k = 0; k < 3; k++)
		{
			c[k] *= tex[k];
		}
		for (int v = 0; v < 3; v++)
		{
			float dx = pos[v][0] - centre[0], dy = pos[v][1] - centre[1], dz = pos[v][2] - centre[2];
			radius = fmaxf(radius, sqrtf(dx * dx + dy * dy + dz * dz));
		}
		float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2]));
		int vivid = (mx >= 0.5f) && ((mx - mn) >= 0.35f * mx);
		if ((mode == RTX_INST_GLOW) || (((tpage >> 5) & 3) == 1) || vivid)
		{
			Rtx_AddEmitter(centre, c, 0.5f + radius * radius / 1500.0f, radius, mode == RTX_INST_GLOW);
		}
		return;
	}
	if (s_rtx.numDyn >= (int)RTX_MAX_DYN_TRIS)
	{
		return;
	}
	struct RtxDynTri *d = &Rtx_DynTris()[s_rtx.numDyn++];
	for (int v = 0; v < 3; v++)
	{
		memcpy(d->p[v], pos[v], sizeof(d->p[v]));
		d->uv[v][0] = uv[v][0];
		d->uv[v][1] = uv[v][1];
		d->col[v][0] = (u8)colors[v];
		d->col[v][1] = (u8)(colors[v] >> 8);
		d->col[v][2] = (u8)(colors[v] >> 16);
		d->col[v][3] = 0xff;
	}
	d->clut = clut;
	d->tpage = tpage;
	d->flags = textured ? RTX_TRI_TEXTURED : 0;
	d->object = s_rtx.serial;
	s_rtx.statCulledTris++;
}

// ---- emitters: glowing (additive) things become point lights ---------------

static u16 Rtx_Vram16(int x, int y)
{
	return NativeRenderer_RtxVRAM()[(y & 511) * 1024 + (x & 1023)];
}

// PS1 texel at (u, v) like the GPU reads it; returns 0 for colour key 0x0000
static int Rtx_Texel(u16 tpage, u16 clut, int u, int v, float *rgb)
{
	int mode = (tpage >> 7) & 3, bx = (tpage & 15) * 64, by = ((tpage >> 4) & 1) * 256;
	int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
	u &= 255;
	v &= 255;
	u16 c;
	if (mode == 0)
	{
		c = Rtx_Vram16(cx + ((Rtx_Vram16(bx + (u >> 2), by + v) >> ((u & 3) * 4)) & 15), cy);
	}
	else if (mode == 1)
	{
		c = Rtx_Vram16(cx + ((Rtx_Vram16(bx + (u >> 1), by + v) >> ((u & 1) * 8)) & 255), cy);
	}
	else
	{
		c = Rtx_Vram16(bx + u, by + v);
	}
	if (c == 0)
	{
		return 0;
	}
	rgb[0] = (c & 31) / 31.0f;
	rgb[1] = ((c >> 5) & 31) / 31.0f;
	rgb[2] = ((c >> 10) & 31) / 31.0f;
	return 1;
}

// average opaque texel colour over a few points of a texture rectangle
static int Rtx_AverageTexels(u16 tpage, u16 clut, const u8 (*uv)[2], int n, float *avg)
{
	int umin = 255, umax = 0, vmin = 255, vmax = 0;
	for (int i = 0; i < n; i++)
	{
		umin = uv[i][0] < umin ? uv[i][0] : umin;
		umax = uv[i][0] > umax ? uv[i][0] : umax;
		vmin = uv[i][1] < vmin ? uv[i][1] : vmin;
		vmax = uv[i][1] > vmax ? uv[i][1] : vmax;
	}
	int count = 0;
	avg[0] = avg[1] = avg[2] = 0.0f;
	for (int sy = 0; sy < 5; sy++)
	{
		for (int sx = 0; sx < 5; sx++)
		{
			float rgb[3];
			int u = umin + ((umax - umin) * (2 * sx + 1)) / 10, v = vmin + ((vmax - vmin) * (2 * sy + 1)) / 10;
			if (Rtx_Texel(tpage, clut, u, v, rgb))
			{
				avg[0] += rgb[0];
				avg[1] += rgb[1];
				avg[2] += rgb[2];
				count++;
			}
		}
	}
	if (count == 0)
	{
		return 0;
	}
	avg[0] /= count;
	avg[1] /= count;
	avg[2] /= count;
	return 1;
}

static void Rtx_AddEmitter(const float *pos, const float *color, float weight, float radius, int glowModel)
{
	float lum = 0.299f * color[0] + 0.587f * color[1] + 0.114f * color[2];
	float mx = fmaxf(color[0], fmaxf(color[1], color[2])), mn = fminf(color[0], fminf(color[1], color[2]));
	s_rtx.statEmitDim += (lum < 0.05f);
	s_rtx.statEmitGrey += (lum >= 0.05f) && ((mx - mn < 0.35f * mx) && (weight >= 0.5f) && !glowModel);
	// white / grey glows (exhaust puffs, sparkles, highlights) don't light the scene:
	// lit from the kart itself they draw a bright outline around it (white smoke
	// particles add themselves, weakly: Rtx_OnParticle). Glowing models (turbo
	// flames burn white-hot) do: trace.comp keeps their light off models
	s_rtx.statEmitFull += (s_rtx.numEmitters >= RTX_MAX_EMITTERS);
	if ((lum < 0.05f) || ((mx - mn < 0.35f * mx) && (weight >= 0.5f) && !glowModel) || (s_rtx.numEmitters >= RTX_MAX_EMITTERS))
	{
		return;
	}
	// split screen: each view draws the same glow (the first view's counts)
	if (sdata->gGT->numPlyrCurrGame > 1)
	{
		for (int i = 0; i < s_rtx.numEmitters; i++)
		{
			const float *q = s_rtx.emitters[i].pos;
			float dx = q[0] - pos[0], dy = q[1] - pos[1], dz = q[2] - pos[2];
			if ((s_rtx.emitterView[i] != s_rtx.view) && (dx * dx + dy * dy + dz * dz < 16.0f))
			{
				return;
			}
		}
	}
	s_rtx.statEmitAdded++;
	s_rtx.emitterView[s_rtx.numEmitters] = (u8)s_rtx.view;
	struct RtxEmitter *e = &s_rtx.emitters[s_rtx.numEmitters++];
	for (int i = 0; i < 3; i++)
	{
		e->pos[i] = pos[i];
		e->color[i] = color[i] * weight;
	}
	e->weight = weight;
	e->radius = radius;
	const struct Instance *src = s_rtx.emitInst;
	e->inst = src;
	for (int i = 0; i < 3; i++)
	{
		e->instPos[i] = src ? (float)src->matrix.t[i] : 0.0f;
	}
}

// additive-blended polygons of a model packet (turbo flames, fire, glows)
static void Rtx_EmitterPacket(u32 *words, int numWords, u16 serial, int anyBlend)
{
	int i = 0;
	int abr = 0; // draw mode blend (DR_MODE / E1) for untextured polygons

	while (i < numWords)
	{
		u32 cmd = words[i];
		u32 code = cmd >> 24;
		if (code == 0xe1)
		{
			abr = (cmd >> 5) & 3;
			i++;
			continue;
		}
		if ((code < 0x20) || (code >= 0x40))
		{
			if ((code == 0) || (code > 0xe0))
			{
				i++;
				continue;
			}
			return;
		}
		int gouraud = (code & 0x10) != 0, quad = (code & 0x08) != 0, textured = (code & 0x04) != 0, semi = (code & 0x02) != 0;
		int n = quad ? 4 : 3;
		int size = 1 + n * (1 + textured) + (gouraud ? n - 1 : 0);
		if (i + size > numWords)
		{
			return;
		}

		float pos[4][3], col[3] = {0, 0, 0};
		u8 uv[4][2];
		u16 clut = 0, tpage = 0;
		int w = i + 1, found = 1;
		for (int v = 0; v < n; v++)
		{
			u32 c = (v == 0 || !gouraud) ? cmd : words[w++];
			col[0] += (u8)c / 128.0f / n;
			col[1] += (u8)(c >> 8) / 128.0f / n;
			col[2] += (u8)(c >> 16) / 128.0f / n;
			const float *view = Rtx_CapLookup(words[w++], serial);
			if (view != NULL)
			{
				Rtx_ViewToWorld(view, pos[v]);
			}
			else
			{
				found = 0;
			}
			if (textured)
			{
				u32 t = words[w++];
				uv[v][0] = (u8)t;
				uv[v][1] = (u8)(t >> 8);
				if (v == 0)
				{
					clut = (u16)(t >> 16);
				}
				else if (v == 1)
				{
					tpage = (u16)(t >> 16);
				}
			}
		}
		int blend = textured ? ((tpage >> 5) & 3) : abr;
		float tex[3] = {1, 1, 1};
		s_rtx.statEmitNoProj += !found;
		s_rtx.statEmitGlow += found && anyBlend;
		s_rtx.statEmitSemi += found && !anyBlend && semi;
		s_rtx.statEmitOpaque += found && !anyBlend && !semi;
		int haveTex = found && (!textured || Rtx_AverageTexels(tpage, clut, uv, n, tex));
		s_rtx.statPolyNoTex += found && !haveTex;
		if (haveTex)
		{
			float c[3] = {col[0] * tex[0], col[1] * tex[1], col[2] * tex[2]};
			float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2]));
			int vivid = (mx >= 0.5f) && ((mx - mn) >= 0.35f * mx); // bright and coloured
			// additive polygons, and other see-through ones that are vivid (glows,
			// flames); opaque paint never (karts, characters)
			if (anyBlend || (semi && ((blend == 1) || vivid)))
			{
				float centre[3] = {0, 0, 0}, radius = 0.0f;
				for (int v = 0; v < n; v++)
				{
					for (int k = 0; k < 3; k++)
					{
						centre[k] += pos[v][k] / n;
					}
				}
				for (int v = 0; v < n; v++)
				{
					float dx = pos[v][0] - centre[0], dy = pos[v][1] - centre[1], dz = pos[v][2] - centre[2];
					float d = sqrtf(dx * dx + dy * dy + dz * dz);
					radius = d > radius ? d : radius;
				}
				Rtx_AddEmitter(centre, c, 0.5f + radius * radius / 1500.0f, radius, anyBlend); // bigger flame, more light
			}
			else
			{
				s_rtx.statPolyNotGlow++;
			}
		}
		i += size;
	}
}

void Rtx_OnParticle(struct PushBuffer *pb, s32 viewX4, s32 viewY4, s32 viewZ4, u32 color, const struct Icon *icon)
{
	s_rtx.emitInst = NULL; // (particles: no model of their own)
	if ((RTX_HDR == NULL) || !Rtx_Drawing() || (Rtx_PbView(pb) < 0) || (icon == NULL))
	{
		return;
	}
	const struct TextureLayout *t = &icon->texLayout;
	s_rtx.statEmitParticle++;
	// fire, sparks, glows: additive sprites, or bright coloured ones (the colour
	// word carries no reliable semi-transparency bit here); grey smoke is skipped
	int additive = ((t->tpage >> 5) & 3) == 1;
	Rtx_CaptureCamera(pb);
	u8 uv[4][2] = {{t->u0, t->v0}, {t->u1, t->v1}, {t->u2, t->v2}, {t->u3, t->v3}};
	float tex[3];
	if (!Rtx_AverageTexels(t->tpage, t->clut, uv, 4, tex))
	{
		s_rtx.statPartNoTex++;
		return;
	}
	s_rtx.statPartAdditive += additive;
	float view[3] = {viewX4 * 0.25f, viewY4 * 0.25f, viewZ4 * 0.25f}, pos[3];
	Rtx_ViewToWorld(view, pos);
	float c[3] = {(u8)color / 128.0f * tex[0], (u8)(color >> 8) / 128.0f * tex[1], (u8)(color >> 16) / 128.0f * tex[2]};
	if (!additive)
	{
		float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2]));
		if ((mx >= 0.6f) && ((mx - mn) < 0.35f * mx))
		{
			s_rtx.statPartWhite++;
			Rtx_AddEmitter(pos, c, 0.25f, 40.0f, 0); // white smoke: a faint glow
			return;
		}
		if ((mx < 0.3f) || ((mx - mn) < 0.35f * mx))
		{
			s_rtx.statPartDull++;
			return; // dim or grey (smoke, dust)
		}
		s_rtx.statPartVivid++;
	}
	Rtx_AddEmitter(pos, c, 1.0f, 40.0f, 0);
}

// This frame's lights against the remembered ones: a light seen in the same
// place for a while is static (a torch, a brazier, a statue's fire) and lights
// from memory from then on, also while the game doesn't draw it (off screen,
// behind the camera) for up to 30 s, until it is on screen nearby and not seen
// (gone out). Lights that move (karts' flames) are this frame's only. Writes
// the strongest (by brightness at the camera) of both into out
// Glows on a model (save screens, warp pads, lamps on scenery) go by that model:
// still while it stays put, from the first sight however the lit part of it
// moves about (an animated screen), kept until the level changes or it should be
// on screen and isn't drawn (collected, gone)
static int Rtx_InstDrawnThisFrame(const void *inst);

static int Rtx_RememberLights(const struct RtxPointLight *cur, const void *const *curInst, const float (*curInstPos)[3], int numCur, struct RtxPointLight *out,
                              int maxOut)
{
	const int numMem = (int)(sizeof(s_rtx.remembered) / sizeof(s_rtx.remembered[0]));
	// (a glow's centre jumps around from frame to frame: flicker, sparks; what
	// tells a torch from a kart's flame is how far it gets from where it was
	// first seen within its first second)
	const float matchDist = 250.0f, staticMatchDist = 200.0f, moveDist = 300.0f;
	const int staticFrames = 30;
	const u32 forgetFrames = 900, levelGen = RTX_HDR->levelGen;
	struct RtxRememberedLight *mem = s_rtx.remembered;
	for (int m = 0; m < numMem; m++)
	{
		mem[m].matched = 0;
		if (mem[m].levelGen != levelGen)
		{
			mem[m].seen = 0; // (another level's)
			mem[m].lastGen = 0;
		}
	}

	static u8 fromMem[RTX_MAX_DYN_LIGHTS * 4];
	for (int c = 0; c < numCur; c++)
	{
		const struct RtxPointLight *l = &cur[c];
		int best = -1;
		float bestD2 = matchDist * matchDist;
		float instD2 = 1e30f;
		for (int m = 0; (m < numMem) && curInst[c]; m++)
		{
			if ((mem[m].seen != 0) && !mem[m].matched && (mem[m].inst == curInst[c]))
			{
				// (its model's glow; of several, the nearest)
				float dx = mem[m].pos[0] - l->pos[0], dy = mem[m].pos[1] - l->pos[1], dz = mem[m].pos[2] - l->pos[2];
				float d2 = dx * dx + dy * dy + dz * dz;
				best = (d2 < instD2) ? m : best;
				instD2 = (d2 < instD2) ? d2 : instD2;
			}
		}
		for (int m = 0; (m < numMem) && (best < 0); m++)
		{
			if ((mem[m].seen == 0) || mem[m].matched || (mem[m].inst != curInst[c]))
			{
				continue;
			}
			float dx = mem[m].pos[0] - l->pos[0], dy = mem[m].pos[1] - l->pos[1], dz = mem[m].pos[2] - l->pos[2];
			float d2 = dx * dx + dy * dy + dz * dz;
			// (a static light only takes what is where it is: a kart's flame going
			// past a torch is not the torch moving)
			int isStatic = !mem[m].moving && (mem[m].seen >= staticFrames);
			if ((d2 < bestD2) && (!isStatic || (d2 < staticMatchDist * staticMatchDist)))
			{
				bestD2 = d2;
				best = m;
			}
		}
		if (best < 0)
		{
			// a new one: a free slot, else the longest unseen
			best = 0;
			for (int m = 0; m < numMem; m++)
			{
				if (mem[m].seen == 0)
				{
					best = m;
					break;
				}
				best = ((s_rtx.gen - mem[m].lastGen) > (s_rtx.gen - mem[best].lastGen)) ? m : best;
			}
			memset(&mem[best], 0, sizeof(mem[best]));
			memcpy(mem[best].pos, l->pos, sizeof(l->pos));
			memcpy(mem[best].anchor, l->pos, sizeof(l->pos));
			memcpy(mem[best].color, l->color, sizeof(l->color));
			mem[best].falloff = l->falloff;
			mem[best].size = l->size;
			mem[best].levelGen = levelGen;
			mem[best].inst = curInst[c];
			memcpy(mem[best].instAnchor, curInstPos[c], sizeof(mem[best].instAnchor));
		}
		struct RtxRememberedLight *r = &mem[best];
		float ax = l->pos[0] - r->anchor[0], ay = l->pos[1] - r->anchor[1], az = l->pos[2] - r->anchor[2];
		int moved = (r->seen < staticFrames) && (ax * ax + ay * ay + az * az > moveDist * moveDist);
		if (r->inst)
		{
			// on a model: moved when the model has
			float mx = curInstPos[c][0] - r->instAnchor[0], my = curInstPos[c][1] - r->instAnchor[1], mz = curInstPos[c][2] - r->instAnchor[2];
			moved = (mx * mx + my * my + mz * mz > 50.0f * 50.0f);
		}
		s_rtx.statMemMoved += moved && !r->moving;
		r->moving |= moved;
		float k = r->moving ? 1.0f : 0.15f; // static: smoothed (the flames flicker, their light less)
		for (int j = 0; j < 3; j++)
		{
			r->pos[j] += (l->pos[j] - r->pos[j]) * (r->moving ? 1.0f : ((r->seen < staticFrames) ? 0.5f : 0.05f));
			r->color[j] += (l->color[j] - r->color[j]) * k;
		}
		r->falloff += (l->falloff - r->falloff) * k;
		r->size += (l->size - r->size) * k;
		r->seen = (r->seen < 60000) ? r->seen + 1 : r->seen;
		r->lastGen = s_rtx.gen;
		r->matched = 1;
		fromMem[c] = !r->moving && ((r->seen >= staticFrames) || r->inst);
	}

	// candidates: this frame's moving / new ones, every static remembered one
	// the players' cameras (split screen: the nearest one counts)
	int numCams = (s_rtx.numViews > 0) ? s_rtx.numViews : 1;
	float cam[RTX_MAX_VIEWS][3];
	const float zero[3] = {0, 0, 0};
	int viewWas = s_rtx.view;
	for (int v = 0; v < numCams; v++)
	{
		s_rtx.view = v;
		Rtx_ViewToWorld(zero, cam[v]);
	}
	s_rtx.view = viewWas;
	static struct RtxPointLight cand[RTX_MAX_DYN_LIGHTS * 4 + 128];
	static float score[RTX_MAX_DYN_LIGHTS * 4 + 128];
	int numCand = 0;
	for (int c = 0; c < numCur; c++)
	{
		if (!fromMem[c])
		{
			cand[numCand++] = cur[c];
		}
	}
	for (int m = 0; m < numMem; m++)
	{
		struct RtxRememberedLight *r = &mem[m];
		if (((r->seen < staticFrames) && !r->inst) || r->moving)
		{
			// not (yet) static: forget it soon after it's gone
			r->seen = ((r->seen != 0) && (s_rtx.gen - r->lastGen > 30u)) ? 0 : r->seen;
			continue;
		}
		if (!r->matched)
		{
			// unseen: gone out if it should be on screen nearby (in any player's
			// view), forgotten after a while
			int onScreen = 0;
			for (int view = 0; view < numCams; view++)
			{
				const struct RtxView *rv = &RTX_HDR->views[view];
				const float *R = rv->camR, *T = rv->camT;
				float v[3];
				for (int i = 0; i < 3; i++)
				{
					v[i] = R[i * 3] * r->pos[0] + R[i * 3 + 1] * r->pos[1] + R[i * 3 + 2] * r->pos[2] + T[i];
				}
				float sx = (v[2] > 1.0f) ? v[0] / v[2] * rv->projH : 1e9f, sy = (v[2] > 1.0f) ? v[1] / v[2] * rv->projH : 1e9f;
				onScreen |= (v[2] > 100.0f) && (v[2] < 2500.0f) && (fabsf(sx) < 0.4f * rv->vpW) && (fabsf(sy) < 0.4f * rv->vpH);
			}
			// (a model's: as long as the level lasts; gone out when the model is on
			// screen and not drawn, not when it is drawn without its glow this
			// frame, an animated screen's dark frame)
			// a model's glow (a level object's: the game culls it by its own bounds,
			// so "on screen" says nothing): out when the game hides the object
			// (collected, broken) or draws it a while without this glow (it moved
			// elsewhere on it); a model that isn't the level's own (another hub's,
			// a weapon) after the usual while
			int drawn = r->inst && Rtx_InstDrawnThisFrame(r->inst);
			int levelObj = r->inst && Rtx_IsLevelInstance(r->inst);
			int goneOut = r->inst ? ((levelObj && (((const struct Instance *)r->inst)->flags & HIDE_MODEL)) || (drawn && (s_rtx.gen - r->lastGen > 15u)) ||
			                         (!levelObj && (s_rtx.gen - r->lastGen > forgetFrames)))
			                      : (onScreen && (s_rtx.gen - r->lastGen > 4u));
			if (goneOut || (!r->inst && (s_rtx.gen - r->lastGen > forgetFrames)))
			{
				s_rtx.statMemGone += onScreen;
				s_rtx.statMemOld += !onScreen;
				r->seen = 0;
				continue;
			}
		}
		struct RtxPointLight *l = &cand[numCand++];
		memset(l, 0, sizeof(*l));
		memcpy(l->pos, r->pos, sizeof(l->pos));
		memcpy(l->color, r->color, sizeof(l->color));
		l->falloff = r->falloff;
		l->size = r->size;
	}

	// the strongest at the camera first
	for (int c = 0; c < numCand; c++)
	{
		const struct RtxPointLight *l = &cand[c];
		float d2 = 1e30f;
		for (int v = 0; v < numCams; v++)
		{
			float dx = l->pos[0] - cam[v][0], dy = l->pos[1] - cam[v][1], dz = l->pos[2] - cam[v][2];
			d2 = fminf(d2, dx * dx + dy * dy + dz * dz);
		}
		float reach = 3.0f * l->falloff;
		score[c] = (l->color[0] + l->color[1] + l->color[2]) / (1.0f + d2 / (reach * reach));
	}
	int n = 0;
	while ((n < maxOut) && (n < numCand))
	{
		int best = n;
		for (int c = n + 1; c < numCand; c++)
		{
			best = (score[c] > score[best]) ? c : best;
		}
		struct RtxPointLight tl = cand[n];
		float ts = score[n];
		cand[n] = cand[best];
		score[n] = score[best];
		cand[best] = tl;
		score[best] = ts;
		out[n] = cand[n];
		n++;
	}
	return n;
}

// greedy clustering of this frame's emitters into at most RTX_MAX_DYN_LIGHTS lights
static void Rtx_BuildDynLights(void)
{
	const float mergeDist = 260.0f;
	struct RtxEmitter clusters[RTX_MAX_DYN_LIGHTS * 4];
	int numClusters = 0;

	for (int i = 0; i < s_rtx.numEmitters; i++)
	{
		const struct RtxEmitter *e = &s_rtx.emitters[i];
		int best = -1;
		for (int c = 0; c < numClusters; c++)
		{
			const struct RtxEmitter *k = &clusters[c];
			float dx = k->pos[0] / k->weight - e->pos[0], dy = k->pos[1] / k->weight - e->pos[1], dz = k->pos[2] / k->weight - e->pos[2];
			if ((k->inst == e->inst) && (dx * dx + dy * dy + dz * dz < mergeDist * mergeDist)) // (one model's glows: not a passing kart's flame)
			{
				best = c;
				break;
			}
		}
		if (best < 0)
		{
			if (numClusters >= (int)(sizeof(clusters) / sizeof(clusters[0])))
			{
				continue;
			}
			best = numClusters++;
			memset(&clusters[best], 0, sizeof(clusters[best]));
			clusters[best].inst = e->inst;
			memcpy(clusters[best].instPos, e->instPos, sizeof(e->instPos));
		}
		struct RtxEmitter *k = &clusters[best];
		for (int j = 0; j < 3; j++)
		{
			k->pos[j] += e->pos[j] * e->weight;
			k->color[j] += e->color[j];
		}
		k->weight += e->weight;
		k->radius = e->radius > k->radius ? e->radius : k->radius;
	}

	// strongest first
	for (int a = 0; a < numClusters; a++)
	{
		for (int b = a + 1; b < numClusters; b++)
		{
			if (clusters[b].weight > clusters[a].weight)
			{
				struct RtxEmitter t = clusters[a];
				clusters[a] = clusters[b];
				clusters[b] = t;
			}
		}
	}

	// this frame's lights: colour = average colour of what glows; more glowing
	// stuff = brighter
	static struct RtxPointLight cur[RTX_MAX_DYN_LIGHTS * 4];
	static const void *curInst[RTX_MAX_DYN_LIGHTS * 4];
	static float curInstPos[RTX_MAX_DYN_LIGHTS * 4][3];
	int numCur = numClusters;
	for (int c = 0; c < numCur; c++)
	{
		const struct RtxEmitter *k = &clusters[c];
		struct RtxPointLight *l = &cur[c];
		curInst[c] = k->inst;
		memcpy(curInstPos[c], k->instPos, sizeof(curInstPos[c]));
		memset(l, 0, sizeof(*l));
		float strength = 1.2f * sqrtf(k->weight);
		strength = strength > 6.0f ? 6.0f : strength;
		for (int j = 0; j < 3; j++)
		{
			l->pos[j] = k->pos[j] / k->weight;
			l->color[j] = k->color[j] / k->weight * strength;
		}
		l->falloff = 300.0f + 4.0f * k->radius;
		l->size = 10.0f + 0.25f * k->radius;
	}
	int n = Rtx_RememberLights(cur, curInst, (const float(*)[3])curInstPos, numCur, RTX_HDR->dynLights, (int)RTX_MAX_DYN_LIGHTS);
	RTX_HDR->numDynLights = (u32)n;
	s_rtx.statLights += n;
}

// ---- GL overlay depth -------------------------------------------------------
// What GL still draws over the ray traced image (glowing and transparent models,
// particles, skidmarks, sprites) gets a view z per vertex; the overlay shaders
// drop fragments behind the traced surface (NativeRenderer_RtxSetImage).

// every packet of the OT walk, OT entries included: which ordering table and
// bucket the following packets hang off
void Rtx_TrackPacket(u32 *packet)
{
	u32 a = (u32)packet;
	for (int v = 0; v < s_rtx.numViews; v++)
	{
		u32 ot = s_rtx.otBase[v];
		if ((ot != 0) && (a >= ot) && (a < ot + 0x400 * 4))
		{
			s_rtx.ctxWorld = 1;
			s_rtx.ctxBucket = (int)((a - ot) >> 2);
			s_rtx.ctxView = v;
			s_rtx.view = v; // (its packets decode with its camera)
			return;
		}
	}
	// HUD: always on top. (The ordering table's top entries: the checkered flag
	// and loading text in the one before the UI's (otSwapchainDB[0]), the UI's
	// five; the UI push buffer's range end may not be set this frame)
	u32 uiEnd = (s_rtx.uiOtEnd > s_rtx.uiOtStart + 5 * 4) ? s_rtx.uiOtEnd : s_rtx.uiOtStart + 5 * 4;
	if ((s_rtx.uiOtStart != 0) && (a + 4 >= s_rtx.uiOtStart) && (a <= uiEnd))
	{
		s_rtx.ctxWorld = 0;
	}
}

static float Rtx_BucketZ(void)
{
	return s_rtx.ctxWorld ? s_rtx.zPerBucket * (s_rtx.ctxBucket + 0.5f) : 0.0f;
}

float Rtx_VertexDepth(u32 xy)
{
	if (!s_rtx.ctxActive || !Rtx_FrameActive())
	{
		return 0.0f;
	}
	if (s_rtx.ctxZ > 0.0f)
	{
		return s_rtx.ctxZ;
	}
	const float *v = Rtx_CapLookup(xy, s_rtx.ctxSerial);
	if ((v != NULL) && (v[2] > 1.0f))
	{
		return v[2];
	}
	return Rtx_BucketZ();
}

// a smoke particle (POLY_FT4 sprite, not additive): its screen corners back at
// its depth into world space, a camera-facing quad that shadows partly
static void Rtx_CaptureSmoke(u32 *packet, int lengthWords, float z)
{
	u32 code = (lengthWords >= 9) ? (packet[1] >> 24) : 0;
	int v = s_rtx.ctxView;
	if ((v != 0) || ((code & 0xfc) != 0x2c) || (s_rtx.partH[v] <= 0.0f) || (z <= 1.0f) || (s_rtx.numSmoke + 2 > (int)(sizeof(s_rtx.smoke) / sizeof(s_rtx.smoke[0]))))
	{
		return;
	}
	u16 clut = (u16)(packet[3] >> 16), tpage = (u16)(packet[5] >> 16);
	if (((tpage >> 5) & 3) == 1)
	{
		return; // additive: fire, sparks, glows (they light, they don't shade)
	}
	float pos[4][3];
	u8 uv[4][2];
	for (int k = 0; k < 4; k++)
	{
		// (the view's projection, not corner k's: other views' are 0 in single player)
		u32 xy = packet[2 + k * 2], t = packet[3 + k * 2];
		float view[3] = {((s16)(xy & 0xffff) - s_rtx.partOfx[v]) * z / s_rtx.partH[v], ((s16)(xy >> 16) - s_rtx.partOfy[v]) * z / s_rtx.partH[v], z};
		Rtx_ViewToWorld(view, pos[k]);
		uv[k][0] = (u8)t;
		uv[k][1] = (u8)(t >> 8);
	}
	static const int triIdx[2][3] = {{0, 1, 2}, {1, 3, 2}};
	for (int t = 0; t < 2; t++)
	{
		struct RtxDynTri *d = &s_rtx.smoke[s_rtx.numSmoke++];
		for (int c = 0; c < 3; c++)
		{
			int v = triIdx[t][c];
			memcpy(d->p[c], pos[v], sizeof(d->p[c]));
			d->uv[c][0] = uv[v][0];
			d->uv[c][1] = uv[v][1];
			d->col[c][0] = (u8)packet[1];
			d->col[c][1] = (u8)(packet[1] >> 8);
			d->col[c][2] = (u8)(packet[1] >> 16);
			d->col[c][3] = 0xff;
		}
		d->clut = clut;
		d->tpage = tpage;
		d->flags = RTX_TRI_TEXTURED | RTX_TRI_SMOKE;
		d->object = 0xfffe;
	}
}

int Rtx_ClassifyPacket(u32 *packet, int lengthWords)
{
	const struct RtxRange *r = Rtx_FindRange((u32)packet);
	s_rtx.ctxSerial = 0;
	s_rtx.ctxZ = 0.0f;
	s_rtx.ctxActive = 0;
	s_rtx.ctxMask = 0;
	if (r == NULL)
	{
		s_rtx.ctxActive = s_rtx.ctxWorld; // world effects: projections recorded outside tags, else the OT bucket
		return RTX_PACKET_DRAW;
	}
	switch (r->tag)
	{
	case RTX_TAG_LEVEL:
		// the exact PS1 texturing of what is on screen: the renderer's primary
		// rays take texels from these (next frame), lighting from the level mesh
		if ((s_rtx.cameraValid & (1 << s_rtx.ctxView)) && (lengthWords > 0))
		{
			struct RtxDynTri *cap = (struct RtxDynTri *)(s_rtx.base + RTX_OFF_LEVEL_CAP);
			int first = s_rtx.numLevelCap;
			Rtx_CapturePacketTo(packet + 1, lengthWords, RTX_LEVEL_SERIAL, 1, cap, &s_rtx.numLevelCap, (int)RTX_MAX_LEVEL_CAP);
			for (int i = first; i < s_rtx.numLevelCap; i++)
			{
				cap[i].flags |= (u16)(s_rtx.ctxView << RTX_TRI_VIEW_SHIFT); // (split screen: the view it belongs to)
			}
		}
		return RTX_PACKET_SKIP;
	case RTX_TAG_SHADOW:
		return RTX_PACKET_SKIP;
	case RTX_TAG_SKY:
		return RTX_PACKET_SKY;
	case RTX_TAG_INSTANCE:
	{
		s_rtx.ctxActive = 1;
		s_rtx.ctxSerial = r->serial;
		// first vertex of a polygon packet: calibrates bucket -> z for packets without projections
		u32 code = (lengthWords >= 2) ? (packet[1] >> 24) : 0;
		if (s_rtx.ctxWorld && (code >= 0x20) && (code < 0x40))
		{
			const float *v = Rtx_CapLookup(packet[2], r->serial);
			if ((v != NULL) && (v[2] > 1.0f))
			{
				s_rtx.calZ += v[2];
				s_rtx.calB += s_rtx.ctxBucket + 0.5;
				s_rtx.calN++;
			}
		}
		u32 mask = Rtx_CapturedMask((u32)packet);
		if (mask == RTX_MASK_ALL)
		{
			return RTX_PACKET_SKIP;
		}
		s_rtx.ctxMask = mask;
		return RTX_PACKET_DRAW;
	}
	case RTX_TAG_DEPTH:
		s_rtx.ctxActive = 1;
		s_rtx.ctxZ = r->z;
		Rtx_CaptureSmoke(packet, lengthWords, r->z);
		return RTX_PACKET_DRAW;
	default:
		return RTX_PACKET_DRAW; // HUD models, other views
	}
}

// one particle's packet (words long, at prim): its view z for the depth test
void Rtx_OnParticleDepth(struct PushBuffer *pb, u32 *prim, int words, s32 viewZ4, s32 ofx, s32 ofy, s32 h)
{
	int v = Rtx_PbView(pb);
	if ((RTX_HDR == NULL) || !Rtx_Drawing() || (v < 0))
	{
		return;
	}
	s_rtx.partOfx[v] = ofx / 65536.0f;
	s_rtx.partOfy[v] = ofy / 65536.0f;
	s_rtx.partH[v] = (float)h;
	u32 start = (u32)prim;
	struct RtxRange *r = (s_rtx.numRanges > 0) ? &s_rtx.ranges[s_rtx.numRanges - 1] : NULL;
	if ((r == NULL) || (r->tag != RTX_TAG_DEPTH) || (r->start != start))
	{
		if (s_rtx.numRanges >= RTX_MAX_RANGES)
		{
			return;
		}
		// (the last particle was culled after its hook: its slot is reused)
		r = &s_rtx.ranges[s_rtx.numRanges++];
		if ((s_rtx.numRanges > 1) && (start < r[-1].start))
		{
			s_rtx.rangesSorted = 0;
		}
	}
	r->start = start;
	r->end = start + (u32)words * 4;
	r->tag = RTX_TAG_DEPTH;
	r->serial = 0;
	r->allowSemi = 0;
	r->mode = 0;
	r->z = viewZ4 * 0.25f;
}

int Rtx_SkyPhase(void)
{
	return s_rtx.skyPhase;
}

int Rtx_IsSkyPacket(u32 *packet)
{
	const struct RtxRange *r = Rtx_FindRange((u32)packet);
	return (r != NULL) && (r->tag == RTX_TAG_SKY);
}

void Rtx_EndSkyPhase(void)
{
	s_rtx.skyPhase = 0;
	NativeRenderer_RtxSkySnapshot();
	int w, h;
	const u8 *image = NULL;
	const float *depth = NULL;
	NativeRenderer_RtxViewportSize(&w, &h);
	int ok = Rtx_RenderFrame(w, h, &image, &depth);
	NativeRenderer_RtxSetImage(ok ? image : NULL, ok ? depth : NULL, w, h);
}

int Rtx_RenderFrame(int outW, int outH, const u8 **rgba, const float **depth)
{
	if (!Rtx_FrameActive())
	{
		return 0;
	}
	if (outW > (int)RTX_MAX_W)
	{
		outW = RTX_MAX_W;
	}
	if (outH > (int)RTX_MAX_H)
	{
		outH = RTX_MAX_H;
	}

	Rtx_RefreshTexSlots();
	memcpy(s_rtx.base + RTX_OFF_VRAM, NativeRenderer_RtxVRAM(), RTX_VRAM_W * RTX_VRAM_H * 2);
	Rtx_BuildDynLights();
	if (SDL_getenv("CTR_RTX_DEBUG") && (RTX_HDR->frame % 300 == 0))
	{
		float cam[3];
		const float zero[3] = {0, 0, 0};
		Rtx_ViewToWorld(zero, cam);
		printf("[RTX] frame %u: %d dyn tris, %d emitters -> %u lights, %d ranges, cam %.0f %.0f %.0f, renderer %.2f ms, %u spp\n", RTX_HDR->frame, s_rtx.numDyn, s_rtx.numEmitters, RTX_HDR->numDynLights, s_rtx.numRanges, cam[0], cam[1],
		       cam[2], RTX_HDR->renderMs, RTX_HDR->traceSpp);
		printf("[RTX]   models: %d instances, polygons %d traced, %d unprojected (GL), %d see-through (GL); %d level polygons\n", s_rtx.statInstances,
		       s_rtx.statCaptured, s_rtx.statNoProj, s_rtx.statSemi, RTX_HDR->numLevelCap);
		printf("[RTX]   models: %d triangles the PS1 culled traced (back faces, off screen, models out of view)\n", s_rtx.statCulledTris);
		printf("[RTX]   (last 300 frames) instances: %d traced, %d glowing, %d GL only, %d hidden (remaster); emitter candidates: %d glow-model polys, %d see-through, %d opaque (skipped), %d particles, %d no projection; rejected %d grey, %d dim\n",
		       s_rtx.statInstMode[RTX_INST_CAPTURE], s_rtx.statInstMode[RTX_INST_GLOW], s_rtx.statInstMode[RTX_INST_GL], s_rtx.statInstMode[RTX_INST_HIDE], s_rtx.statEmitGlow, s_rtx.statEmitSemi,
		       s_rtx.statEmitOpaque, s_rtx.statEmitParticle, s_rtx.statEmitNoProj, s_rtx.statEmitGrey, s_rtx.statEmitDim);
		printf("[RTX]   particles: %d additive, %d vivid, %d white, %d dull (no light), %d no texels\n", s_rtx.statPartAdditive, s_rtx.statPartVivid,
		       s_rtx.statPartWhite, s_rtx.statPartDull, s_rtx.statPartNoTex);
		printf("[RTX]   polygons: %d no texels, %d not glowing; emitters added %d (%d over the limit) -> %d frame lights\n", s_rtx.statPolyNoTex, s_rtx.statPolyNotGlow,
		       s_rtx.statEmitAdded, s_rtx.statEmitFull, s_rtx.statLights);
		int remembered = 0, tracked = 0, moving = 0;
		for (int m = 0; m < (int)(sizeof(s_rtx.remembered) / sizeof(s_rtx.remembered[0])); m++)
		{
			const struct RtxRememberedLight *r = &s_rtx.remembered[m];
			remembered += (r->seen >= 30) && !r->moving;
			tracked += (r->seen != 0);
			moving += (r->seen != 0) && r->moving;
		}
		printf("[RTX]   karts on the traced floor: %d seated (%d bodies moved along), mean gap %.1f (abs %.1f), %d with no floor under their wheels; %d animated texture rects, %d animated level polygons drawn\n",
		       s_rtx.statGapN, s_rtx.statGapBody, s_rtx.statGapN ? s_rtx.statGapSum / s_rtx.statGapN : 0.0, s_rtx.statGapN ? s_rtx.statGapAbs / s_rtx.statGapN : 0.0, s_rtx.statGapFar,
		       s_rtx.numAnimRect, s_rtx.statCapAnimated);
		s_rtx.statGapN = s_rtx.statGapFar = s_rtx.statGapBody = s_rtx.statCapAnimated = 0;
		s_rtx.statGapSum = s_rtx.statGapAbs = 0.0;
		printf("[RTX]   glows tracked %d: %d static (remembered), %d moving; %d moved / %d forgotten on screen / %d timed out (last 300 frames)\n", tracked, remembered, moving,
		       s_rtx.statMemMoved, s_rtx.statMemGone, s_rtx.statMemOld);
		for (int m = 0; m < (int)(sizeof(s_rtx.remembered) / sizeof(s_rtx.remembered[0])); m++)
		{
			const struct RtxRememberedLight *r = &s_rtx.remembered[m];
			if (r->seen != 0)
			{
				printf("[RTX]     glow at %.0f %.0f %.0f colour %.2f %.2f %.2f: seen %u frames, %s, last seen %u frames ago\n", r->pos[0], r->pos[1], r->pos[2],
				       r->color[0], r->color[1], r->color[2], r->seen, r->moving ? "moving" : "still", s_rtx.gen - r->lastGen);
			}
		}
		s_rtx.statMemMoved = s_rtx.statMemGone = s_rtx.statMemOld = 0;
		s_rtx.statPolyNoTex = s_rtx.statPolyNotGlow = s_rtx.statEmitAdded = s_rtx.statEmitFull = s_rtx.statLights = 0;
		memset(s_rtx.statInstMode, 0, sizeof(s_rtx.statInstMode));
		printf("[RTX]   models partly GL drawn (id x instances):");
		for (int m = 0; m < 256; m++)
		{
			if (s_rtx.statGlLeftModel[m])
			{
				printf(" %d x%d", m, s_rtx.statGlLeftModel[m]);
			}
		}
		printf("\n");
		memset(s_rtx.statGlLeftModel, 0, sizeof(s_rtx.statGlLeftModel));
		s_rtx.statEmitGlow = s_rtx.statEmitSemi = s_rtx.statEmitOpaque = s_rtx.statEmitParticle = s_rtx.statEmitGrey = s_rtx.statEmitDim = s_rtx.statEmitNoProj = 0;
		s_rtx.statPartAdditive = s_rtx.statPartVivid = s_rtx.statPartWhite = s_rtx.statPartDull = s_rtx.statPartNoTex = 0;
		for (int i = 0; i < (int)RTX_HDR->numDynLights && i < 8; i++)
		{
			const struct RtxPointLight *l = &RTX_HDR->dynLights[i];
			printf("[RTX]   frame light %d: %.0f %.0f %.0f colour %.2f %.2f %.2f falloff %.0f\n", i, l->pos[0], l->pos[1], l->pos[2], l->color[0], l->color[1], l->color[2], l->falloff);
		}
		const struct PrimMem *pm = &sdata->gGT->backBuffer->primMem;
		printf("[RTX]   primitive memory %u of %u KB, level LOD x%.2f\n", (u32)((u32)pm->cursor - (u32)pm->start) / 1024, (u32)pm->capacityBytes / 1024,
		       Rtx_LevelLodScale(1000) / 1000.0f);
		for (int i = 0; i < s_rtx.numDyn && i < 3; i++)
		{
			struct RtxDynTri *d = &Rtx_DynTris()[i];
			printf("[RTX]   tri %d obj %d: %.0f %.0f %.0f / %.0f %.0f %.0f / %.0f %.0f %.0f\n", i, d->object, d->p[0][0], d->p[0][1], d->p[0][2], d->p[1][0],
			       d->p[1][1], d->p[1][2], d->p[2][0], d->p[2][1], d->p[2][2]);
		}
	}
	// last frame's smoke after this frame's models
	int smoke = s_rtx.numSmoke < (int)RTX_MAX_DYN_TRIS - s_rtx.numDyn ? s_rtx.numSmoke : (int)RTX_MAX_DYN_TRIS - s_rtx.numDyn;
	memcpy(&Rtx_DynTris()[s_rtx.numDyn], s_rtx.smoke, sizeof(struct RtxDynTri) * smoke);
	s_rtx.numDyn += smoke;
	s_rtx.numSmoke = 0;
	const char *probeEnv = getenv("CTR_RTX_PROBE_FRAME"); // (test runs: probes at these ray traced frames, comma separated)
	for (const char *e = probeEnv; (e != NULL) && (*e != 0); e = strchr(e, ',') ? strchr(e, ',') + 1 : NULL)
	{
		s_probe.want |= (atoi(e) > 0) && (RTX_HDR->frame + 1u == (u32)atoi(e));
	}
	if (s_probe.want)
	{
		s_probe.want = 0;
		s_probe.pending = 1;
		Rtx_ProbeGame();
		RTX_HDR->probe++; // (the renderer adds its camera rays to the file)
	}
	RTX_HDR->numDynTris = (u32)s_rtx.numDyn;
	Rtx_PublishProxies();
	RTX_HDR->numLevelCap = (u32)s_rtx.numLevelCap; // last frame's; this frame's walk refills the section
	s_rtx.numLevelCap = 0;
	RTX_HDR->outW = (u32)outW;
	RTX_HDR->outH = (u32)outH;
	RTX_HDR->dispW = 512.0f;
	RTX_HDR->dispH = 216.0f;
	RTX_HDR->debugView = (u32)s_rtx.debugView;
	RTX_HDR->lightMode = s_rtx.lightMode ? 0u : 1u; // default full relight, F5 hybrid
	RTX_HDR->settings = s_rtx.settings;
	RTX_HDR->settings.baked = !s_rtx.enabled; // (drawing with it off: an NF track, Rtx_Drawing)
	// pipelined: show the previous request's image (made the same way, at this
	// size, the game frame before) and only wait for the renderer to take this
	// frame's input
	int pipelined = s_rtx.settings.pipelined && s_rtx.prevPipelined && (s_rtx.prevW == outW) && (s_rtx.prevH == outH) && (s_rtx.prevGen + 1u == s_rtx.gen);
	RTX_HDR->frame++;

	__sync_synchronize();
	u32 req = RTX_HDR->frameReq + 1;
	RTX_HDR->frameReq = req;
	Rtx_FutexWake(&RTX_HDR->frameReq);

	u64 start = SDL_GetTicksNS(), lastBusy = start;
	volatile u32 *wait = pipelined ? &RTX_HDR->frameTaken : &RTX_HDR->frameDone;
	s_rtx.prevPipelined = 0;
	for (;;)
	{
		u32 v = *wait;
		if (v == req)
		{
			break;
		}
		// (a level being loaded / prepared takes a while: up to 15 s; the frame
		// after it still comes, so 250 ms from when it was last seen busy)
		u64 now = SDL_GetTicksNS(), waited = now - start;
		lastBusy = RTX_HDR->rendererBusy ? now : lastBusy;
		// (64 spp at 4K takes its time: the wait grows with the samples)
		u64 frameLimit = 250000000ull + 20000000ull * s_rtx.settings.spp;
		if (((now - lastBusy) > frameLimit) || (waited > 15000000000ull))
		{
			static int warned;
			if (!warned++)
			{
				printf("[RTX] renderer timed out (request %u, game frame %u, %u ms), frame drawn without ray tracing\n", req, RTX_HDR->frame, (u32)(waited / 1000000ull));
			}
			return 0;
		}
		if (Rtx_RendererExited() || !RTX_HDR->rendererReady)
		{
			return 0;
		}
		Rtx_FutexWait(wait, v, 2000000); // (the renderer wakes it)
	}
	__sync_synchronize();
	s_rtx.rendererRan = 1;
	s_rtx.prevPipelined = s_rtx.settings.pipelined;
	s_rtx.prevW = outW;
	s_rtx.prevH = outH;
	s_rtx.prevGen = s_rtx.gen;
	u32 slot = (pipelined ? req - 1u : req) & 1u;
	*rgba = s_rtx.base + RTX_OFF_IMAGE + slot * RTX_SLOT_BYTES;
	*depth = (const float *)(s_rtx.base + RTX_OFF_DEPTH + slot * RTX_SLOT_BYTES);
	if (s_probe.pending)
	{
		s_probe.pending = 0;
		Rtx_ProbeDepth(*depth, outW, outH);
	}
	return 1;
}
