/* Port-side tweaks to the game's data, applied by the Resource Manager when a
 * resource is loaded or added. They never touch player-made data. */
#include "toolbox.h"
#include "res.h"

/* 'HiSc' 128: one-player high scores, 10 entries (1-based i):
 *   score  long   at 4*i
 *   name   Str27  at 44 + 28*i
 *   level  long   at 352 + 4*i */
#define HISC_SIZE 0x18C
#define HS_SCORE(i) (4 * (i))
#define HS_NAME(i) (44 + 28 * (i))
#define HS_LEVEL(i) (352 + 4 * (i))

static const struct { int rank; const char *name; uint32_t score; uint32_t level; } extra_scores[] = {
    { 2, "Vlad", 13337, 10 },
};

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

void port_patch_resource(uint32_t type, int16_t id, uint32_t h)
{
    if (type != OSTYPE('H','i','S','c') || id != 128) return;
    if (!h || !rdl(h) || mm_handle_size(h) < HISC_SIZE) return;
    uint32_t p = rdl(h);
    if (!hisc_is_factory_default(p)) return;
    for (size_t k = 0; k < sizeof extra_scores / sizeof extra_scores[0]; k++)
        hisc_insert(p, extra_scores[k].rank, extra_scores[k].name, extra_scores[k].score, extra_scores[k].level);
}
