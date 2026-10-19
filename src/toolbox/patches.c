/* Port-side tweaks to the game's data, applied by the Resource Manager when a
 * resource is loaded or added. They never touch player-made data. */
#include "toolbox.h"
#include "res.h"
#include "file.h"
#include <stdio.h>
#include <stdlib.h>

/* Optional extra entries for the factory high-score table, read from
 * "highscores.cfg" in the game data folder. One entry per line:
 *     name, score, level
 * Lines starting with # are ignored. Each entry is inserted at the rank its
 * score earns; the table keeps its 10 slots. */

/* 'HiSc' 128: one-player high scores, 10 entries (1-based i):
 *   score  long   at 4*i
 *   name   Str27  at 44 + 28*i
 *   level  long   at 352 + 4*i */
#define HISC_SIZE 0x18C
#define HS_SCORE(i) (4 * (i))
#define HS_NAME(i) (44 + 28 * (i))
#define HS_LEVEL(i) (352 + 4 * (i))

static bool name_is(uint32_t p, int i, const char *s)
{
    size_t n = strlen(s);
    return rdb(p + HS_NAME(i)) == n && memcmp(HP(p + HS_NAME(i) + 1), s, n) == 0;
}

/* only the untouched table the game creates on first run is changed */
static bool hisc_is_factory_default(uint32_t p)
{
    return name_is(p, 1, "Mr. Rogers") && rdl(p + HS_SCORE(1)) == 1223123 &&
           name_is(p, 2, "Jon T.") && rdl(p + HS_SCORE(2)) == 10000;
}

static void hisc_insert(uint32_t p, int rank, const char *name, uint32_t score, uint32_t level)
{
    for (int i = 10; i > rank; i--) {
        wrl(p + HS_SCORE(i), rdl(p + HS_SCORE(i - 1)));
        memcpy(HP(p + HS_NAME(i)), HP(p + HS_NAME(i - 1)), 28);
        wrl(p + HS_LEVEL(i), rdl(p + HS_LEVEL(i - 1)));
    }
    wrl(p + HS_SCORE(rank), score);
    memset(HP(p + HS_NAME(rank)), 0, 28);
    c_to_pstr(name, p + HS_NAME(rank));
    wrl(p + HS_LEVEL(rank), level);
}

static void hisc_add_sorted(uint32_t p, const char *name, uint32_t score, uint32_t level)
{
    int rank = 1;
    while (rank <= 10 && rdl(p + HS_SCORE(rank)) >= score) rank++;
    if (rank <= 10) hisc_insert(p, rank, name, score, level);
}

static void hisc_add_from_config(uint32_t p)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/highscores.cfg", g_game_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char name[28];
        unsigned long score, level;
        if (line[0] == '#') continue;
        if (sscanf(line, " %27[^,\r\n] , %lu , %lu", name, &score, &level) != 3) continue;
        for (size_t n = strlen(name); n && name[n - 1] == ' '; n--) name[n - 1] = 0;
        hisc_add_sorted(p, name, (uint32_t)score, (uint32_t)level);
    }
    fclose(f);
}

void port_patch_resource(uint32_t type, int16_t id, uint32_t h)
{
    if (type != OSTYPE('H','i','S','c') || id != 128) return;
    if (!h || !rdl(h) || mm_handle_size(h) < HISC_SIZE) return;
    uint32_t p = rdl(h);
    if (!hisc_is_factory_default(p)) return;
    hisc_add_from_config(p);
}
