/* gameplaySP - N64 Storage / ROM Loading
 *
 * Handles ROM loading from SD card using demand paging.
 * GBA ROMs can be up to 32 MB but N64 only has 4-8 MB RAM,
 * so we stream ROM data from SD card in 32 KB pages.
 *
 * N64 port Copyright (C) 2026
 */

#include <libdragon.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../common.h"
#include "../gba_memory.h"
#include "n64_storage.h"
#include "n64_video.h"
#include "n64_input.h"

/* ROM directory on SD card */
#define ROM_DIRECTORY "sd:/gba"

/* Maximum number of ROMs to list */
#define MAX_ROM_ENTRIES 64

static char rom_entries[MAX_ROM_ENTRIES][256];
static int rom_count = 0;

void n64_storage_init(void)
{
  debugf("N64 storage initialized\n");
}

/* Scan the ROM directory for .gba files */
static int scan_rom_directory(void)
{
  dir_t dir;
  int err;

  rom_count = 0;

  err = dir_findfirst(ROM_DIRECTORY, &dir);
  while (err == 0 && rom_count < MAX_ROM_ENTRIES) {
    const char *name = dir.d_name;
    size_t len = strlen(name);

    /* Check for .gba/.bin/.agb extension */
    if (len > 4 && dir.d_type == DT_REG && (
        strcasecmp(name + len - 4, ".gba") == 0 ||
        strcasecmp(name + len - 4, ".bin") == 0 ||
        strcasecmp(name + len - 4, ".agb") == 0)) {
      snprintf(rom_entries[rom_count], sizeof(rom_entries[0]),
               "%s/%s", ROM_DIRECTORY, name);
      rom_count++;
    }

    err = dir_findnext(ROM_DIRECTORY, &dir);
  }

  return rom_count;
}

bool n64_storage_browse_roms(char *path_out, size_t path_size)
{
  if (scan_rom_directory() == 0)
    return false;

  int selected = 0;
  bool chosen = false;

  while (!chosen) {
    surface_t *disp = display_get();
    if (!disp) continue;

    /* Clear screen */
    graphics_fill_screen(disp, 0x00000001);
    graphics_set_color(0xFFFFFFFF, 0x00000001);

    graphics_draw_text(disp, 20, 10, "gpSP N64 - Select ROM:");

    /* Show visible entries (10 at a time) */
    int start = (selected / 10) * 10;
    for (int i = start; i < rom_count && i < start + 10; i++) {
      char line[280];
      const char *basename = strrchr(rom_entries[i], '/');
      basename = basename ? basename + 1 : rom_entries[i];

      snprintf(line, sizeof(line), "%c %s",
               (i == selected) ? '>' : ' ', basename);
      graphics_draw_text(disp, 20, 30 + (i - start) * 16, line);
    }

    graphics_draw_text(disp, 20, 210, "A=Select  D-Pad=Navigate");
    display_show(disp);

    /* Wait for input */
    joypad_poll();
    joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

    if (pressed.d_up && selected > 0)
      selected--;
    if (pressed.d_down && selected < rom_count - 1)
      selected++;
    if (pressed.a)
      chosen = true;
  }

  strncpy(path_out, rom_entries[selected], path_size - 1);
  path_out[path_size - 1] = '\0';
  return true;
}

int n64_load_save(const char *rom_path)
{
  char save_path[512];
  size_t len = strlen(rom_path);

  strncpy(save_path, rom_path, sizeof(save_path) - 1);
  save_path[sizeof(save_path) - 1] = '\0';
  if (len > 4)
    strcpy(save_path + len - 4, ".sav");

  FILE *fp = fopen(save_path, "rb");
  if (!fp) return -1;
  fread(gamepak_backup, 1, sizeof(gamepak_backup), fp);
  fclose(fp);
  return 0;
}

int n64_save_backup(const char *rom_path)
{
  char save_path[512];
  size_t len = strlen(rom_path);

  strncpy(save_path, rom_path, sizeof(save_path) - 1);
  save_path[sizeof(save_path) - 1] = '\0';
  if (len > 4)
    strcpy(save_path + len - 4, ".sav");

  FILE *fp = fopen(save_path, "wb");
  if (!fp) return -1;
  fwrite(gamepak_backup, 1, sizeof(gamepak_backup), fp);
  fclose(fp);
  return 0;
}
