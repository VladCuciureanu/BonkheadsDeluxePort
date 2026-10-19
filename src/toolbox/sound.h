#pragma once
#include <stdint.h>
#include <stdbool.h>

void sound_init(bool enabled);
void sound_update(void);
void sound_register_traps(void);
void sound_beep(void);
