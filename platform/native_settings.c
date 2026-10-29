// MultiTrack: settings.ini (see include/platform/native_settings.h)

#include "platform/native_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_SETTINGS_FILE "settings.ini"
#define NATIVE_SETTINGS_MAX  48

static struct
{
	int count;
	struct
	{
		char key[32];
		int value;
	} items[NATIVE_SETTINGS_MAX];
} s_settings;

static int NativeSettings_Find(const char *key)
{
	for (int i = 0; i < s_settings.count; i++)
	{
		if (strcmp(s_settings.items[i].key, key) == 0)
		{
			return i;
		}
	}
	return -1;
}

void NativeSettings_Load(void)
{
	FILE *f = fopen(NATIVE_SETTINGS_FILE, "r");
	s_settings.count = 0;
	if (f == NULL)
	{
		return;
	}
	char line[128];
	while (fgets(line, sizeof(line), f) != NULL)
	{
		char *eq = strchr(line, '=');
		if ((line[0] == '#') || (eq == NULL))
		{
			continue;
		}
		*eq = 0;
		char *key = line;
		while ((*key == ' ') || (*key == '\t'))
		{
			key++;
		}
		char *end = key + strlen(key);
		while ((end > key) && ((end[-1] == ' ') || (end[-1] == '\t')))
		{
			*--end = 0;
		}
		if (key[0] != 0)
		{
			NativeSettings_SetInt(key, atoi(eq + 1));
		}
	}
	fclose(f);
	printf("[Settings] %d read from %s\n", s_settings.count, NATIVE_SETTINGS_FILE);

	// version 2: ray tracing and the baked lighting (what shows with ray tracing
	// off) on by default; an older file turns them on once
	if (NativeSettings_GetInt("settings_version", 1) < 2)
	{
		NativeSettings_SetInt("rtx", 1);
		NativeSettings_SetInt("baked_rt", 1);
		NativeSettings_SetInt("settings_version", 2);
		NativeSettings_Save();
	}
}

int NativeSettings_Has(const char *key)
{
	return NativeSettings_Find(key) >= 0;
}

int NativeSettings_GetInt(const char *key, int fallback)
{
	int i = NativeSettings_Find(key);
	return (i < 0) ? fallback : s_settings.items[i].value;
}

void NativeSettings_SetInt(const char *key, int value)
{
	int i = NativeSettings_Find(key);
	if (i < 0)
	{
		if (s_settings.count == NATIVE_SETTINGS_MAX)
		{
			return;
		}
		i = s_settings.count++;
		snprintf(s_settings.items[i].key, sizeof(s_settings.items[i].key), "%s", key);
	}
	s_settings.items[i].value = value;
}

void NativeSettings_Save(void)
{
	FILE *f = fopen(NATIVE_SETTINGS_FILE ".tmp", "w");
	if (f == NULL)
	{
		printf("[Settings] could not write %s\n", NATIVE_SETTINGS_FILE);
		return;
	}
	fprintf(f, "# CTR-MultiTrack settings (main menu Settings / F12 overlay)\n");
	for (int i = 0; i < s_settings.count; i++)
	{
		fprintf(f, "%s=%d\n", s_settings.items[i].key, s_settings.items[i].value);
	}
	fclose(f);
#ifdef _WIN32
	remove(NATIVE_SETTINGS_FILE); // (rename doesn't replace there)
#endif
	rename(NATIVE_SETTINGS_FILE ".tmp", NATIVE_SETTINGS_FILE);
}
