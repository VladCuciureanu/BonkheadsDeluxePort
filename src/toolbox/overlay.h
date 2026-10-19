/* Port-only UI drawn on top of the game at presentation time (never into the
 * game's own framebuffer): network status banner and main-menu hints. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Set the banner (two lines; NULL/empty clears that line). Both NULL hides it. */
void overlay_banner(const char *line1, const char *line2);
/* Compose the overlay onto `fb` (640x480 indices); returns the buffer to present. */
const uint8_t *overlay_compose(const uint8_t *fb);
