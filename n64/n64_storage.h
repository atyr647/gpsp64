/* gameplaySP - N64 Storage / ROM Loading
 *
 * N64 port Copyright (C) 2026
 */

#ifndef N64_STORAGE_H
#define N64_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

/* Initialize the SD card filesystem */
void n64_storage_init(void);

/* Browse ROMs on SD card, return selected path */
bool n64_storage_browse_roms(char *path_out, size_t path_size);

/* Load a GBA ROM from SD card using demand paging */
int n64_load_gamepak(const char *path);

/* Load a save file from SD card */
int n64_load_save(const char *rom_path);

/* Write save file to SD card */
int n64_save_backup(const char *rom_path);

#endif
