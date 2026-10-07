// fondo.h - imagen de fondo del launcher (320x240, RGB565, en flash).
// El archivo fondo.c lo genera herramientas/hacer_fondo.py
#ifndef FONDO_LAUNCHER_H
#define FONDO_LAUNCHER_H
#include <stdint.h>

#define FONDO_W 320
#define FONDO_H 240

extern const uint16_t FONDO[FONDO_W * FONDO_H];

#endif
