/* HLE initialisation: registers every trap family and sets up low memory. */
#include "qd.h"
#include "res.h"
#include "file.h"
#include "event.h"
#include "sound.h"
#include "netcfg.h"
#include <stdio.h>

void trap_register_core(void);
void mem_register_traps(void);
void res_register_traps(void);
void file_register_traps(void);
void qd_register_traps(void);
void qdcolor_register_traps(void);
void pict_register_traps(void);
void text_register_traps(void);
void window_register_traps(void);
void dialog_register_traps(void);
void misc_register_traps(void);
void qd_init(void);

void toolbox_init(void)
{
    trap_register_core();
    mem_register_traps();
    res_register_traps();
    file_register_traps();
    qd_register_traps();
    qdcolor_register_traps();
    pict_register_traps();
    text_register_traps();
    window_register_traps();
    dialog_register_traps();
    event_register_traps();
    sound_register_traps();
    misc_register_traps();

    mm_init();
    wrl(LM_CurrentA5, A[5]);
    wrl(LM_CurStackBase, STACK_TOP);
    c_to_pstr("Bonkheads Deluxe", LM_CurApName);
    wrl(LM_Time, plat_secs_since_1904());
    wrw(0x0BA8, 12);         /* SysFontSize */
    wrw(LM_DlgFont, 0);
    wrw(LM_ROM85, 0x3FFF);   /* Color QuickDraw present */
    wrb(0x0BB2, 0);          /* SegHiEnable */
    wrb(0x012D, 0);          /* LoadTrap */
    wrl(0x02F0, 30);         /* DoubleTime */
    qd_init();
    evt_init();

    char app[1100];
    snprintf(app, sizeof app, "%s/Bonkheads Deluxe.rsrc", g_game_dir);
    int16_t ref = res_open_path(app, true);
    if (ref < 0) log_fatal("cannot open the game's resources at %s", app);
    res_set_app_file(ref);
}
