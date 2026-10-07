// =====================================================================
//  audio.c - ver audio.h
//
//  Cadena de sonido:
//     SD (.pvd) --f_read--> lut[volumen] --> anillo de RAM (u16)
//        --DMA (marcada por un timer DMA a "rate" Hz)--> PWM nivel de GP7
//
//  El anillo tiene RING_N muestras (~370 ms a 11025 Hz). La CPU solo lo
//  rellena en audio_service(); el DMA lo vacia solo y a ritmo exacto.
//  "consumed" (muestras que ya salieron) se saca del contador del DMA, asi
//  no hace falta ninguna interrupcion y el control de huecos es exacto.
// =====================================================================
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "ff.h"
#include "audio.h"

#define RING_N       4096u                 // muestras de 16 bits
#define RING_BYTES   (RING_N * 2u)         // 8192 = 1 << 13
#define RING_BITS    13
#define CHUNK        512u                  // muestras por lectura de SD (= 1 sector)
#define DMA_COUNT    0x0FFFFFFFu           // ~6 horas a 11 kHz (valido en RP2040 y RP2350)
#define PWM_WRAP     255                   // 8 bits
#define PWM_CLKDIV   8                     // 125 MHz / 8 / 256 = 61 kHz de portadora
#define FADE_IN      512u                  // muestras de fundido al arrancar

_Static_assert(RING_BYTES == (1u << RING_BITS), "RING_BITS no cuadra con RING_N");

// Anillo alineado a su tamano (el DMA envuelve la direccion de lectura).
static uint16_t ring[RING_N] __attribute__((aligned(RING_BYTES)));
static uint16_t lut[256];                  // muestra 8 bits sin signo -> nivel PWM con volumen

static FIL      afil;
static bool     a_open, a_run, hw_ok;
static uint32_t a_rate, a_off, a_total;    // a_total = muestras de la pista
static int      dma_ch = -1, dma_tmr = -1;
static uint     slice, chan;
static int      volume = AUDIO_VOL_DEF;

static uint32_t base_sample;               // muestra de la pista que ocupaba la posicion 0 al arrancar
static uint32_t written;                   // muestras escritas en el anillo desde el arranque
static uint32_t file_pos;                  // siguiente muestra de la pista que se leera

// Amplitud maxima del PWM (0..255) para cada volumen. El centro (silencio)
// queda en la mitad: cuanto mas volumen, mas corriente continua en el parlante.
static const uint8_t vol_scale[AUDIO_VOL_MAX + 1] = { 0, 28, 48, 72, 104, 140, 180, 220, 255 };

static void build_lut(void) {
    const uint32_t sc = vol_scale[volume];
    for (uint32_t i = 0; i < 256; i++) lut[i] = (uint16_t)((i * sc + 127u) / 255u);
}

static void pin_idle(void) {
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(AUDIO_PIN, GPIO_OUT);
    gpio_put(AUDIO_PIN, 0);
}

void audio_init_pin(void) {
    gpio_init(AUDIO_PIN);
    pin_idle();
}

int audio_volume(void) { return volume; }

void audio_set_volume(int v) {
    if (v < 0) v = 0;
    if (v > AUDIO_VOL_MAX) v = AUDIO_VOL_MAX;
    volume = v;
    build_lut();
}

bool audio_is_open(void) { return a_open; }

// ---------------------------------------------------------------------
//  hardware
// ---------------------------------------------------------------------
// Fraccion del timer DMA: ritmo = clk_sys * n / d (n, d de 16 bits, n <= d).
// Se busca la n pequena que deje el menor error relativo para que el audio
// no se desfase del video en peliculas largas.
static void set_rate(uint32_t rate) {
    const uint32_t sys = clock_get_hz(clk_sys);
    uint32_t bn = 1, bd = (sys + rate / 2) / rate;
    uint64_t be = UINT64_MAX;
    for (uint32_t n = 1; n <= 64; n++) {
        const uint64_t d = ((uint64_t)n * sys + rate / 2) / rate;
        if (d > 65535u || d < n) continue;
        const int64_t diff = (int64_t)n * sys - (int64_t)rate * (int64_t)d;
        const uint64_t e = (uint64_t)(diff < 0 ? -diff : diff);
        // error relativo = e / (rate * d): se compara e / d
        if (be == UINT64_MAX || e * bd < be * d) { bn = n; bd = (uint32_t)d; be = e; }
    }
    if (bd > 65535u) bd = 65535u;
    dma_timer_set_fraction((uint)dma_tmr, (uint16_t)bn, (uint16_t)bd);
}

static void hw_init(void) {
    if (hw_ok) return;
    slice = pwm_gpio_to_slice_num(AUDIO_PIN);
    chan  = pwm_gpio_to_channel(AUDIO_PIN);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&cfg, PWM_CLKDIV);
    pwm_config_set_wrap(&cfg, PWM_WRAP);
    pwm_init(slice, &cfg, false);
    pwm_set_chan_level(slice, chan, 0);
    dma_ch  = (int)dma_claim_unused_channel(true);
    dma_tmr = (int)dma_claim_unused_timer(true);
    hw_ok = true;
}

static void halt(void) {
    if (a_run) {
        dma_channel_abort((uint)dma_ch);
        a_run = false;
    }
}

// ---------------------------------------------------------------------
//  anillo
// ---------------------------------------------------------------------
// Anade hasta CHUNK muestras al anillo: de la SD, o silencio si la pista
// ya termino (o si la lectura fallo). Siempre escribe algo para que el DMA
// nunca reproduzca datos viejos.
static void put_chunk(uint32_t max_n) {
    uint8_t tmp[CHUNK];
    uint32_t n = max_n > CHUNK ? CHUNK : max_n;

    if (file_pos < a_total) {
        const uint32_t al = CHUNK - ((a_off + file_pos) & (CHUNK - 1u));   // hasta el limite de sector
        if (n > al) n = al;
        if (n > a_total - file_pos) n = a_total - file_pos;
        UINT br = 0;
        if (f_read(&afil, tmp, n, &br) == FR_OK && br > 0) {
            for (UINT i = 0; i < br; i++) ring[(written + i) & (RING_N - 1u)] = lut[tmp[i]];
            file_pos += br;
            written  += br;
            return;
        }
        a_total = file_pos;                    // error de lectura: tratar como fin de la pista
        n = max_n > CHUNK ? CHUNK : max_n;
    } else {
        n = max_n > CHUNK ? CHUNK : max_n;
    }
    const uint16_t sil = lut[128];
    for (uint32_t i = 0; i < n; i++) ring[(written + i) & (RING_N - 1u)] = sil;
    written += n;
}

static uint32_t consumed_now(void) {
    return DMA_COUNT - (dma_channel_hw_addr((uint)dma_ch)->transfer_count & DMA_COUNT);
}

// ---------------------------------------------------------------------
//  API
// ---------------------------------------------------------------------
bool audio_open(const char *path, uint32_t rate, uint32_t off, uint32_t nbytes) {
    audio_close();
    if (!rate || !nbytes) return false;
    if (f_open(&afil, path, FA_READ) != FR_OK) return false;
    hw_init();
    a_rate = rate; a_off = off; a_total = nbytes;
    set_rate(rate);
    build_lut();
    a_open = true;
    return true;
}

void audio_start(uint32_t pos) {
    if (!a_open) return;
    halt();
    if (pos > a_total) pos = a_total;
    if (f_lseek(&afil, (FSIZE_t)a_off + pos) != FR_OK) return;
    file_pos = pos; base_sample = pos; written = 0;
    while (written < RING_N) put_chunk(RING_N - written);

    for (uint32_t i = 0; i < FADE_IN; i++)                 // sin "pop" al empezar
        ring[i] = (uint16_t)(((uint32_t)ring[i] * i) / FADE_IN);

    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    pwm_set_chan_level(slice, chan, 0);
    pwm_set_enabled(slice, true);

    dma_channel_config c = dma_channel_get_default_config((uint)dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_ring(&c, false, RING_BITS);          // envuelve la LECTURA en 8 KB
    channel_config_set_dreq(&c, dma_get_timer_dreq((uint)dma_tmr));
    // Escritura de 16 bits en la mitad del registro CC que corresponde al
    // canal de GP7 (impar = B = mitad alta).
    volatile uint16_t *dst = (volatile uint16_t *)&pwm_hw->slice[slice].cc + chan;
    dma_channel_configure((uint)dma_ch, &c, dst, ring, DMA_COUNT, true);
    a_run = true;
}

void audio_stop(void) {
    if (a_run) {
        const uint32_t cc = pwm_hw->slice[slice].cc;
        const uint32_t lvl = (cc >> (chan ? 16 : 0)) & 0xFFFFu;
        halt();
        for (int i = 16; i >= 0; i--) {                    // fundido de salida (~17 ms)
            pwm_set_chan_level(slice, chan, (uint16_t)(lvl * (uint32_t)i / 16u));
            sleep_us(1000);
        }
        pwm_set_enabled(slice, false);
    }
    if (hw_ok) pin_idle();
}

void audio_service(void) {
    if (!a_run) return;
    const uint32_t consumed = consumed_now();

    if (consumed >= written) {
        // El anillo se vacio (la SD tardo demasiado): el DMA ya esta repitiendo
        // datos viejos. Reenganchar con el instante real (+ lo que tarda esto).
        audio_start(base_sample + consumed + a_rate / 80u);
        return;
    }
    while (RING_N - (written - consumed) >= CHUNK) put_chunk(CHUNK);
}

void audio_close(void) {
    if (hw_ok) audio_stop();
    if (a_open) {
        f_close(&afil);
        a_open = false;
    }
}
