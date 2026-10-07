// bootxp.h - pantalla de carga estilo "Windows XP" (logo + barra con 3 bloques azules).
// Los datos los genera herramientas/hacer_bootxp.py a partir del GIF.
#ifndef BOOTXP_H
#define BOOTXP_H

#include <stdint.h>
#include "bootxp_geom.h"

// Tiempo minimo (ms) que se ve la pantalla al ENCENDER (aunque la SD se lea antes).
// Un ciclo completo de la animacion dura 19 * 110 ms = ~2 s. Pon 0 para no esperar.
#ifndef XP_BOOT_MS
#define XP_BOOT_MS 4200
#endif
#define XP_FRAME_MS 110
#define XP_FRAMES   19

extern const uint16_t XP_PAL[256];
extern const uint16_t XP_BLOCK[XP_BLOCK_H];
extern const uint8_t  XP_IMG[];
extern const unsigned XP_IMG_LEN;

void xp_boot_start(void);              // pinta pantalla negra + logo + barra y arranca el reloj
void xp_boot_tick(void);               // avanza la barra si ya pasaron 110 ms (llamar seguido)
void xp_boot_wait(unsigned min_ms);    // anima hasta que hayan pasado min_ms desde xp_boot_start()

#endif
