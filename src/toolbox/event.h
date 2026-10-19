#pragma once
#include <stdint.h>
#include <stdbool.h>

void evt_init(void);
void evt_idle(void);                       /* pump OS events, network, audio, present */
bool evt_button_down(void);
void evt_mouse_local(uint32_t port, int *h, int *v);
bool evt_next(uint16_t mask, uint32_t ev, bool remove);   /* fills emulated EventRecord */
bool evt_quit_requested(void);
void event_register_traps(void);
