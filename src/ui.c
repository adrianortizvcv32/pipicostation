// =====================================================================
//  ui.c - interfaz del launcher. Ver ui.h.
// =====================================================================
#include "ui.h"
#include "bootxp.h"
#include <string.h>
#include <stdio.h>
#include "font6x10.h"
#include "fondo.h"

#define CV_MAXH 32
static uint16_t cvbuf[UI_W * CV_MAXH] __attribute__((aligned(4)));
static int cw = UI_W, ch = 1;
static int oy = 0;               // origen Y del canvas (0 salvo al dibujar por bandas)

// ---------------------------------------------------------------------
//  canvas
// ---------------------------------------------------------------------
void ui_begin(int w, int h, uint16_t bg) {
    if (w > UI_W) w = UI_W;
    if (h > CV_MAXH) h = CV_MAXH;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    cw = w; ch = h; oy = 0;
    for (int i = 0; i < w * h; i++) cvbuf[i] = bg;
}

// Mezcla "vidrio" (UI_CARD) sobre un pixel del fondo. Truco 5-6-5 en 32 bits.
static inline uint16_t glass_over(uint16_t bgpx) {
    const uint32_t M = 0x07E0F81Fu;
    uint32_t b = ((uint32_t)bgpx | ((uint32_t)bgpx << 16)) & M;
    uint32_t f = ((uint32_t)UI_CARD | ((uint32_t)UI_CARD << 16)) & M;
    uint32_t r = ((((f - b) * UI_GLASS_A) >> 5) + b) & M;
    return (uint16_t)(r | (r >> 16));
}

void ui_end(int x, int y) {
    // Los pixeles con color llave se sustituyen por el fondo (o por vidrio).
    for (int j = 0; j < ch; j++) {
        const int yy = y + j;
        if ((unsigned)yy >= (unsigned)UI_H) break;
        uint16_t *p = cvbuf + j * cw;
        for (int i = 0; i < cw; i++) {
            const uint16_t v = p[i];
            if (v != UI_WALL && v != UI_GLASS) continue;
            const int xx = x + i;
            if ((unsigned)xx >= (unsigned)UI_W) continue;
            const uint16_t f = FONDO[yy * UI_W + xx];
            p[i] = (v == UI_WALL) ? f : glass_over(f);
        }
    }
    st7789_blit((uint16_t)x, (uint16_t)y, (uint16_t)cw, (uint16_t)ch, cvbuf);
}

void ui_wallpaper(void) {
    st7789_blit(0, 0, UI_W, UI_H, FONDO);          // directo desde la flash
}

void ui_panel(int x, int y, int w, int h) {
    for (int off = 0; off < h; off += CV_MAXH) {
        int hh = h - off < CV_MAXH ? h - off : CV_MAXH;
        ui_begin(w, hh, UI_GLASS);
        ui_end(x, y + off);
    }
}

static inline void put(int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)cw && (unsigned)y < (unsigned)ch) cvbuf[y * cw + x] = c;
}

void ui_rect(int x, int y, int w, int h, uint16_t c) {
    y -= oy;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > cw) x1 = cw;
    if (y1 > ch) y1 = ch;
    for (int j = y; j < y1; j++) {
        uint16_t *p = cvbuf + j * cw;
        for (int i = x; i < x1; i++) p[i] = c;
    }
}

uint16_t ui_mix(uint16_t a, uint16_t b, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (br - ar) * t / 255;
    int g = ag + (bg - ag) * t / 255;
    int bl = ab + (bb - ab) * t / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static int isqrt(int v) {
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

// Cuanto se recorta la fila dy de una esquina redondeada de radio r.
static int inset_for(int dy, int h, int r) {
    if (r <= 0) return 0;
    if (r * 2 > h) r = h / 2;
    int d = dy < r ? dy : (dy >= h - r ? h - 1 - dy : r);
    if (d >= r) return 0;
    int dx2 = 4 * r * r - (2 * (r - d) - 1) * (2 * (r - d) - 1);
    if (dx2 < 0) dx2 = 0;
    return r - (isqrt(dx2) + 1) / 2;
}

void ui_round_grad(int x, int y, int w, int h, int r, uint16_t c0, uint16_t c1) {
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int yy = y + j - oy;
        if ((unsigned)yy >= (unsigned)ch) continue;
        int ins = inset_for(j, h, r);
        uint16_t *p = cvbuf + yy * cw;
        for (int i = ins; i < w - ins; i++) {
            int xx = x + i;
            if ((unsigned)xx >= (unsigned)cw) continue;
            p[xx] = (c0 == c1) ? c0 : ui_mix(c0, c1, w > 1 ? i * 255 / (w - 1) : 0);
        }
    }
}

void ui_round(int x, int y, int w, int h, int r, uint16_t c) {
    ui_round_grad(x, y, w, h, r, c, c);
}

void ui_vgrad(int x, int y, int w, int h, uint16_t c0, uint16_t c1) {
    for (int j = 0; j < h; j++)
        ui_rect(x, y + j, w, 1, ui_mix(c0, c1, h > 1 ? j * 255 / (h - 1) : 0));
}

// ---------------------------------------------------------------------
//  texto
// ---------------------------------------------------------------------
int ui_text_w(const char *s, int sc) { return (int)strlen(s) * FONT_W * sc; }

void ui_text(int x, int y, const char *s, uint16_t fg, int sc) {
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) c = '?';
        const uint8_t *g = FONT6X10[c - 32];
        for (int r = 0; r < FONT_H; r++)
            for (int b = 0; b < FONT_W; b++)
                if (g[r] & (1 << (FONT_W - 1 - b)))
                    ui_rect(x + (i * FONT_W + b) * sc, y + r * sc, sc, sc, fg);
    }
}

void ui_text_sh(int x, int y, const char *s, uint16_t fg, uint16_t sh, int sc) {
    ui_text(x + (sc > 1 ? 2 : 1), y + (sc > 1 ? 2 : 1), s, sh, sc);
    ui_text(x, y, s, fg, sc);
}

void ui_text_b(int x, int y, const char *s, uint16_t fg, int sc) {
    ui_text(x + 1, y, s, fg, sc);
    ui_text(x, y, s, fg, sc);
}

void ui_fit(char *dst, size_t n, const char *src, int max_px, int sc) {
    int maxc = max_px / (FONT_W * sc);
    if (maxc < 3) maxc = 3;
    if ((size_t)maxc > n - 1) maxc = (int)n - 1;
    size_t l = strlen(src);
    if ((int)l <= maxc) { memcpy(dst, src, l + 1); return; }
    memcpy(dst, src, (size_t)maxc - 2);
    dst[maxc - 2] = '.'; dst[maxc - 1] = '.'; dst[maxc] = 0;
}

void ui_fmt_time(char *out, size_t n, unsigned s) {
    if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else           snprintf(out, n, "%02u:%02u", s / 60, s % 60);
}

// ---------------------------------------------------------------------
//  iconos (16x16, dibujados con primitivas)
// ---------------------------------------------------------------------
static void icon_game(int x, int y, uint16_t fg, uint16_t hole) {
    ui_round(x, y + 3, 16, 10, 4, fg);
    ui_rect(x + 3, y + 8, 5, 1, hole);          // cruceta
    ui_rect(x + 5, y + 6, 1, 5, hole);
    ui_rect(x + 10, y + 6, 2, 2, hole);          // botones
    ui_rect(x + 12, y + 9, 2, 2, hole);
}

static void icon_video(int x, int y, uint16_t fg, uint16_t hole) {
    ui_round(x, y + 2, 16, 12, 3, fg);
    for (int i = 0; i < 4; i++) {                // triangulo "play"
        int h = 8 - 2 * i;
        ui_rect(x + 5 + i, y + 8 - h / 2, 1, h, hole);
    }
}

// ---------------------------------------------------------------------
//  estilo Windows XP: escritorio + ventana "Mi PC" con la lista
// ---------------------------------------------------------------------
#define XP_BLACK   RGB565(0, 0, 0)
#define XP_WHITE   RGB565(255, 255, 255)
#define XP_T0      RGB565(0, 66, 214)       // barra de titulo (izq)
#define XP_T1      RGB565(52, 128, 248)     // barra de titulo (der)
#define XP_FRAME   RGB565(0, 84, 227)
#define XP_FACE    RGB565(236, 233, 216)    // gris beige de XP
#define XP_PANE    RGB565(116, 150, 222)    // panel izquierdo azul
#define XP_PBODY   RGB565(214, 223, 247)
#define XP_LINK    RGB565(33, 93, 198)
#define XP_SEL     RGB565(49, 106, 197)
#define XP_GRAY    RGB565(90, 90, 90)

// Geometria de la ventana (ver ui.h): x 8..312, y 6..220
#define WX 8
#define WY 6
#define WW 304
#define WB 220

// Colores claros del tema anterior -> colores legibles sobre fondo claro.
static uint16_t dark_of(uint16_t c) {
    if (c == UI_TXT)  return XP_BLACK;
    if (c == UI_DIM)  return XP_GRAY;
    if (c == UI_OK)   return RGB565(0, 128, 0);
    if (c == UI_WARN) return RGB565(176, 110, 0);
    if (c == UI_ERR)  return RGB565(190, 0, 0);
    return c;
}

static void icon_folder(int x, int y) {
    ui_rect(x, y + 2, 7, 3, RGB565(222, 172, 52));          // pestana
    ui_round(x, y + 4, 16, 10, 2, RGB565(244, 206, 92));    // cuerpo
    ui_rect(x + 1, y + 6, 14, 1, RGB565(255, 236, 160));    // brillo
}

static void xp_button(int bx, int by, int kind) {            // 0 minimizar, 1 maximizar, 2 cerrar
    ui_round(bx, by, 15, 15, 3, XP_WHITE);
    ui_round(bx + 1, by + 1, 13, 13, 2, kind == 2 ? RGB565(214, 72, 40) : RGB565(36, 94, 220));
    if (kind == 0) ui_rect(bx + 4, by + 10, 6, 3, XP_WHITE);
    else if (kind == 1) {
        ui_rect(bx + 4, by + 4, 8, 2, XP_WHITE);
        ui_rect(bx + 4, by + 6, 1, 5, XP_WHITE);
        ui_rect(bx + 11, by + 6, 1, 5, XP_WHITE);
        ui_rect(bx + 4, by + 10, 8, 1, XP_WHITE);
    } else {
        for (int i = 0; i < 7; i++) {
            ui_rect(bx + 4 + i, by + 4 + i, 2, 1, XP_WHITE);
            ui_rect(bx + 10 - i, by + 4 + i, 2, 1, XP_WHITE);
        }
    }
}

static void xp_titlebar(int x, int y, int w, const char *title, bool folder) {
    ui_round_grad(x, y, w, 19, 5, XP_T0, XP_T1);
    ui_rect(x + 5, y + 1, w - 10, 1, RGB565(120, 170, 255));
    int tx = x + 8;
    if (folder) { icon_folder(x + 4, y + 1); tx = x + 24; }
    ui_text_sh(tx, y + 5, title, XP_WHITE, RGB565(0, 0, 90), 1);
    xp_button(x + w - 16 * 3 - 3, y + 2, 0);
    xp_button(x + w - 16 * 2 - 3, y + 2, 1);
    xp_button(x + w - 16 - 3, y + 2, 2);
}

static void panel_head(int y, const char *t) {
    ui_round_grad(15, y, 80, 14, 4, XP_WHITE, RGB565(198, 211, 247));
    ui_text_b(19, y + 2, t, XP_LINK, 1);
}

static void win_draw(int tab, int ng, int nv) {
    // marco azul + barra de titulo
    ui_rect(WX, WY + 19, WW, WB - WY - 19, XP_FRAME);
    xp_titlebar(WX, WY, WW, tab == 0 ? "Mis juegos" : "Mis videos", true);

    // barra de direcciones
    ui_rect(11, 25, 298, 16, XP_FACE);
    ui_text(15, 28, "Direccion", XP_GRAY, 1);
    ui_rect(72, 27, 214, 12, RGB565(127, 157, 185));
    ui_rect(73, 28, 212, 10, XP_WHITE);
    ui_text(76, 28, tab == 0 ? "Mi PC\\Mis juegos" : "Mi PC\\Mis videos", XP_BLACK, 1);
    ui_round_grad(289, 27, 18, 12, 2, RGB565(70, 180, 70), RGB565(40, 140, 40));
    ui_text_b(292, 28, "Ir", XP_WHITE, 1);

    // panel izquierdo
    ui_rect(11, 41, 88, 176, XP_PANE);
    panel_head(46, "Tareas");
    ui_rect(15, 60, 80, 38, XP_PBODY);
    ui_text(19, 63, "A: Abrir", XP_LINK, 1);
    ui_text(19, 75, "B: Cerrar", XP_LINK, 1);
    ui_text(19, 87, "SEL:Recargar", XP_LINK, 1);

    panel_head(102, "Ir a");
    ui_rect(15, 116, 80, 38, XP_PBODY);
    for (int t = 0; t < 2; t++) {
        char lab[20];
        snprintf(lab, sizeof lab, "%s (%d)", t == 0 ? "Juegos" : "Videos", t == 0 ? ng : nv);
        const int y = 119 + t * 12;
        if (t == tab) ui_rect(16, y - 1, 78, 12, XP_SEL);
        ui_text(19, y, lab, t == tab ? XP_WHITE : XP_LINK, 1);
    }
    ui_text(19, 143, "< > cambiar", XP_GRAY, 1);

    panel_head(158, "Detalles");
    ui_rect(15, 172, 80, 40, XP_PBODY);

    // panel derecho (blanco) con encabezado de grupo
    ui_rect(99, 41, 210, 176, XP_WHITE);
    ui_text_b(104, 45, tab == 0 ? "Juegos en la tarjeta SD" : "Videos en la tarjeta SD", XP_LINK, 1);
    ui_rect(103, 58, 200, 1, RGB565(170, 190, 235));
}

void ui_window(int tab, int n_games, int n_videos) {
    for (int y0 = WY; y0 < WB; y0 += CV_MAXH) {
        const int h = (WB - y0 < CV_MAXH) ? WB - y0 : CV_MAXH;
        ui_begin(UI_W, h, UI_WALL);
        oy = y0;
        win_draw(tab, n_games, n_videos);
        ui_end(0, y0);
    }
}

void ui_row(int pos, const ui_item_t *it, bool sel) {
    ui_begin(200, UI_ROW_H, XP_WHITE);
    if (it) {
        const bool vid = it->kind == UI_KIND_VIDEO;
        if (sel) ui_rect(0, 0, 200, UI_ROW_H, XP_SEL);
        if (vid) icon_video(3, 1, sel ? XP_WHITE : RGB565(60, 120, 225), sel ? XP_SEL : XP_WHITE);
        else     icon_folder(3, 1);
        int right_w = 0;
        if (it->badge && it->badge[0]) {
            const int w = ui_text_w(it->badge, 1);
            ui_text_b(196 - w, 4, it->badge, sel ? XP_WHITE : dark_of(it->badge_color), 1);
            right_w = w + 8;
        }
        char nom[40];
        ui_fit(nom, sizeof nom, it->name, 200 - 24 - 4 - right_w, 1);
        ui_text(24, 4, nom, sel ? XP_WHITE : XP_BLACK, 1);
    }
    ui_end(100, UI_LIST_Y + pos * UI_ROW_H);
}

void ui_scrollbar(int top, int visible, int total) {
    const int th = UI_ROWS * UI_ROW_H;
    ui_begin(8, th, XP_WHITE);
    if (total > visible) {
        ui_rect(0, 0, 8, th, RGB565(240, 240, 238));
        int hh = th * visible / total;
        if (hh < 16) hh = 16;
        const int yy = (th - hh) * top / (total - visible);
        ui_round(0, yy, 8, hh, 2, RGB565(160, 185, 240));
        ui_round(1, yy + 1, 6, hh - 2, 1, RGB565(196, 213, 250));
    }
    ui_end(301, UI_LIST_Y);
}

void ui_empty(const char *l1, const char *l2) {
    ui_begin(200, 32, XP_WHITE);
    if (l1) ui_text((200 - ui_text_w(l1, 1)) / 2, 4, l1, XP_GRAY, 1);
    if (l2) ui_text((200 - ui_text_w(l2, 1)) / 2, 20, l2, XP_GRAY, 1);
    ui_end(100, UI_LIST_Y + 40);
}

// Panel "Detalles" (abajo a la izquierda): nombre y dos lineas.
void ui_details(const char *name, const char *l2, const char *l3) {
    ui_begin(80, 32, XP_PBODY);    // el canvas mide como maximo 32 filas
    if (name) {
        char b[16];
        ui_fit(b, sizeof b, name, 74, 1);
        ui_text_b(4, 1, b, XP_BLACK, 1);
    }
    if (l2) ui_text(4, 12, l2, XP_GRAY, 1);
    if (l3) ui_text(4, 22, l3, XP_GRAY, 1);
    ui_end(15, 172);
}

// Escritorio: solo el fondo de Windows XP (con su barra de tareas) y un
// globito que invita a pulsar START.
void ui_desktop(void) {
    ui_wallpaper();
    const char *t = "Pulsa START para comenzar";
    const int w = ui_text_w(t, 1) + 12;
    ui_begin(w, 22, UI_WALL);
    ui_round(0, 0, w, 18, 4, XP_BLACK);
    ui_round(1, 1, w - 2, 16, 3, RGB565(255, 255, 225));
    ui_text(6, 4, t, XP_BLACK, 1);
    for (int i = 0; i < 4; i++) {                    // piquito hacia el boton start
        ui_rect(10 + i, 18 + i, 7 - 2 * i, 1, XP_BLACK);
        if (7 - 2 * i > 2) ui_rect(11 + i, 18 + i, 5 - 2 * i, 1, RGB565(255, 255, 225));
    }
    ui_end(3, 202);
}

// ---------------------------------------------------------------------
//  pantallas completas (cuadros de dialogo estilo XP)
// ---------------------------------------------------------------------
#define DX 30
#define DW 260
#define DY 30
#define DB 222
#define DIN_X (DX + 3)           // zona interior (beige)
#define DIN_W (DW - 6)

static void dlg_draw(void) {
    ui_rect(DX, DY + 19, DW, DB - DY - 19, XP_FRAME);
    xp_titlebar(DX, DY, DW, "Pico Launcher", false);
    ui_rect(DIN_X, DY + 19, DIN_W, DB - DY - 22, XP_FACE);
}

static void title_c(int y, const char *s, uint16_t color, int sc) {
    const int tw = ui_text_w(s, sc);
    const int th = FONT_H * sc;
    const uint16_t c = dark_of(color);
    ui_begin(DIN_W, th + 8, XP_FACE);
    ui_text_b((DIN_W - tw) / 2, 0, s, c, sc);
    ui_rect((DIN_W - tw) / 2 - 6, th + 3, tw + 12, 2, c);
    ui_end(DIN_X, y);
}

void ui_line_c(int y, const char *s, uint16_t fg, int sc) {
    ui_begin(DIN_W, FONT_H * sc + 2, XP_FACE);
    int x = (DIN_W - ui_text_w(s, sc)) / 2;
    ui_text(x < 0 ? 0 : x, 1, s, dark_of(fg), sc);
    ui_end(DIN_X, y);
}

// Barra de progreso estilo XP (bloques verdes). "color" ya no se usa.
void ui_bar(int y, uint32_t cur, uint32_t tot, uint16_t color) {
    (void)color;
    const int x = 10, w = DIN_W - 20, h = 14;
    ui_begin(DIN_W, 18, XP_FACE);
    ui_rect(x, 2, w, h, RGB565(127, 157, 185));
    ui_rect(x + 1, 3, w - 2, h - 2, XP_WHITE);
    if (tot) {
        const int wf = (int)((uint64_t)(w - 6) * cur / tot);
        for (int k = 0; k * 9 + 7 <= wf; k++)
            ui_vgrad(x + 3 + k * 9, 5, 7, h - 6, RGB565(120, 230, 100), RGB565(40, 170, 40));
    }
    ui_end(DIN_X, y - 2);
}

// Pantalla de carga: estilo Windows XP (ver bootxp.c). msg/pct ya no se muestran;
// la barra azul se anima sola con xp_boot_tick() mientras se lee la SD.
void ui_splash(const char *msg, int pct) {
    (void)msg; (void)pct;
    xp_boot_start();
}

void ui_message(const char *titulo, uint16_t color, const char *l1,
                const char *l2, const char *l3, const char *l4) {
    ui_wallpaper();
    for (int y0 = DY; y0 < DB; y0 += CV_MAXH) {
        const int h = (DB - y0 < CV_MAXH) ? DB - y0 : CV_MAXH;
        ui_begin(UI_W, h, UI_WALL);
        oy = y0;
        dlg_draw();
        ui_end(0, y0);
    }
    title_c(56, titulo, color, 2);
    const char *ls[4] = { l1, l2, l3, l4 };
    for (int i = 0; i < 4; i++)
        if (ls[i]) ui_line_c(86 + i * 16, ls[i], UI_TXT, 1);
}

// ---------------------------------------------------------------------
//  reproductor
// ---------------------------------------------------------------------
void ui_hud_top(int y, int h, const char *title) {
    if (h > CV_MAXH) h = CV_MAXH;
    ui_begin(UI_W, h, RGB565(0, 0, 0));
    char buf[48];
    ui_fit(buf, sizeof buf, title, 280, 1);
    const int ty = (h - FONT_H) / 2;
    icon_video(8, (h - 16) / 2, UI_VID, RGB565(0, 0, 0));
    ui_text_b(30, ty, buf, UI_TXT, 1);
    ui_end(0, y);
}

void ui_hud_bottom(int y, int h, unsigned cur_s, unsigned tot_s, bool paused) {
    if (h > CV_MAXH) h = CV_MAXH;
    ui_begin(UI_W, h, RGB565(0, 0, 0));
    const int cy = h / 2;
    // icono play / pausa
    if (paused) {
        ui_rect(10, cy - 5, 3, 10, UI_TXT);
        ui_rect(16, cy - 5, 3, 10, UI_TXT);
    } else {
        for (int i = 0; i < 5; i++) {
            int hh = 10 - 2 * i;
            ui_rect(10 + i * 2, cy - hh / 2, 2, hh, UI_TXT);
        }
    }
    char a[16], b[16];
    ui_fmt_time(a, sizeof a, cur_s);
    ui_fmt_time(b, sizeof b, tot_s);
    ui_text(30, cy - 5, a, UI_TXT, 1);
    const int tw = ui_text_w(b, 1);
    ui_text(UI_W - 8 - tw, cy - 5, b, UI_DIM, 1);
    const int bx = 30 + ui_text_w(a, 1) + 10;
    const int bw = UI_W - 8 - tw - 10 - bx;
    if (bw > 20) {
        ui_round(bx, cy - 3, bw, 6, 3, RGB565(44, 48, 72));
        int wf = tot_s ? (int)((uint64_t)bw * (cur_s > tot_s ? tot_s : cur_s) / tot_s) : 0;
        if (wf >= 6) ui_round_grad(bx, cy - 3, wf, 6, 3, UI_VID, RGB565(240, 64, 140));
    }
    ui_end(0, y);
}

void ui_overlay(const char *txt, uint16_t color) {
    const int w = ui_text_w(txt, 2) + 36, h = 36;
    ui_begin(w, h, UI_BG);
    ui_round(0, 0, w, h, 9, color);
    ui_round(2, 2, w - 4, h - 4, 7, UI_BG);
    ui_text_sh(18, 8, txt, UI_TXT, RGB565(30, 30, 80), 2);
    ui_end((UI_W - w) / 2, (UI_H - h) / 2);
}

void ui_vol_badge(int vol, int vmax) {
    if (vmax < 1) vmax = 1;
    if (vmax > 12) vmax = 12;
    const int w = 26 + vmax * 5 + 5, h = 20;
    ui_begin(w, h, UI_BG);
    ui_round(0, 0, w, h, 6, UI_VID);
    ui_round(1, 1, w - 2, h - 2, 5, UI_BG);
    // altavoz
    ui_rect(7, 7, 4, 6, UI_TXT);
    for (int i = 0; i < 4; i++) ui_rect(11 + i, 7 - i, 1, 6 + 2 * i, UI_TXT);
    // barritas crecientes
    for (int i = 0; i < vmax; i++) {
        const int bh = 3 + (i * 7) / (vmax > 1 ? vmax - 1 : 1);
        ui_rect(26 + i * 5, 15 - bh, 3, bh, i < vol ? UI_VID : RGB565(60, 64, 90));
    }
    ui_end(UI_W - w - 6, 6);
}
