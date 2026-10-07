// =====================================================================
//  video.c - reproductor .pvd (ver video.h)
//
//  Mientras el DMA manda el cuadro N a la pantalla (en segundo plano),
//  la CPU lee el cuadro N+1 de la SD en el otro buffer. Si la SD no
//  alcanza el ritmo del video, se saltan cuadros para no ir en camara
//  lenta.
//
//  Audio (ver audio.h): suena por PWM+DMA en GP7 sin ayuda de la CPU. El
//  reloj de referencia es el mismo del video (time_us_64), asi que al
//  saltar cuadros el sonido no se entera: sigue a ritmo real. Mientras se
//  lee un cuadro de la SD (por trozos) se rellena el anillo de audio entre
//  trozo y trozo.
// =====================================================================
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "ff.h"
#include "st7789.h"
#include "ui.h"
#include "input.h"
#include "audio.h"
#include "video.h"

#define HDR_BYTES     512u
#define VID_MAX_PX    38400u                 // 160x120, 240x160 ...
#define VID_BUF_BYTES (76800u + 1024u)       // 2 buffers de ~75 KB
#define SEEK_SECONDS  5
#define READ_CHUNK    8192u                  // bytes por f_read de video (multiplo de 512)
#define AUDIO_MIN_HZ  4000u
#define AUDIO_MAX_HZ  32000u

static FIL vfil;
static uint8_t vbuf[2][VID_BUF_BYTES] __attribute__((aligned(4)));

static inline uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

// Abre el archivo (queda abierto en vfil si devuelve PVD_OK).
static int abrir(const char *path, pvd_info_t *o) {
    uint8_t h[HDR_BYTES];
    UINT br;
    if (f_open(&vfil, path, FA_READ) != FR_OK) return PVD_E_ABRIR;
    if (f_read(&vfil, h, sizeof h, &br) != FR_OK || br != sizeof h ||
        (memcmp(h, "PVD1", 4) && memcmp(h, "PVD2", 4))) {
        f_close(&vfil);
        return PVD_E_FORMATO;
    }
    const bool con_audio = !memcmp(h, "PVD2", 4);
    o->w = (uint16_t)rd16(h + 4);
    o->h = (uint16_t)rd16(h + 6);
    o->scale = h[8];
    const uint8_t fmt = h[9];
    o->fps_num = (uint16_t)rd16(h + 10);
    o->fps_den = (uint16_t)rd16(h + 12);
    o->frames = rd32(h + 16);
    o->stride = rd32(h + 20);
    if (!o->fps_num) o->fps_num = 15;
    if (!o->fps_den) o->fps_den = 1;

    o->frames_off = HDR_BYTES;
    o->a_rate = o->a_off = o->a_samples = 0;
    if (con_audio) {
        const uint32_t ar = rd32(h + 24), ao = rd32(h + 28), an = rd32(h + 32), fo = rd32(h + 36);
        if (fo < HDR_BYTES || (fo & 511u)) { f_close(&vfil); return PVD_E_FORMATO; }
        o->frames_off = fo;
        // Un audio raro no impide ver el video: simplemente se ignora.
        if (ar >= AUDIO_MIN_HZ && ar <= AUDIO_MAX_HZ && an && ao >= HDR_BYTES && (ao & 511u) == 0 &&
            (uint64_t)ao + an <= fo) {
            o->a_rate = ar; o->a_off = ao; o->a_samples = an;
        }
    }

    if (fmt != 0 || !o->w || !o->h || o->scale < 1 || o->scale > 8 ||
        (uint32_t)o->w * o->scale > ST7789_SCREEN_W || (uint32_t)o->h * o->scale > ST7789_SCREEN_H ||
        (uint32_t)o->w * o->h > VID_MAX_PX ||
        o->stride < (uint32_t)o->w * o->h * 2 || o->stride > VID_BUF_BYTES || (o->stride & 3)) {
        f_close(&vfil);
        return PVD_E_SOPORTE;
    }
    // No confiar en el contador si el archivo quedo cortado.
    if (f_size(&vfil) < o->frames_off) { f_close(&vfil); return PVD_E_FORMATO; }
    const uint32_t disponibles = (uint32_t)((f_size(&vfil) - o->frames_off) / o->stride);
    if (o->frames > disponibles) o->frames = disponibles;
    if (o->frames == 0) { f_close(&vfil); return PVD_E_FORMATO; }
    o->dur_s = (uint32_t)((uint64_t)o->frames * o->fps_den / o->fps_num);
    return PVD_OK;
}

int pvd_info(const char *path, pvd_info_t *o) {
    int e = abrir(path, o);
    if (e == PVD_OK) f_close(&vfil);
    return e;
}

// ---------------------------------------------------------------------
//  botones (con flancos y repeticion para IZQ/DER)
// ---------------------------------------------------------------------
#define M(p) (1u << (p))
#define MASK_ALL (M(PIN_UP) | M(PIN_DOWN) | M(PIN_LEFT) | M(PIN_RIGHT) | \
                  M(PIN_SELECT) | M(PIN_START) | M(PIN_A) | M(PIN_B))

typedef enum { ACT_NONE, ACT_EXIT, ACT_PAUSE, ACT_BACK, ACT_FWD, ACT_VOL_UP, ACT_VOL_DOWN } act_t;

static uint32_t prev_mask;
static uint64_t t_last_press, t_rep, t_rep_v;

static act_t leer_accion(void) {
    const uint64_t now = time_us_64();
    const uint32_t m = (~gpio_get_all()) & MASK_ALL;
    uint32_t press = m & ~prev_mask;
    prev_mask = m;
    if (press && now - t_last_press < 60000) press = 0;      // antirrebote
    if (press) t_last_press = now;

    if (press & (M(PIN_B) | M(PIN_SELECT))) return ACT_EXIT;
    if (press & (M(PIN_A) | M(PIN_START)))  return ACT_PAUSE;
    if (press & M(PIN_LEFT))  { t_rep = now + 450000; return ACT_BACK; }
    if (press & M(PIN_RIGHT)) { t_rep = now + 450000; return ACT_FWD; }
    if (press & M(PIN_UP))    { t_rep_v = now + 400000; return ACT_VOL_UP; }
    if (press & M(PIN_DOWN))  { t_rep_v = now + 400000; return ACT_VOL_DOWN; }
    if ((m & M(PIN_LEFT))  && now > t_rep) { t_rep = now + 180000; return ACT_BACK; }
    if ((m & M(PIN_RIGHT)) && now > t_rep) { t_rep = now + 180000; return ACT_FWD; }
    if ((m & M(PIN_UP))    && now > t_rep_v) { t_rep_v = now + 150000; return ACT_VOL_UP; }
    if ((m & M(PIN_DOWN))  && now > t_rep_v) { t_rep_v = now + 150000; return ACT_VOL_DOWN; }
    return ACT_NONE;
}

// ---------------------------------------------------------------------
//  reproduccion
// ---------------------------------------------------------------------
static pvd_info_t in;
static uint32_t   next_seq;      // indice del cuadro que leera el proximo f_read
static uint64_t   t_base;        // instante en que se mostro el cuadro f_base
static uint32_t   f_base;
static int        top_h, bot_h, bot_y;   // franjas para el HUD (0 = no hay)

static uint32_t read_us_est;     // lo que tardo la ultima lectura de un cuadro

static bool leer_cuadro(uint32_t idx, int slot) {
    UINT br = 0;
    const uint64_t t0 = time_us_64();
    if (idx != next_seq) {
        if (f_lseek(&vfil, (FSIZE_t)in.frames_off + (FSIZE_t)idx * in.stride) != FR_OK) return false;
    }
    // Por trozos: entre trozo y trozo se rellena el anillo de audio, asi la
    // lectura de un cuadro grande nunca deja al sonido sin datos.
    uint8_t *dst = vbuf[slot];
    for (uint32_t left = in.stride; left; ) {
        const UINT n = left > READ_CHUNK ? READ_CHUNK : left;
        if (f_read(&vfil, dst, n, &br) != FR_OK || br != n) return false;
        dst += n; left -= n;
        audio_service();
    }
    next_seq = idx + 1;
    read_us_est = (uint32_t)(time_us_64() - t0);
    return true;
}

static inline uint64_t due_us(uint32_t f) {
    return t_base + (uint64_t)(f - f_base) * 1000000ull * in.fps_den / in.fps_num;
}

static inline void rebase(uint32_t f) { t_base = time_us_64(); f_base = f; }

static inline unsigned seg_de(uint32_t f) {
    return (unsigned)((uint64_t)f * in.fps_den / in.fps_num);
}

// Muestra del audio que corresponde al cuadro f.
static inline uint32_t muestra_de(uint32_t f) {
    return (uint32_t)((uint64_t)f * in.a_rate * in.fps_den / in.fps_num);
}

static bool         a_sonando;             // el audio esta saliendo ahora mismo
static const char  *cur_title;
static bool         badge_on;              // hay un indicador de volumen en pantalla
static uint64_t     badge_until;

static void audio_en(uint32_t f) {         // arranca el audio en el cuadro f
    if (!audio_is_open()) return;
    audio_start(muestra_de(f));
    a_sonando = true;
}
static void audio_fuera(void) {
    if (a_sonando) { audio_stop(); a_sonando = false; }
}

static void badge_show(void) {
    ui_vol_badge(audio_volume(), AUDIO_VOL_MAX);
    badge_on = true;
    badge_until = time_us_64() + 1500000;
}
// Quita el indicador (si hay barra superior se repinta; si no, el siguiente
// cuadro de video lo borra solo).
static void badge_clear(void) {
    if (!badge_on) return;
    badge_on = false;
    if (top_h) ui_hud_top(0, top_h, cur_title);
}

static void hud(uint32_t f, bool paused) {
    if (bot_h) ui_hud_bottom(bot_y, bot_h, seg_de(f), in.dur_s, paused);
}

static unsigned last_sec;

void pvd_play(const char *path, const char *title) {
    int e = abrir(path, &in);
    if (e != PVD_OK) {
        ui_message("VIDEO NO VALIDO", UI_ERR,
                   e == PVD_E_ABRIR ? "No se pudo abrir el archivo" :
                   e == PVD_E_SOPORTE ? "Formato o tamano no soportado" : "No es un archivo .pvd",
                   "Conviertelo con", "herramientas/convertir_video.py", NULL);
        ui_line_c(205, "Pulsa un boton", UI_DIM, 1);
        sleep_ms(300);
        while (gpio_get(PIN_A) && gpio_get(PIN_B) && gpio_get(PIN_START) && gpio_get(PIN_SELECT)) sleep_ms(10);
        return;
    }

    const int vw = in.w * in.scale, vh = in.h * in.scale;
    const int x0 = (ST7789_SCREEN_W - vw) / 2, y0 = (ST7789_SCREEN_H - vh) / 2;
    const int bar_b = ST7789_SCREEN_H - (y0 + vh);
    top_h = y0 >= 20 ? (y0 > 32 ? 32 : y0) : 0;
    bot_h = bar_b >= 20 ? (bar_b > 32 ? 32 : bar_b) : 0;
    bot_y = ST7789_SCREEN_H - bot_h;

    cur_title = title;
    badge_on = false;
    a_sonando = false;
    if (in.a_rate && !audio_open(path, in.a_rate, in.a_off, in.a_samples)) in.a_rate = 0;

    st7789_fill_screen(0);
    if (top_h) ui_hud_top(0, top_h, title);

    next_seq = 0xFFFFFFFFu;          // fuerza un f_lseek al primer cuadro (con audio ya no va pegado a la cabecera)
    read_us_est = 0;
    last_sec = 0xFFFFFFFFu;
    prev_mask = (~gpio_get_all()) & MASK_ALL;       // el boton que abrio el video ya estaba apretado
    t_last_press = time_us_64();
    t_rep = 0;
    t_rep_v = 0;

    uint32_t cur = 0;                // cuadro que se esta mostrando (esta en vbuf[ci])
    int ci = 0;
    bool paused = false, salir = false;

    if (!leer_cuadro(0, ci)) goto cierre;
    audio_en(0);                                     // primero el audio (se prepara en ~10 ms)...
    rebase(0);                                       // ...y justo despues arranca el reloj del video

    while (!salir) {
        // 1) mostrar `cur` (el DMA lo envia en segundo plano)
        st7789_present_scaled((const uint16_t *)vbuf[ci], in.w, in.h, in.scale, x0, y0);

        // 2) mientras tanto, leer el siguiente de la SD
        uint32_t next = cur + 1;
        bool fin = next >= in.frames;
        if (!fin) {
            if (!paused) {
                // Si la SD no alcanza, saltar cuadros: se elige el que tocara mostrar
                // cuando termine de leerse (ahora + lo que tardo la lectura anterior).
                const uint64_t llegada = time_us_64() + read_us_est;
                const uint32_t want = f_base +
                    (uint32_t)((llegada - t_base) * in.fps_num / (1000000ull * in.fps_den));
                if (want > next) next = want < in.frames ? want : in.frames - 1;
            }
            if (!leer_cuadro(next, ci ^ 1)) fin = true;  // error de lectura = fin
        }

        // 3) HUD (una vez por segundo, o siempre que haya un cartel)
        if (seg_de(cur) != last_sec || paused || fin) {
            last_sec = seg_de(cur);
            hud(cur, paused || fin);
        }
        if (fin) ui_overlay("FIN", UI_OK);
        else if (paused) ui_overlay("PAUSA", UI_VID);
        else if (badge_on) {                            // el cuadro nuevo borra el indicador: repintarlo
            if (time_us_64() < badge_until) ui_vol_badge(audio_volume(), AUDIO_VOL_MAX);
            else badge_clear();
        }

        // 4) esperar el turno del siguiente atendiendo botones
        for (;;) {
            const act_t a = leer_accion();
            bool saltar = false;
            uint32_t objetivo = 0;

            if (a == ACT_EXIT) { salir = true; break; }

            if ((a == ACT_VOL_UP || a == ACT_VOL_DOWN) && audio_is_open()) {
                audio_set_volume(audio_volume() + (a == ACT_VOL_UP ? 1 : -1));
                badge_show();
            } else if (a == ACT_PAUSE) {
                if (fin) {                               // "FIN" + A = repetir
                    objetivo = 0; saltar = true; paused = false;
                } else {
                    paused = !paused;
                    hud(cur, paused);
                    if (paused) {
                        audio_fuera();
                        ui_overlay("PAUSA", UI_VID);
                    } else {
                        badge_clear();
                        audio_en(next);                  // el audio arranca donde esta el siguiente cuadro
                        rebase(next);                    // el siguiente sale ya
                    }
                }
            } else if (a == ACT_BACK || a == ACT_FWD) {
                const uint32_t paso = (uint32_t)SEEK_SECONDS * in.fps_num / in.fps_den;
                const uint32_t base = fin ? cur : next;
                if (a == ACT_BACK) objetivo = base > paso ? base - paso : 0;
                else               objetivo = base + paso >= in.frames ? in.frames - 1 : base + paso;
                saltar = true;
            }

            if (saltar) {
                audio_fuera();
                badge_clear();
                st7789_wait_idle();
                if (!leer_cuadro(objetivo, ci ^ 1)) { salir = true; break; }
                next = objetivo;
                if (!paused) audio_en(next);
                rebase(next);                            // se muestra ya (aun en pausa)
                break;
            }
            audio_service();
            if (fin) {                                   // ultimo cuadro: solo botones
                // el audio termina de sonar hasta el final del ultimo cuadro
                if (a_sonando && time_us_64() >= due_us(in.frames)) audio_fuera();
                sleep_ms(5);
                continue;
            }
            if (!paused && time_us_64() >= due_us(next)) break;
            sleep_us(300);
        }
        if (salir) break;

        ci ^= 1;
        cur = next;
    }

cierre:
    audio_close();
    st7789_wait_idle();
    f_close(&vfil);
}
