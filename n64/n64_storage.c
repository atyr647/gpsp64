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
#include <dirent.h>
#include <malloc.h>

#include "../common.h"
#include "../gba_memory.h"
#include "n64_storage.h"
#include "n64_video.h"
#include "n64_input.h"

/* ROM directory on SD card */
#define ROM_DIRECTORY "sd://gba"

/* Maximum number of ROMs to list */
#define MAX_ROM_ENTRIES 64

static char rom_entries[MAX_ROM_ENTRIES][256];
static int rom_count = 0;

void n64_storage_init(void)
{
  /* SD card and filesystem are initialized via dfs_init in main */
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
    /* Check for .gba extension */
    const char *name = dir.d_name;
    size_t len = strlen(name);

    if (len > 4 && (
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
    /* Draw the ROM list */
    n64_video_clear();

    n64_video_draw_text(20, 10, "gpSP N64 - Select ROM:");
    n64_video_draw_text(20, 26, "========================");

    /* Show visible entries */
    int start = (selected / 10) * 10;
    for (int i = start; i < rom_count && i < start + 10; i++) {
      char line[280];
      /* Extract just the filename from the full path */
      const char *basename = strrchr(rom_entries[i], '/');
      basename = basename ? basename + 1 : rom_entries[i];

      snprintf(line, sizeof(line), "%c %s",
               (i == selected) ? '>' : ' ', basename);
      n64_video_draw_text(20, 42 + (i - start) * 16, line);
    }

    n64_video_draw_text(20, 210, "A=Select  D-Pad=Navigate");
    n64_video_flip();

    /* Wait for input */
    n64_input_poll();
    joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

    if (pressed.d_up && selected > 0)
      selected--;
    if (pressed.d_down && selected < rom_count - 1)
      selected++;
    if (pressed.a) {
      chosen = true;
    }
  }

  strncpy(path_out, rom_entries[selected], path_size - 1);
  path_out[path_size - 1] = '\0';
  return true;
}

/* N64-specific gamepak loading that uses stdio instead of libretro streams */
int n64_load_gamepak(const char *path)
{
  char game_code[5] = {0};

  /* Open ROM file via stdio (libdragon supports fopen on sd://) */
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return -1;

  /* Get file size */
  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  /* Round to 32 KB pages */
  gamepak_size = (u32)((file_size + 0x7FFF) & ~0x7FFF);

  /* Store path for demand paging */
  strncpy(gamepak_filename, path, sizeof(gamepak_filename) - 1);

  /* Load as many pages as we have buffer space for */
  u32 rom_blocks = gamepak_size >> 15;
  u32 buf_blocks = gamepak_size / (1024 * 1024);  /* blocks in MB */
  if (buf_blocks > (u32)gamepak_buffer_count)
    buf_blocks = gamepak_buffer_count;

  /* Read the first N megabytes into our buffers */
  for (u32 i = 0; i < buf_blocks; i++) {
    fread(gamepak_buffers[i], 1, 1024 * 1024, fp);

    /* Map 32 KB pages from this buffer */
    for (u32 j = 0; j < 32 && i * 32 + j < rom_blocks; j++) {
      u32 phyn = i * 32 + j;
      u8 *blkptr = &gamepak_buffers[i][32 * 1024 * j];
      /* Map to read handlers */
      /* Note: map_rom_entry is internal to gba_memory.c,
         we'll need to expose or replicate this */
    }
  }

  /* Read game code from ROM header */
  if (buf_blocks > 0) {
    memcpy(game_code, &gamepak_buffers[0][0xAC], 4);
    memcpy(gamepak_code, game_code, 4);
  }

  fclose(fp);

  /* Set up defaults */
  idle_loop_target_pc = 0xFFFFFFFF;
  translation_gate_targets = 0;

  return 0;
}

int n64_load_save(const char *rom_path)
{
  char save_path[512];
  size_t len = strlen(rom_path);

  /* Replace .gba extension with .sav */
  strncpy(save_path, rom_path, sizeof(save_path) - 1);
  if (len > 4) {
    strcpy(save_path + len - 4, ".sav");
  }

  FILE *fp = fopen(save_path, "rb");
  if (!fp)
    return -1;

  fread(gamepak_backup, 1, sizeof(gamepak_backup), fp);
  fclose(fp);
  return 0;
}

int n64_save_backup(const char *rom_path)
{
  char save_path[512];
  size_t len = strlen(rom_path);

  strncpy(save_path, rom_path, sizeof(save_path) - 1);
  if (len > 4) {
    strcpy(save_path + len - 4, ".sav");
  }

  FILE *fp = fopen(save_path, "wb");
  if (!fp)
    return -1;

  fwrite(gamepak_backup, 1, sizeof(gamepak_backup), fp);
  fclose(fp);
  return 0;
}
