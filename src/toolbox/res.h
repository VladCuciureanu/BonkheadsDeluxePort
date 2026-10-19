#pragma once
#include <stdint.h>
#include <stdbool.h>

int16_t res_open_path(const char *path, bool readonly);
bool res_create_path(const char *path);
void res_close(int16_t ref);
void res_set_app_file(int16_t ref);
int16_t res_cur_file(void);
int16_t res_error(void);
uint32_t res_get(uint32_t type, int16_t id, bool one);
uint32_t res_get_named(uint32_t type, const char *name, bool one);
const uint8_t *res_peek(uint32_t type, int16_t id, uint32_t *len);
void res_register_traps(void);

#define OSTYPE(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
