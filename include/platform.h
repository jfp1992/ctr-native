#ifndef PLATFORM_H
#define PLATFORM_H

struct PlatformMempackArena
{
	void *base;
	void *start;
	void *endOfMemory;
	int size;
	int backingSize;
};

void Platform_Init(const char *title, int width, int height);
void Platform_Shutdown(void);
void Platform_InitScratchpad(void);
const struct PlatformMempackArena *Platform_InitMempackArena(void);
const struct PlatformMempackArena *Platform_GetMempackArena(void);
void Platform_BeginFrame(void);
int Platform_BeginScene(void);
void Platform_EndScene(void);
void Platform_EndFrame(void);
void Platform_PresentVRAMDisplay(void);
void Platform_PinVRAMDisplayFrames(int frameCount);
void Platform_PinVRAMDisplayRect(int x, int y, int w, int h, int frameCount);
int Platform_GetVBlankCount(void);
void Platform_WaitUntilVBlank(int targetVBlank);
void Platform_PollHostEvents(void);
int Platform_PollInput(void);
int Platform_InputSkipPressed(void); // MultiTrack: cross / gamepad A went down (boot intro skip)
// MultiTrack: display settings (main menu Settings, F11); w x h 0 x 0 = the desktop's
int Platform_GetDisplayModes(int *w, int *h, int max); // distinct sizes, largest first
void Platform_GetDisplay(int *w, int *h, int *fullscreen);
void Platform_SetDisplay(int w, int h, int fullscreen);

#if defined(CTR_NATIVE)
int NikoGetEnterKey(void);
#endif

#endif
