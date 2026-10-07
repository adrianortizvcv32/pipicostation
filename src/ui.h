// =====================================================================
//  ui.h - capa de interfaz del launcher (colores, texto, tarjetas,
//  barras, pantallas). Todo se compone en un canvas RAM pequeno y se
//  envia a la pantalla con UN solo blit por banda -> sin parpadeo.
//  Solo depende de st7789.h (blit / fill_rect), no del SDK.
// =====================================================================
#ifndef UI_LAUNCHER_H
#define UI_LAUNCHER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "st7789.h"

#define UI_W ST7789_SCREEN_W
#define UI_H ST7789_SCREEN_H

// ---------------- paleta ----------------
#define UI_BG       RGB565(12, 14, 26)
#define UI_CARD     RGB565(24, 28, 46)
#define UI_CARD_HI  RGB565(36, 42, 68)
#define UI_TXT      RGB565(236, 238, 246)
#define UI_DIM      RGB565(128, 134, 160)
#define UI_ACC1     RGB565(56, 110, 255)    // azul
#define UI_ACC2     RGB565(138, 70, 235)    // violeta
#define UI_OK       RGB565(86, 224, 130)
#define UI_WARN     RGB565(255, 196, 64)
#define UI_ERR      RGB565(240, 78, 78)
#define UI_VID      RGB565(255, 120, 90)    // color de la pestana VIDEOS

// ---- colores "llave" (se sustituyen en ui_end(), no son colores reales) ----
//  UI_WALL  = transparente: se ve el fondo (FONDO, fondo.c) tal cual.
//  UI_GLASS = "vidrio": tarjeta oscura semitransparente sobre el fondo.
#define UI_WALL     ((uint16_t)0x0020)
#define UI_GLASS    ((uint16_t)0x0041)
#define UI_GLASS_A  24                      // opacidad del vidrio, 0..32

// ---------------- layout de la ventana XP ("Mi PC") ----------------
// Ventana: x 8..312, y 6..220. Lista a la derecha, panel azul a la izquierda.
#define UI_LIST_Y     62
#define UI_ROW_H      18
#define UI_ROWS       8

enum { UI_KIND_GAME = 0, UI_KIND_VIDEO = 1 };

typedef struct {
    const char *name;     // texto principal
    const char *badge;    // texto a la derecha (duracion, "LISTO"...) o NULL
    uint16_t    badge_color;
    int         kind;     // UI_KIND_*
} ui_item_t;

// ---------------- primitivas (sobre el canvas) ----------------
void     ui_begin(int w, int h, uint16_t bg);     // w*h <= 320*32 (bg: color, UI_WALL o UI_GLASS)
void     ui_end(int x, int y);                    // envia el canvas a la pantalla
void     ui_rect(int x, int y, int w, int h, uint16_t c);
void     ui_round(int x, int y, int w, int h, int r, uint16_t c);
void     ui_round_grad(int x, int y, int w, int h, int r, uint16_t c0, uint16_t c1);
void     ui_vgrad(int x, int y, int w, int h, uint16_t c0, uint16_t c1);
void     ui_text(int x, int y, const char *s, uint16_t fg, int sc);
void     ui_text_sh(int x, int y, const char *s, uint16_t fg, uint16_t sh, int sc);
void     ui_text_b(int x, int y, const char *s, uint16_t fg, int sc);   // "negrita"
int      ui_text_w(const char *s, int sc);
uint16_t ui_mix(uint16_t a, uint16_t b, int t256);
void     ui_fit(char *dst, size_t n, const char *src, int max_px, int sc);
void     ui_fmt_time(char *out, size_t n, unsigned seconds);

// ---------------- fondo ----------------
void     ui_wallpaper(void);                                    // pinta el fondo en toda la pantalla
void     ui_panel(int x, int y, int w, int h);                  // zona de vidrio sobre el fondo

// ---------------- piezas del menu ----------------
void ui_desktop(void);                                          // escritorio XP (fondo + globito START)
void ui_window(int tab, int n_games, int n_videos);             // ventana "Mis juegos"/"Mis videos"
void ui_row(int pos, const ui_item_t *it, bool sel);            // it==NULL -> fila vacia
void ui_scrollbar(int top, int visible, int total);
void ui_details(const char *name, const char *l2, const char *l3);   // panel "Detalles"
void ui_empty(const char *l1, const char *l2);                  // lista vacia

// ---------------- pantallas completas ----------------
void ui_splash(const char *msg, int pct);                       // pct<0: sin barra
void ui_message(const char *titulo, uint16_t color, const char *l1,
                const char *l2, const char *l3, const char *l4);
void ui_line_c(int y, const char *s, uint16_t fg, int sc);      // linea centrada sobre fondo
void ui_bar(int y, uint32_t cur, uint32_t tot, uint16_t color); // barra de progreso grande

// ---------------- reproductor ----------------
// Barras de estado sobre las franjas negras del video (alto >= 20).
void ui_hud_top(int y, int h, const char *title);
void ui_hud_bottom(int y, int h, unsigned cur_s, unsigned tot_s, bool paused);
// Cuadro "PAUSA" / "FIN" centrado (se pinta sobre el video; al reanudar
// el siguiente cuadro lo borra).
void ui_overlay(const char *txt, uint16_t color);
// Indicador de volumen (altavoz + barritas) en la esquina superior derecha.
void ui_vol_badge(int vol, int vmax);

#endif
