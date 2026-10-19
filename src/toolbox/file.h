#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FS_VREFNUM  (-1)
#define FS_ROOT_DIR 2

extern char g_game_dir[1024];  /* host folder that plays the role of the app folder */

/* Map a Mac (vRefNum, dirID, name/partial path) to a host path.
 * rsrc=true selects the resource fork ("<name>.rsrc"). Returns false if the
 * name is empty/invalid; the file need not exist. */
bool fs_resolve(int16_t vref, int32_t dir, const char *name, char *out, size_t outsz, bool rsrc);
bool fs_exists(const char *path);
void file_register_traps(void);

/* driver hooks */
int16_t net_driver_control(uint32_t pb, bool async);
