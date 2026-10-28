#ifndef NATIVE_RTX_H
#define NATIVE_RTX_H

// MultiTrack: real-time ray tracing bridge to the 64-bit renderer (native/rtx).
// The game keeps rendering through the PSX GPU path; when a ray-traced frame is
// active, level and captured model packets are dropped from the GL pass and the
// renderer's image is composited between the sky and the overlay (HUD, particles).

#include <macros.h>

struct PrimMem;
struct PushBuffer;

enum RtxTag
{
	RTX_TAG_NONE,
	RTX_TAG_LEVEL,    // level quadblocks: replaced by the static level mesh
	RTX_TAG_SKY,      // skybox, glow, stars: GL background layer
	RTX_TAG_INSTANCE, // model instances: captured into dynamic triangles
	RTX_TAG_SHADOW,   // kart blob shadows: dropped (real shadows instead)
	RTX_TAG_DEPTH,    // GL drawn, constant view z (particles)
};

enum RtxPacketClass
{
	RTX_PACKET_DRAW, // GL draws it
	RTX_PACKET_SKIP, // ray traced instead
	RTX_PACKET_SKY,  // GL background layer
};

extern int g_rtxCapture; // GTE projections are being recorded

void Rtx_Init(void);
void Rtx_Shutdown(void);
void Rtx_HandleKey(int scancode);
const char *Rtx_KeyStatus(int scancode);  // F5 / F6 / F7 state, for the F12 overlay
int Rtx_OverlayText(char *buf, int size); // renderer settings and timings
void Rtx_OverlayKey(int digit, int back);  // overlay keys 1-8 step a setting
int Rtx_Supported(void);   // the renderer runs (0: not started, or the GPU can't ray trace)
int Rtx_Enabled(void);     // F7 / Settings menu: ray tracing on
void Rtx_SetEnabled(int on);
int Rtx_GetSpp(void);      // samples per pixel (RTX_MIN_SPP..RTX_MAX_SPP in steps of RTX_SPP_STEP)
void Rtx_SetSpp(int spp);
// baked ray traced lighting for the GL renderer (native_rtx_bake.c)
void Rtx_BakeFrame(void);        // each frame first: the loaded levels' bakes in or out
int Rtx_BakedEnabled(void);      // Settings "Baked RT"
void Rtx_SetBakedEnabled(int on);
void Rtx_BakeStart(void);        // "Bake now": every level not baked yet
void Rtx_BakeStop(void);         // after the level being baked
void Rtx_BakeTick(void);         // a step of it (each menu frame)
int Rtx_BakeRunning(void);
void Rtx_BakeProgress(int *done, int *total, const char **name);
void Rtx_BakeShutdown(void);

// game hooks
void Rtx_FrameBegin(void);
void Rtx_BeginTag(int tag, struct PrimMem *primMem);
void Rtx_EndTag(struct PrimMem *primMem);
struct Instance;
void Rtx_BeginInstance(struct PrimMem *primMem, struct Instance *inst, struct PushBuffer *pb, const s32 *mvpT);
void Rtx_BeginScaled(struct PrimMem *primMem, struct PushBuffer *pb, float viewScale, const struct Instance *wheelsOf); // GTE view space = true view / viewScale; wheelsOf: a driver's wheels (set onto the traced floor)
struct Icon;
void Rtx_OnParticle(struct PushBuffer *pb, s32 viewX4, s32 viewY4, s32 viewZ4, u32 color, const struct Icon *icon);
void Rtx_OnParticleDepth(struct PushBuffer *pb, u32 *prim, int words, s32 viewZ4, s32 ofx, s32 ofy, s32 h); // GTE OFX/OFY (16.16), H
void Rtx_OnLevelCamera(struct PushBuffer *pb); // each player's view (split screen: every player's)
void Rtx_SetCaptureView(struct PushBuffer *pb); // level overlays: the player's view being drawn
int Rtx_LevelLodScale(int depth); // level LOD distances scaled to the output resolution while ray tracing
void Rtx_SetTextureNearDepth(int depth); // past it the PS1 uses lower textures: the renderer uses the mesh's near ones
void Rtx_OnProject(u32 sxy, s32 mac1, s32 mac2, s32 mac3, int sf);
int Rtx_WantsCulledTris(void); // ray tracing: model triangles the PS1 culls go to the ray tracer (Rtx_CaptureCulledTri)
void Rtx_SetRtxOnlyInstance(int on); // the instance being drawn is outside the view: all its triangles ray traced only
int Rtx_RtxOnlyInstance(void);
void Rtx_CaptureCulledTri(const u32 *gteRT, const u32 *packedXY, const u32 *packedZ, int textured, const u32 *texWords, const int *colors);

// GPU hooks
int Rtx_FrameActive(void);
int Rtx_ClassifyPacket(u32 *packet, int lengthWords); // also sets the overlay depth context
void Rtx_TrackPacket(u32 *packet);                    // every packet of the OT walk
float Rtx_VertexDepth(u32 xy);                         // view z for the overlay depth test, 0 = none
int Rtx_SkipCommand(u32 *packet, u32 *cmd);            // partly captured packet: words to skip at cmd, 0 = draw
int Rtx_SkyPhase(void);
int Rtx_IsSkyPacket(u32 *packet); // skybox / glow / stars (no side effects)
void Rtx_EndSkyPhase(void); // sky layer done: snapshot it, ray trace, blit under the overlay
int Rtx_RenderFrame(int outW, int outH, const u8 **rgba, const float **depth); // pipelined: last request's image

// renderer (platform/native_renderer.c)
void NativeRenderer_RtxSkySnapshot(void);
void NativeRenderer_RtxViewportSize(int *w, int *h);
void NativeRenderer_RtxSetImage(const u8 *rgba, const float *depth, int w, int h);

#endif
