#ifndef NATIVE_SETTINGS_H
#define NATIVE_SETTINGS_H

// MultiTrack: player settings kept in settings.ini (key=value lines) in the
// game's base directory, next to memcards/. Read once at startup; every place
// that changes a setting (main menu Settings, F12 overlay, F7, F11) writes it
// back with NativeSettings_Save. CTR_* env vars still override what the file says.

void NativeSettings_Load(void);
int NativeSettings_Has(const char *key);
int NativeSettings_GetInt(const char *key, int fallback);
void NativeSettings_SetInt(const char *key, int value);
void NativeSettings_Save(void);

#endif
