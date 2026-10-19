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
    printf("Bonkheads Deluxe (cross-platform port)\n\n"
           "  --data DIR          folder containing the extracted game files\n"
           "                      (default: <exe dir>/data/Bonkheads Deluxe)\n"
           "  --scale N           window scale factor (default 2)\n"
           "  --fullscreen        start in fullscreen (Alt+Enter toggles)\n"
           "  --nosound           disable audio\n"
           "  --port N            UDP port for network games (default 24731)\n"
           "  --connect HOST[:P]  network host: also look for a joiner at HOST\n"
           "                      (may be repeated; LAN joiners are found automatically)\n"
           "  --hold N            max ticks a game packet waits for the game (default 8)\n"
           "  --no-redundancy     send each game packet once instead of twice\n"
           "  --trace             verbose trap tracing\n");
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
    cpu_run_main();
    app_quit();
    return 0;
}
