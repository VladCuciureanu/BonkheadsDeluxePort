/* Bonkheads Deluxe source port: entry point. */
#include "runtime/cpu.h"
#include "runtime/layout.h"
#include "toolbox/toolbox.h"
#include "toolbox/file.h"
#include "toolbox/sound.h"
#include "toolbox/netcfg.h"
#include "platform/platform.h"
#include "platform/log.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    printf("Bonkheads Deluxe (cross-platform port)\n"
           "usage: bonkheads [options]\n\n"
           "Game:\n"
           "  --data DIR            folder with the extracted game files\n"
           "                        (default: <exe dir>/data/Bonkheads Deluxe)\n"
           "  --scale N             window scale factor (default 2)\n"
           "  --fullscreen          start in fullscreen (Alt+Enter toggles)\n"
           "  --nosound             disable audio\n"
           "  --pixel-text          draw text in the original low-resolution fonts\n"
           "\n"
           "Network (see docs/network.md):\n"
           "  --port N              UDP port the host listens on (default 24731)\n"
           "  --connect HOST[:PORT] NET JOIN: join this host without asking (repeatable)\n"
           "  --hold N              ticks a game packet may wait for the game (default 8)\n"
           "  --no-redundancy       send each game packet once instead of twice\n"
           "  --raw-net             no pacing or redundancy (original behaviour)\n"
           "  --dedup               strip repeated registered events (experimental)\n"
           "  --sim-latency MS      testing: delay every outgoing packet\n"
           "  --sim-loss PCT        testing: drop this share of outgoing packets\n"
           "\n"
           "Debugging and automation:\n"
           "  --trace               log every Toolbox call\n"
           "  --netlog              log network events\n"
           "  --headless            hidden window; real keyboard and mouse are ignored\n"
           "  --shot SECONDS FILE   save a screenshot (.bmp) after SECONDS, then exit\n"
           "  --press T:KEY[:FRAMES] at T seconds, hold Mac key code KEY (e.g. 0x24 Return)\n"
           "  --click T:X:Y         at T seconds, click at game coordinates X,Y\n"
           "  --move T:X:Y          at T seconds, move the mouse to X,Y\n"
           "  --snap SECONDS        save an extra screenshot (FILE-SECONDS.bmp), keep running\n"
           "  --dump-a5 OFF:LEN     log LEN bytes of game globals at A5+OFF at --shot time\n"
           "  --poke-a5 T:OFF:HEX[:FUNC] at T seconds (once FUNC runs), write bytes at A5+OFF\n");
}

/* hang diagnostics: report where the game is stuck if it stops reaching the
 * event loop for a few seconds */
/* debug: --dump-a5 OFFSET:LEN (repeatable) logs A5-relative game globals at --shot time */
static struct { int off, len; } dumps[16];
static int ndumps;

void debug_dump_globals(void)
{
    for (int i = 0; i < ndumps; i++) {
        char hex[3 * 64 + 1] = "";
        int n = dumps[i].len > 64 ? 64 : dumps[i].len;
        for (int k = 0; k < n; k++) snprintf(hex + 3 * k, 4, "%02X ", rdb(A[5] + dumps[i].off + k));
        log_info("A5%+d: %s", dumps[i].off, hex);
    }
}

/* debug: --poke-a5 SECONDS:OFFSET:HEXBYTES[:FUNC] writes A5-relative game
 * globals at a given time (optionally only once the original function FUNC is
 * running, e.g. STANDARDPLAY for "during gameplay"); --snap SECONDS saves an extra screenshot (FILE-SECONDS.bmp)
 * without exiting */
static struct { double at; int off, len; uint8_t bytes[16]; char fn[64]; bool done; } pokes[32];
static int npokes;
static struct { double at; bool done; } snaps[32];
static int nsnaps;
extern char g_shot_path[512];

void debug_tick(double now)
{
    double batch = -1;   /* pokes given the same time apply together; later ones wait for the next tick */
    for (int i = 0; i < npokes; i++)
        if (!pokes[i].done && now >= pokes[i].at && (batch < 0 || pokes[i].at == batch)) {
            const char *fn = func_name_for(g_cur_fn);
            if (pokes[i].fn[0] && (!fn || strcmp(fn, pokes[i].fn))) continue;
            pokes[i].done = true; batch = pokes[i].at;
            for (int k = 0; k < pokes[i].len; k++) wrb(A[5] + pokes[i].off + k, pokes[i].bytes[k]);
            log_info("poked A5%+d (%d bytes)", pokes[i].off, pokes[i].len);
        }
    for (int i = 0; i < nsnaps; i++)
        if (!snaps[i].done && now >= snaps[i].at) {
            snaps[i].done = true;
            char path[600]; size_t n = strlen(g_shot_path);
            if (n > 4 && !strcmp(g_shot_path + n - 4, ".bmp")) n -= 4;
            snprintf(path, sizeof path, "%.*s-%g.bmp", (int)n, g_shot_path, snaps[i].at);
            plat_request_capture(path);
            log_info("snapshot saved to %s", path);
            break;   /* one capture per presented frame */
        }
}

void stackdump_init(void);
void stackdump_main_thread(void);

static int SDLCALL watchdog(void *arg)
{
    (void)arg;
    extern volatile uint64_t g_last_idle_us;
    extern const char *trap_name(uint16_t op);
    uint64_t reported = 0;
    for (;;) {
        SDL_Delay(1000);
        uint64_t last = g_last_idle_us, now = plat_micros();
        if (last && now - last > 3000000 && last != reported) {
            reported = last;
            uint32_t fn = g_cur_fn; uint16_t trap = g_cur_trap;
            const char *name = func_name_for(fn);
            log_warn("watchdog: no event-loop activity for %.1fs; in %s (0x%08X), trap %s ($%04X)",
                     (now - last) / 1e6, name ? name : "?", fn, trap ? trap_name(trap) : "none", trap);
            stackdump_main_thread();
        }
    }
    return 0;
}

void app_quit(void)
{
    sound_update();
    plat_shutdown();
    exit(0);
}

int main(int argc, char **argv)
{
    int scale = 2;
    bool fullscreen = false, nosound = false;
    const char *data = NULL;
    g_netcfg.hold_ticks = 8;
    g_netcfg.redundancy = true;
    udp_init();
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(a, "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(a, "--fullscreen")) fullscreen = true;
        else if (!strcmp(a, "--nosound")) nosound = true;
        else if (!strcmp(a, "--headless")) { plat_set_headless(true); g_headless = 1; }
        else if (!strcmp(a, "--port") && i + 1 < argc) g_netcfg.port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(a, "--hold") && i + 1 < argc) g_netcfg.hold_ticks = atoi(argv[++i]);
        else if (!strcmp(a, "--no-redundancy")) g_netcfg.redundancy = false;
        else if (!strcmp(a, "--raw-net")) { g_netcfg.raw = true; g_netcfg.redundancy = false; }
        else if (!strcmp(a, "--dedup")) g_netcfg.dedup = true;
        else if (!strcmp(a, "--sim-latency") && i + 1 < argc) g_netcfg.sim_latency_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--sim-loss") && i + 1 < argc) g_netcfg.sim_loss_pct = atoi(argv[++i]);
        else if (!strcmp(a, "--connect") && i + 1 < argc) {
            const char *h = argv[++i];
            if (g_netcfg.nconnect < 8 && udp_resolve(h, g_netcfg.port, &g_netcfg.connect[g_netcfg.nconnect])) g_netcfg.nconnect++;
            else fprintf(stderr, "cannot resolve %s\n", h);
        }
        else if (!strcmp(a, "--trace")) g_log_trace = 1;
        else if (!strcmp(a, "--dump-a5") && i + 1 < argc && ndumps < 16) {
            if (sscanf(argv[++i], "%i:%i", &dumps[ndumps].off, &dumps[ndumps].len) == 2) ndumps++;
        }
        else if (!strcmp(a, "--poke-a5") && i + 1 < argc && npokes < 32) {
            char hex[64], fn[64] = ""; double at; int off;
            if (sscanf(argv[++i], "%lf:%i:%63[0-9a-fA-F]:%63s", &at, &off, hex, fn) >= 3) {
                snprintf(pokes[npokes].fn, sizeof pokes[npokes].fn, "%s", fn);
                int len = 0;
                for (const char *h = hex; h[0] && h[1] && len < 16; h += 2) {
                    unsigned v; if (sscanf(h, "%2x", &v) != 1) break;
                    pokes[npokes].bytes[len++] = (uint8_t)v;
                }
                pokes[npokes].at = at; pokes[npokes].off = off; pokes[npokes].len = len; npokes++;
            }
        }
        else if (!strcmp(a, "--snap") && i + 1 < argc && nsnaps < 32) snaps[nsnaps++].at = atof(argv[++i]);
        else if (!strcmp(a, "--pixel-text")) { extern bool g_text_hires; g_text_hires = false; }
        else if (!strcmp(a, "--netlog")) { extern int g_netlog; g_netlog = 1; }
        else if (!strcmp(a, "--shot") && i + 2 < argc) {
            extern double g_shot_after; extern char g_shot_path[512];
            g_shot_after = atof(argv[++i]); snprintf(g_shot_path, sizeof g_shot_path, "%s", argv[++i]);
            g_headless = 1;
        }
        else if (!strcmp(a, "--press") && i + 1 < argc) {
            double at = 0; int mac = 0, frames = 6;
            if (sscanf(argv[++i], "%lf:%i:%d", &at, &mac, &frames) >= 2) plat_script_key(at, mac, frames);
        }
        else if (!strcmp(a, "--move") && i + 1 < argc) {
            double at = 0; int x = 0, y = 0;
            if (sscanf(argv[++i], "%lf:%d:%d", &at, &x, &y) == 3) plat_script_move(at, x, y);
        }
        else if (!strcmp(a, "--click") && i + 1 < argc) {
            double at = 0; int x = 0, y = 0;
            if (sscanf(argv[++i], "%lf:%d:%d", &at, &x, &y) == 3) plat_script_click(at, x, y);
        }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 1; }
    }
    if (data) snprintf(g_game_dir, sizeof g_game_dir, "%s", data);
    else snprintf(g_game_dir, sizeof g_game_dir, "%sdata/Bonkheads Deluxe", SDL_GetBasePath());
    for (char *p = g_game_dir; *p; p++) if (*p == '\\') *p = '/';
    size_t l = strlen(g_game_dir);
    if (l && g_game_dir[l - 1] == '/') g_game_dir[l - 1] = 0;

    if (!plat_init(scale, fullscreen)) log_fatal("failed to initialise SDL video");
    cpu_init();
    toolbox_init();
    sound_init(!nosound);
    net_init();
    log_info("game data: %s", g_game_dir);
    stackdump_init();
    SDL_DetachThread(SDL_CreateThread(watchdog, "watchdog", NULL));
    cpu_run_main();
    app_quit();
    return 0;
}
