// bootxp.c - dibuja la pantalla de carga estilo Windows XP.
// Memoria: ~3 KB de RAM (banda de 8 filas), la imagen vive en flash (bootxp_data.c).
#include <stdbool.h>
#include "bootxp.h"
#include "bootxp_foot.h"
#include "st7789.h"

#if defined(__has_include) && __has_include("pico/stdlib.h")
#include "pico/stdlib.h"
static unsigned ahora_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static void dormir_ms(unsigned ms) { sleep_ms(ms); }
#else   // prueba en PC (herramientas/ui_preview.c): reloj falso, avanza 15 ms por consulta
static unsigned t_falso;
static unsigned ahora_ms(void) { return t_falso += 15; }
static void dormir_ms(unsigned ms) { t_falso += ms; }
#endif

#define BANDA 8
static uint16_t banda[XP_IMG_W * BANDA];
static unsigned frame_n, t_ultimo, t_inicio;

// Imagen estatica: paleta + RLE (0,n = n pixeles negros; otro byte = color).
static void dibujar_imagen(void) {
    unsigned pos = 0, negros = 0;
    for (int y = 0; y < XP_IMG_H; y += BANDA) {
        const int hh = (XP_IMG_H - y < BANDA) ? XP_IMG_H - y : BANDA;
        const int n = XP_IMG_W * hh;
        for (int i = 0; i < n; i++) {
            if (negros) { negros--; banda[i] = 0; continue; }
            const uint8_t b = XP_IMG[pos++];
            if (b == 0) { negros = XP_IMG[pos++]; i--; continue; }
            banda[i] = XP_PAL[b];
        }
        st7789_blit(XP_IMG_X, (uint16_t)(XP_IMG_Y + y), XP_IMG_W, (uint16_t)hh, banda);
    }
}

// Interior de la barra: 15 huecos, 3 bloques en fila que avanzan 1 hueco por paso.
// 19 pasos = 1 vacio + 15 + 3 de salida, igual que el GIF.
static void dibujar_barra(unsigned f) {
    enum { W = XP_SLOTS * XP_PITCH };
    static uint16_t tira[W * XP_BLOCK_H];
    const int cabeza = (int)(f % XP_FRAMES) - 1;
    for (int i = 0; i < W * XP_BLOCK_H; i++) tira[i] = 0;
    for (int k = 0; k < 3; k++) {
        const int s = cabeza - k;
        if (s < 0 || s >= XP_SLOTS) continue;
        for (int y = 0; y < XP_BLOCK_H; y++)
            for (int x = 0; x < XP_BLOCK_W; x++)
                tira[y * W + s * XP_PITCH + x] = XP_BLOCK[y];
    }
    st7789_blit(XP_BAR_X, XP_BAR_Y, W, XP_BLOCK_H, tira);
}


// Pie: texto de copyright (izquierda) y logo Microsoft (derecha), abajo del todo.
static void dibujar_gris(int x, int y, int w, int h, const uint8_t *g) {
    static uint16_t fila[XP_FOOT_TXT_W > XP_FOOT_LOGO_W ? XP_FOOT_TXT_W : XP_FOOT_LOGO_W];
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            const unsigned v = g[j * w + i];
            fila[i] = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
        }
        st7789_blit((uint16_t)x, (uint16_t)(y + j), (uint16_t)w, 1, fila);
    }
}

static void dibujar_pie(void) {
    const int y = 240 - 6 - XP_FOOT_TXT_H;
    dibujar_gris(6, y, XP_FOOT_TXT_W, XP_FOOT_TXT_H, XP_FOOT_TXT);
    dibujar_gris(320 - 6 - XP_FOOT_LOGO_W, 240 - 6 - XP_FOOT_LOGO_H,
                 XP_FOOT_LOGO_W, XP_FOOT_LOGO_H, XP_FOOT_LOGO);
}

void xp_boot_start(void) {
    st7789_fill_screen(0);
    dibujar_imagen();
    dibujar_pie();
    frame_n = 0;
    dibujar_barra(frame_n);
    t_inicio = t_ultimo = ahora_ms();
}

void xp_boot_tick(void) {
    const unsigned t = ahora_ms();
    if (t - t_ultimo < XP_FRAME_MS) return;
    t_ultimo = t;
    dibujar_barra(++frame_n);
}

void xp_boot_wait(unsigned min_ms) {
    while (ahora_ms() - t_inicio < min_ms) {
        xp_boot_tick();
        dormir_ms(5);
    }
}
