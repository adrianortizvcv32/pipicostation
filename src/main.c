// =====================================================================
//  PICO LAUNCHER  -  menu de juegos y videos desde la SD para Raspberry Pi
//  Pico (RP2040) + TFT ST7789 + botones.
//
//  Pestana JUEGOS: archivos .uf2 (se instalan en la flash y se arrancan).
//  Pestana VIDEOS: archivos .pvd (se reproducen directo desde la SD; ver
//  video.h y herramientas/convertir_video.py).
//
//  Mapa de la flash (2 MB):
//    0x10000000 - 0x1003EFFF  este launcher (con su boot2)
//    0x1003F000 - 0x1003FFFF  sector de configuracion (juego instalado)
//    0x10040000 - 0x101FFFFF  AREA DE JUEGOS (1.75 MB)
//
//  Los juegos son archivos .uf2 en la SD (carpeta /games o raiz) que
//  estan ENLAZADOS en 0x10040000 y SIN boot2 (ver memmap_app_0x40000.ld).
//  El launcher nunca escribe por debajo de 0x10040000, asi que no se
//  puede "brickear" a si mismo con un UF2 equivocado: lo rechaza.
//
//  Para volver al launcher desde un juego basta reiniciar la Pico
//  (por ejemplo watchdog_reboot(0,0,10), o el boton RUN/reset).
// =====================================================================
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/m0plus.h"

#include "ff.h"
#include "st7789.h"
#include "ui.h"
#include "input.h"
#include "video.h"
#include "audio.h"
#include "bootxp.h"

// ----------------------------- mapa de flash --------------------------
#define APP_OFFSET      0x40000u                       // inicio de juegos (desde XIP_BASE)
#define APP_ADDR        (XIP_BASE + APP_OFFSET)
#define FLASH_TOTAL     (PICO_FLASH_SIZE_BYTES)
#define APP_MAX_BYTES   (FLASH_TOTAL - APP_OFFSET)
#define CFG_OFFSET      (APP_OFFSET - FLASH_SECTOR_SIZE) // 0x3F000
#define CFG_MAGIC       0x48434E4Cu                    // 'LNCH'
#define BOOT_MAGIC      0xB007CAFEu                    // en watchdog scratch[0]

// ----------------------------- botones --------------------------------
// (pines en input.h)
static const uint8_t PINES[] = { PIN_UP, PIN_DOWN, PIN_LEFT, PIN_RIGHT,
                                 PIN_SELECT, PIN_START, PIN_A, PIN_B };

// ----------------------------- compatibilidad con el codigo de instalacion
#define C_BG    UI_BG
#define C_TXT   UI_TXT
#define C_DIM   UI_DIM
#define C_OK    UI_OK
#define C_ERR   UI_ERR
#define C_WARN  UI_WARN
static inline void draw_text_c(int y, const char *s, uint16_t fg, uint16_t bg, int sc) {
    (void)bg; ui_line_c(y, s, fg, sc);
}
static inline void pantalla_mensaje(const char *t, uint16_t c, const char *l1,
                                    const char *l2, const char *l3, const char *l4) {
    ui_message(t, c, l1, l2, l3, l4);
}
static inline void barra(int y, uint32_t cur, uint32_t tot, uint16_t color) { ui_bar(y, cur, tot, color); }

// ----------------------------- botones --------------------------------
static void botones_init(void) {
    for (unsigned i = 0; i < sizeof PINES; i++) {
        gpio_init(PINES[i]);
        gpio_set_dir(PINES[i], GPIO_IN);
        gpio_pull_up(PINES[i]);
    }
}
static inline bool btn(unsigned pin) { return !gpio_get(pin); }

// Espera un boton (con antirrebote y autorepeticion en arriba/abajo).
// timeout_ms = 0 -> espera indefinida. Devuelve el pin, o -1 si vence.
static int esperar_boton(uint32_t timeout_ms) {
    static int last = -1;
    static uint32_t t_press = 0, t_rep = 0;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    for (;;) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        int cur = -1;
        for (unsigned i = 0; i < sizeof PINES; i++) if (btn(PINES[i])) { cur = PINES[i]; break; }
        if (cur != last) {
            last = cur;
            if (cur >= 0) {
                sleep_ms(15);                       // antirrebote
                if (!btn((unsigned)cur)) { last = -1; continue; }
                t_press = now; t_rep = now;
                return cur;
            }
        } else if (cur == PIN_UP || cur == PIN_DOWN) {
            const uint32_t gap = (now - t_press > 1500) ? 40 : 90;      // acelera si se mantiene
            if (now - t_press > 350 && now - t_rep > gap) { t_rep = now; return cur; }
        }
        if (timeout_ms && now - t0 >= timeout_ms) return -1;
        sleep_ms(5);
    }
}
static void esperar_soltar_todo(void) {
    for (;;) {
        bool any = false;
        for (unsigned i = 0; i < sizeof PINES; i++) if (btn(PINES[i])) any = true;
        if (!any) break;
        sleep_ms(10);
    }
    sleep_ms(20);
}

// ----------------------------- CRC / config ---------------------------
static uint32_t crc32(const void *data, size_t n) {
    const uint8_t *p = data; uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}

typedef struct {
    uint32_t magic;
    uint32_t size;       // tamano del .uf2 en SD
    uint16_t fdate;      // fecha/hora de modificacion del .uf2 (identifica la version)
    uint16_t ftime;
    uint32_t app_end;    // fin de la imagen en flash (informativo)
    char     name[64];
    uint32_t crc;        // crc32 de todo lo anterior
} cfg_t;

static const cfg_t *cfg_flash(void) { return (const cfg_t *)(XIP_BASE + CFG_OFFSET); }

static bool cfg_valida(const cfg_t *c) {
    return c->magic == CFG_MAGIC && c->crc == crc32(c, offsetof(cfg_t, crc));
}

static bool app_valida(void) {
    const uint32_t *vt = (const uint32_t *)APP_ADDR;
    uint32_t sp = vt[0], rst = vt[1];
    return sp >= 0x20000000u && sp <= 0x20042000u &&
           (rst & 1u) && rst >= APP_ADDR && rst < XIP_BASE + FLASH_TOTAL;
}

// ----------------------------- arranque de la app ---------------------
static void __attribute__((noreturn)) saltar_a_app(void) {
    const uint32_t *vt = (const uint32_t *)APP_ADDR;
    *(volatile uint32_t *)(PPB_BASE + M0PLUS_NVIC_ICER_OFFSET) = 0xFFFFFFFFu;
    *(volatile uint32_t *)(PPB_BASE + M0PLUS_NVIC_ICPR_OFFSET) = 0xFFFFFFFFu;
    *(volatile uint32_t *)(PPB_BASE + M0PLUS_VTOR_OFFSET) = APP_ADDR;
    __asm volatile("msr msp, %0\n bx %1\n" :: "r"(vt[0]), "r"(vt[1]));
    __builtin_unreachable();
}

// Reinicia el chip (estado limpio) y, al volver a arrancar, main() salta a
// la app antes de tocar nada.
static void __attribute__((noreturn)) iniciar_app(void) {
    watchdog_hw->scratch[0] = BOOT_MAGIC;
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

// ----------------------------- listas (juegos y videos) ---------------
#define MAX_JUEGOS 40
#define MAX_VIDEOS 40
typedef struct {
    char     path[96];
    char     name[44];     // nombre visible (sin extension)
    uint32_t size;
    uint16_t fdate, ftime;
    // solo videos:
    bool     bad;          // el .pvd no es valido / no soportado
    uint16_t vw, vh;
    uint16_t fps10;        // fps * 10
    uint32_t dur_s;
} juego_t;

static juego_t juegos[MAX_JUEGOS];
static juego_t videos[MAX_VIDEOS];
static int n_juegos = 0, n_videos = 0;
static FATFS fs;
static FILINFO fno;
static DIR dir;
static FIL fil;

static bool tiene_ext(const char *n, const char *ext) {
    size_t l = strlen(n), e = strlen(ext);
    return l > e && strcasecmp(n + l - e, ext) == 0;
}

static void escanear_dir(const char *carpeta, const char *ext, juego_t *arr, int *cnt, int max) {
    if (f_opendir(&dir, carpeta) != FR_OK) return;
    while (*cnt < max && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
        if (fno.fname[0] == '.' || fno.fname[0] == '_') continue;   // basura de macOS (._x)
        if (!tiene_ext(fno.fname, ext)) continue;
        xp_boot_tick();                                   // anima la barra de carga
        juego_t *j = &arr[*cnt];
        memset(j, 0, sizeof *j);
        const char *sep = (strcmp(carpeta, "0:/") == 0) ? "" : "/";
        if (snprintf(j->path, sizeof j->path, "%s%s%s", carpeta, sep, fno.fname) >= (int)sizeof j->path) continue;
        size_t l = strlen(fno.fname) - strlen(ext);
        if (l >= sizeof j->name) l = sizeof j->name - 1;
        memcpy(j->name, fno.fname, l); j->name[l] = 0;
        j->size = (uint32_t)fno.fsize; j->fdate = fno.fdate; j->ftime = fno.ftime;
        (*cnt)++;
    }
    f_closedir(&dir);
}

static int cmp_juegos(const void *a, const void *b) {
    return strcasecmp(((const juego_t *)a)->name, ((const juego_t *)b)->name);
}

// Lee el encabezado de cada .pvd (duracion, tamano, fps).
static void info_videos(void) {
    for (int i = 0; i < n_videos; i++) {
        xp_boot_tick();
        pvd_info_t pi;
        if (pvd_info(videos[i].path, &pi) == PVD_OK) {
            videos[i].vw = pi.w; videos[i].vh = pi.h;
            videos[i].fps10 = (uint16_t)((uint32_t)pi.fps_num * 10 / pi.fps_den);
            videos[i].dur_s = pi.dur_s;
        } else {
            videos[i].bad = true;
        }
    }
}

// 0 = ok, <0 = no hay SD
static int cargar_lista(void) {
    n_juegos = 0; n_videos = 0;
    f_mount(NULL, "0:", 0);
    if (f_mount(&fs, "0:", 1) != FR_OK) return -1;
    xp_boot_tick();
    escanear_dir("0:/games", ".uf2", juegos, &n_juegos, MAX_JUEGOS);
    escanear_dir("0:/", ".uf2", juegos, &n_juegos, MAX_JUEGOS);
    escanear_dir("0:/videos", ".pvd", videos, &n_videos, MAX_VIDEOS);
    escanear_dir("0:/", ".pvd", videos, &n_videos, MAX_VIDEOS);
    qsort(juegos, (size_t)n_juegos, sizeof juegos[0], cmp_juegos);
    qsort(videos, (size_t)n_videos, sizeof videos[0], cmp_juegos);
    info_videos();
    return 0;
}

// ----------------------------- instalacion de UF2 ---------------------
#define UF2_M0 0x0A324655u
#define UF2_M1 0x9E5D5157u
#define UF2_ME 0x0AB16F30u
#define UF2_FLAG_NOT_MAIN_FLASH 0x00000001u
#define UF2_FLAG_FAMILY         0x00002000u
#define UF2_FAMILY_RP2040       0xE48BFF56u

typedef struct {
    uint32_t m0, m1, flags, addr, len, blk, nblk, fam;
    uint8_t  data[476];
    uint32_t me;
} uf2_t;

enum { E_OK = 0, E_SD, E_FORMATO, E_DIR_BAJA, E_DIR_ALTA, E_ORDEN, E_FAMILIA,
       E_VACIO, E_SIN_VECTORES, E_ESCRIBIR, E_VERIFICAR };

static char err_extra[40];
static uint32_t scan_min, scan_max, scan_blocks;

static const char *msg_error(int e) {
    switch (e) {
    case E_SD:           return "Error leyendo la SD";
    case E_FORMATO:      return "El archivo no es un UF2 valido";
    case E_DIR_BAJA:     return "UF2 enlazado en 0x10000000";
    case E_DIR_ALTA:     return "El juego no cabe en la flash";
    case E_ORDEN:        return "UF2 con bloques desordenados";
    case E_FAMILIA:      return "UF2 no es para RP2040";
    case E_VACIO:        return "UF2 sin datos de flash";
    case E_SIN_VECTORES: return "UF2 no empieza en 0x10040000";
    case E_ESCRIBIR:     return "Fallo al escribir la flash";
    case E_VERIFICAR:    return "La flash no coincide (verif.)";
    default:             return "Error desconocido";
    }
}

// Pasada 1: valida TODO el archivo antes de tocar la flash.
static int uf2_validar(void) {
    UINT br; uf2_t b;
    uint32_t prev = 0; bool primero = true;
    scan_min = 0xFFFFFFFFu; scan_max = 0; scan_blocks = 0;
    err_extra[0] = 0;
    f_lseek(&fil, 0);
    for (;;) {
        if (f_read(&fil, &b, sizeof b, &br) != FR_OK) return E_SD;
        if (br == 0) break;
        if (br != sizeof b) return E_FORMATO;
        if (b.m0 != UF2_M0 || b.m1 != UF2_M1 || b.me != UF2_ME) return E_FORMATO;
        if (b.flags & UF2_FLAG_NOT_MAIN_FLASH) continue;
        if ((b.flags & UF2_FLAG_FAMILY) && b.fam != UF2_FAMILY_RP2040) return E_FAMILIA;
        if (b.len != 256 || (b.addr & 0xFF)) return E_FORMATO;
        if (b.addr < XIP_BASE || b.addr >= XIP_BASE + FLASH_TOTAL) return E_FORMATO;
        if (b.addr < APP_ADDR) { snprintf(err_extra, sizeof err_extra, "bloque en 0x%08lX", (unsigned long)b.addr); return E_DIR_BAJA; }
        if (b.addr + 256 > XIP_BASE + FLASH_TOTAL) return E_DIR_ALTA;
        if (!primero && b.addr <= prev) return E_ORDEN;
        primero = false; prev = b.addr;
        if (b.addr < scan_min) scan_min = b.addr;
        if (b.addr + 256 > scan_max) scan_max = b.addr + 256;
        scan_blocks++;
    }
    if (!scan_blocks) return E_VACIO;
    if (scan_min != APP_ADDR) return E_SIN_VECTORES;
    return E_OK;
}

static uint8_t sbuf[FLASH_SECTOR_SIZE];

static int flush_sector(uint32_t sec_addr) {
    const uint32_t off = sec_addr - XIP_BASE;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(off, sbuf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    return memcmp((const void *)sec_addr, sbuf, FLASH_SECTOR_SIZE) ? E_VERIFICAR : E_OK;
}

static void cfg_escribir(const cfg_t *c) {
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    memcpy(page, c, sizeof *c);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CFG_OFFSET, FLASH_SECTOR_SIZE);
    if (c->magic == CFG_MAGIC) flash_range_program(CFG_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

static int instalar(const juego_t *j) {
    int e = E_OK;
    if (f_open(&fil, j->path, FA_READ) != FR_OK) return E_SD;

    pantalla_mensaje("COMPROBANDO", C_WARN, j->name, "Validando el UF2...", NULL, NULL);
    e = uf2_validar();
    if (e != E_OK) { f_close(&fil); return e; }

    // Todo bien: a partir de aqui se modifica la flash. Primero se invalida
    // la config, asi un corte de luz a medias no deja un "juego instalado" roto.
    pantalla_mensaje("INSTALANDO", C_WARN, j->name, "No apagues la consola", NULL, NULL);
    cfg_escribir(&(cfg_t){ .magic = 0 });

    // --- borrar [scan_min, scan_max) en bloques de 64 KB (con progreso) ---
    const uint32_t ini = scan_min - XIP_BASE;
    const uint32_t fin = (scan_max - XIP_BASE + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
    draw_text_c(120, "Borrando...", C_TXT, C_BG, 1);
    for (uint32_t o = ini; o < fin; ) {
        uint32_t n = fin - o; if (n > 65536) n = 65536;
        barra(140, o - ini, fin - ini, C_WARN);
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(o, n);
        restore_interrupts(ints);
        o += n;
    }
    barra(140, 1, 1, C_WARN);

    // --- escribir ---
    draw_text_c(120, "Escribiendo...", C_TXT, C_BG, 1);
    f_lseek(&fil, 0);
    uf2_t b; UINT br;
    uint32_t cur = 0xFFFFFFFFu, hechos = 0;
    memset(sbuf, 0xFF, sizeof sbuf);
    for (;;) {
        if (f_read(&fil, &b, sizeof b, &br) != FR_OK || br != sizeof b) {
            if (br == 0) break;
            e = E_SD; break;
        }
        if (b.flags & UF2_FLAG_NOT_MAIN_FLASH) continue;
        uint32_t sec = b.addr & ~(FLASH_SECTOR_SIZE - 1);
        if (sec != cur) {
            if (cur != 0xFFFFFFFFu && (e = flush_sector(cur)) != E_OK) break;
            memset(sbuf, 0xFF, sizeof sbuf);
            cur = sec;
        }
        memcpy(sbuf + (b.addr - sec), b.data, 256);
        if ((++hechos & 31) == 0) barra(168, hechos, scan_blocks, C_OK);
    }
    if (e == E_OK && cur != 0xFFFFFFFFu) e = flush_sector(cur);
    f_close(&fil);
    if (e != E_OK) return e;
    barra(168, 1, 1, C_OK);

    if (!app_valida()) return E_SIN_VECTORES;

    // --- confirmar instalacion ---
    cfg_t c = { .magic = CFG_MAGIC, .size = j->size, .fdate = j->fdate, .ftime = j->ftime,
                .app_end = scan_max };
    strncpy(c.name, j->path, sizeof c.name - 1);
    c.crc = crc32(&c, offsetof(cfg_t, crc));
    cfg_escribir(&c);
    return E_OK;
}

static bool ya_instalado(const juego_t *j) {
    const cfg_t *c = cfg_flash();
    if (!cfg_valida(c) || !app_valida()) return false;
    char tmp[64]; memset(tmp, 0, sizeof tmp);
    strncpy(tmp, j->path, sizeof tmp - 1);
    return c->size == j->size && c->fdate == j->fdate && c->ftime == j->ftime &&
           memcmp(c->name, tmp, sizeof tmp) == 0;
}

static void lanzar(const juego_t *j) {
    if (!ya_instalado(j)) {
        int e = instalar(j);
        if (e != E_OK) {
            pantalla_mensaje("ERROR", C_ERR, msg_error(e), err_extra[0] ? err_extra : NULL,
                             (e == E_DIR_BAJA || e == E_SIN_VECTORES)
                                 ? "Recompila el juego con el linker" : NULL,
                             (e == E_DIR_BAJA || e == E_SIN_VECTORES)
                                 ? "memmap_app_0x40000.ld" : NULL);
            draw_text_c(205, "Pulsa un boton", C_DIM, C_BG, 1);
            esperar_soltar_todo();
            esperar_boton(0);
            return;
        }
    }
    pantalla_mensaje("INICIANDO", C_OK, j->name, NULL, NULL, NULL);
    sleep_ms(250);
    iniciar_app();
}

// ----------------------------- menu -----------------------------------
static juego_t *lista_de(int tab) { return tab == 0 ? juegos : videos; }
static int      cuenta_de(int tab) { return tab == 0 ? n_juegos : n_videos; }

static void fila_item(int tab, int idx, ui_item_t *it, char *badge, size_t bn) {
    juego_t *j = &lista_de(tab)[idx];
    it->name = j->name;
    it->kind = tab;
    it->badge = NULL;
    it->badge_color = UI_OK;
    if (tab == 0) {
        if (ya_instalado(j)) { it->badge = "LISTO"; }
    } else if (j->bad) {
        it->badge = "NO VALIDO"; it->badge_color = UI_WARN;
    } else {
        ui_fmt_time(badge, bn, j->dur_s);
        it->badge = badge; it->badge_color = UI_TXT;
    }
}

static void dibujar_fila(int tab, int idx, int pos, bool sel) {
    if (idx < 0 || idx >= cuenta_de(tab)) { ui_row(pos, NULL, false); return; }
    ui_item_t it; char badge[16];
    fila_item(tab, idx, &it, badge, sizeof badge);
    ui_row(pos, &it, sel);
}

static void dibujar_lista(int tab, int sel, int top) {
    const int n = cuenta_de(tab);
    for (int p = 0; p < UI_ROWS; p++) dibujar_fila(tab, top + p, p, top + p == sel);
    ui_scrollbar(top, UI_ROWS, n);
    if (n == 0) {
        ui_empty(tab == 0 ? "No hay juegos (.uf2)" : "No hay videos (.pvd)",
                 tab == 0 ? "Copialos en la carpeta /games" : "Copialos en la carpeta /videos");
    }
}

static void dibujar_info(int tab, int sel) {
    const int n = cuenta_de(tab);
    if (sel < 0 || sel >= n) { ui_details(NULL, NULL, NULL); return; }
    const juego_t *j = &lista_de(tab)[sel];
    char l2[16], l3[16];
    snprintf(l2, sizeof l2, "%d de %d", sel + 1, n);
    if (tab == 0) {
        if (ya_instalado(j)) snprintf(l3, sizeof l3, "Instalado");
        else snprintf(l3, sizeof l3, "%lu KB", (unsigned long)(j->size / 1024));
    } else if (j->bad) {
        snprintf(l3, sizeof l3, "No valido");
    } else {
        ui_fmt_time(l3, sizeof l3, j->dur_s);
    }
    ui_details(j->name, l2, l3);
}

// fondo = true: repinta antes el fondo de XP (hace falta si la pantalla tenia otra cosa).
static void dibujar_todo(int tab, int sel, int top, bool fondo) {
    if (fondo) ui_wallpaper();
    ui_window(tab, n_juegos, n_videos);
    dibujar_lista(tab, sel, top);
    dibujar_info(tab, sel);
}

static void reproducir(const juego_t *j) {
    esperar_soltar_todo();
    pvd_play(j->path, j->name);
    esperar_soltar_todo();
}

static void menu(void) {
    int tab = 0;
    int sel[2] = { 0, 0 }, top[2] = { 0, 0 };
    bool primera = true;                                  // solo al encender: dejar ver la animacion
    bool abierta = false;                                 // false = escritorio, true = ventana "Mi PC"
    for (;;) {
        ui_splash("Leyendo la SD...", 35);               // pantalla de carga estilo Windows XP
        int r = cargar_lista();
        if (primera) { xp_boot_wait(XP_BOOT_MS); primera = false; }
        if (r < 0) {
            pantalla_mensaje("SIN SD", C_ERR, "No se pudo leer la tarjeta SD.",
                             "Revisa que este bien insertada", "y formateada en FAT32.", NULL);
            draw_text_c(205, "Pulsa un boton para reintentar", C_DIM, C_BG, 1);
            esperar_soltar_todo(); esperar_boton(0); continue;
        }
        if (n_juegos == 0 && n_videos == 0) {
            pantalla_mensaje("SD VACIA", C_WARN, "No hay juegos (.uf2) ni videos (.pvd).",
                             "Juegos: carpeta /games", "Videos: carpeta /videos", NULL);
            draw_text_c(205, "Pulsa un boton para recargar", C_DIM, C_BG, 1);
            esperar_soltar_todo(); esperar_boton(0); continue;
        }
        if (cuenta_de(tab) == 0) tab ^= 1;               // empezar en una pestana con contenido
        for (int t = 0; t < 2; t++) {
            const int n = cuenta_de(t);
            if (sel[t] >= n) sel[t] = n > 0 ? n - 1 : 0;
            if (top[t] > sel[t]) top[t] = sel[t];
            if (sel[t] >= top[t] + UI_ROWS) top[t] = sel[t] - UI_ROWS + 1;
        }
        if (abierta) dibujar_todo(tab, sel[tab], top[tab], true);
        else         ui_desktop();
        esperar_soltar_todo();

        bool recargar = false;
        while (!recargar) {
            const int b = esperar_boton(0);
            const int n = cuenta_de(tab);
            int nsel = sel[tab];

            if (!abierta) {                               // escritorio: START abre la ventana
                if (b == PIN_START || b == PIN_A) { abierta = true; dibujar_todo(tab, sel[tab], top[tab], false); }
                else if (b == PIN_SELECT) recargar = true;
                continue;
            }
            if (b == PIN_B) { abierta = false; ui_desktop(); continue; }   // cerrar la ventana

            if (b == PIN_LEFT || b == PIN_RIGHT) {        // cambiar entre juegos y videos
                tab ^= 1;
                dibujar_todo(tab, sel[tab], top[tab], false);
                continue;
            } else if (b == PIN_SELECT) { recargar = true; continue; }
            else if (b == PIN_A || b == PIN_START) {
                if (n == 0) continue;
                if (tab == 0) {
                    lanzar(&juegos[sel[0]]);              // solo vuelve si hubo error
                    recargar = true;
                } else {
                    reproducir(&videos[sel[1]]);
                    dibujar_todo(tab, sel[tab], top[tab], true);
                }
                continue;
            }
            else if (n == 0) continue;
            else if (b == PIN_UP)   nsel = (sel[tab] + n - 1) % n;
            else if (b == PIN_DOWN) nsel = (sel[tab] + 1) % n;
            else continue;

            if (nsel == sel[tab]) continue;
            const int old = sel[tab], old_top = top[tab];
            sel[tab] = nsel;
            if (sel[tab] < top[tab]) top[tab] = sel[tab];
            if (sel[tab] >= top[tab] + UI_ROWS) top[tab] = sel[tab] - UI_ROWS + 1;
            if (top[tab] != old_top) {
                dibujar_lista(tab, sel[tab], top[tab]);           // la lista se desplazo
            } else {                                              // solo cambian 2 filas
                dibujar_fila(tab, old, old - top[tab], false);
                dibujar_fila(tab, sel[tab], sel[tab] - top[tab], true);
            }
            dibujar_info(tab, sel[tab]);
        }
    }
}

// ----------------------------- main -----------------------------------
int main(void) {
    // Si veniamos de "iniciar_app()", saltar al juego ANTES de inicializar nada.
    if (watchdog_hw->scratch[0] == BOOT_MAGIC) {
        watchdog_hw->scratch[0] = 0;
        if (app_valida()) saltar_a_app();
    }

    botones_init();
    audio_init_pin();                // GP7 en bajo: el transistor del parlante no debe quedar flotando
    st7789_init();
    menu();                          // lo primero que hace es pintar la pantalla de carga XP
    return 0;
}
