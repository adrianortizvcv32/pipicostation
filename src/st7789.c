#include "st7789.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>
#include <stdbool.h>

// ---- Comandos ST7789 ----
#define CMD_SWRESET 0x01
#define CMD_SLPOUT  0x11
#define CMD_INVOFF  0x20
#define CMD_INVON   0x21
#define CMD_NORON   0x13
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A

// ---------------------------------------------------------------------
// Envio por DMA SIN buffer de pantalla completa.
//
// Antes habia un out_buf de 320x240 (150 KB). Ahora:
//   - snap  : copia congelada del framebuffer chico (RENDER_W x RENDER_H,
//             9.6 KB), para que la CPU pueda dibujar el cuadro siguiente
//             en "framebuffer" mientras este se esta enviando.
//   - linea : DOS buffers de UNA linea de pantalla (320 px = 640 bytes).
//             Cada fila del framebuffer chico se agranda a una linea y esa
//             misma linea se envia RENDER_SCALE veces seguidas (sin copiar).
// Una interrupcion del DMA (DMA_IRQ_1) relanza la siguiente transferencia
// y prepara la proxima fila en el otro buffer mientras la actual se envia.
// ---------------------------------------------------------------------
static uint16_t snap[RENDER_W * RENDER_H] __attribute__((aligned(4)));
static uint16_t linea[2][ST7789_SCREEN_W] __attribute__((aligned(4)));

// Acceso de 32 bits sin problemas de strict-aliasing.
typedef uint32_t __attribute__((may_alias)) u32a;

static int  dma_ch = -1;
static volatile bool dma_en_curso = false;
static volatile bool frame_terminado = true;
static volatile int  fila_act = 0;     // fila del framebuffer chico que se esta enviando
static volatile int  rep_act = 0;      // cuantas veces se envio ya esa fila (0..RENDER_SCALE-1)
static volatile int  buf_act = 0;      // cual de los 2 buffers de linea se esta enviando
// Cuadro que se esta enviando (fijados antes de arrancar el DMA)
static const uint16_t *g_src = 0;      // pixeles origen
static int g_w = RENDER_W, g_h = RENDER_H, g_scale = RENDER_SCALE;
static int g_line = RENDER_W * RENDER_SCALE;   // pixeles por linea de pantalla
static bool spi_16bit = false;

static inline void cs_low(void)  { gpio_put(ST7789_PIN_CS, 0); }
static inline void cs_high(void) { gpio_put(ST7789_PIN_CS, 1); }
static inline void dc_cmd(void)  { gpio_put(ST7789_PIN_DC, 0); }
static inline void dc_data(void) { gpio_put(ST7789_PIN_DC, 1); }

// Comandos y parametros van en frames de 8 bits; los pixeles del DMA van
// en frames de 16 bits (el SPI manda el bit alto primero, que es
// justo el orden que quiere el ST7789 -> no hace falta invertir bytes).
static inline void spi_modo8(void) {
    if (spi_16bit) {
        spi_set_format(ST7789_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        spi_16bit = false;
    }
}

static inline void spi_modo16(void) {
    if (!spi_16bit) {
        spi_set_format(ST7789_SPI_PORT, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        spi_16bit = true;
    }
}

static void write_cmd(uint8_t cmd) {
    spi_modo8();
    dc_cmd();
    cs_low();
    spi_write_blocking(ST7789_SPI_PORT, &cmd, 1);
    cs_high();
}

static void write_data(const uint8_t *buf, size_t len) {
    spi_modo8();
    dc_data();
    cs_low();
    spi_write_blocking(ST7789_SPI_PORT, buf, len);
    cs_high();
}

static void write_data8(uint8_t v) { write_data(&v, 1); }

static void set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t buf[4];

    write_cmd(CMD_CASET);
    buf[0] = x0 >> 8; buf[1] = x0 & 0xFF;
    buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    write_data(buf, 4);

    write_cmd(CMD_RASET);
    buf[0] = y0 >> 8; buf[1] = y0 & 0xFF;
    buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    write_data(buf, 4);

    write_cmd(CMD_RAMWR);
}

// ---------------------------------------------------------------------
// Expansion de UNA fila de snap a una linea de pantalla, y control del DMA
// ---------------------------------------------------------------------

// Agranda la fila sy del cuadro actual a una linea de pantalla (cada pixel
// se repite g_scale veces en horizontal).
static void __not_in_flash_func(expandir_fila)(int sy, uint16_t *dst) {
    const uint16_t *src = g_src + sy * g_w;
    const int w = g_w;
    if (g_scale == 2) {
        u32a *d32 = (u32a *)dst;
        for (int x = 0; x < w; x++) {
            const uint32_t p = src[x];
            d32[x] = p | (p << 16);
        }
    } else if (g_scale == 1) {
        for (int x = 0; x < w; x++) dst[x] = src[x];
    } else {
        uint16_t *d = dst;
        for (int x = 0; x < w; x++) {
            const uint16_t p = src[x];
            for (int k = 0; k < g_scale; k++) *d++ = p;
        }
    }
}

// Lanza el envio de la linea del buffer actual. Si esta es la ULTIMA
// repeticion de la fila, aprovecha para dejar lista la fila siguiente en
// el otro buffer mientras el DMA trabaja.
static void __not_in_flash_func(lanzar_linea)(void) {
    dma_channel_transfer_from_buffer_now((uint)dma_ch, linea[buf_act], (uint32_t)g_line);
    if (rep_act == g_scale - 1 && fila_act + 1 < g_h) {
        expandir_fila(fila_act + 1, linea[buf_act ^ 1]);
    }
}

// Se ejecuta cada vez que termina el envio de una linea.
static void __not_in_flash_func(dma_irq_handler)(void) {
    if (!(dma_hw->ints1 & (1u << dma_ch))) return;   // no es nuestro canal
    dma_hw->ints1 = 1u << dma_ch;                    // limpia la interrupcion

    if (++rep_act >= g_scale) {
        rep_act = 0;
        if (++fila_act >= g_h) {
            frame_terminado = true;                   // CS lo suelta st7789_wait_idle()
            return;
        }
        buf_act ^= 1;
    }
    lanzar_linea();
}

void st7789_init(void) {
    gpio_init(ST7789_PIN_CS);  gpio_set_dir(ST7789_PIN_CS, GPIO_OUT);
    gpio_init(ST7789_PIN_DC);  gpio_set_dir(ST7789_PIN_DC, GPIO_OUT);
    gpio_init(ST7789_PIN_RST); gpio_set_dir(ST7789_PIN_RST, GPIO_OUT);
    gpio_init(ST7789_PIN_BL);  gpio_set_dir(ST7789_PIN_BL, GPIO_OUT);

    gpio_put(ST7789_PIN_BL, 1); // enciende backlight (cambia a 0 si tu módulo es activo en bajo)
    cs_high();

    spi_init(ST7789_SPI_PORT, ST7789_SPI_FREQ_HZ);
    gpio_set_function(ST7789_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(ST7789_PIN_MOSI, GPIO_FUNC_SPI);
    spi_set_format(ST7789_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_16bit = false;

    // Reset físico
    gpio_put(ST7789_PIN_RST, 1); sleep_ms(10);
    gpio_put(ST7789_PIN_RST, 0); sleep_ms(10);
    gpio_put(ST7789_PIN_RST, 1); sleep_ms(120);

    write_cmd(CMD_SWRESET); sleep_ms(150);
    write_cmd(CMD_SLPOUT);  sleep_ms(120);

    write_cmd(CMD_COLMOD);
    write_data8(0x55); // 16 bits/pixel (RGB565)
    sleep_ms(10);

    // MADCTL para orientación horizontal, girada 180 grados respecto a la
    // version anterior (la pantalla se veia "de cabeza": techo/piso/pared
    // quedaban correctos en el buffer, pero el panel fisico los mostraba
    // volteados). Antes: 0x60 (MX=1 MV=1). Ahora: 0xA0 (MY=1 MV=1), que es
    // el otro modo "landscape" de estos paneles, rotado 180 grados.
    // Si vuelve a salir al reves, proba con 0x00 o 0xC0 (los otros dos
    // modos posibles: portrait normal y portrait invertido).
    write_cmd(CMD_MADCTL);
    write_data8(0xA0); // MY=1 MX=0 MV=1 ML=0 RGB=0 MH=0 -> landscape invertido

#if ST7789_INVERT_COLORS
    write_cmd(CMD_INVON);
#else
    write_cmd(CMD_INVOFF);
#endif

    write_cmd(CMD_NORON); sleep_ms(10);
    write_cmd(CMD_DISPON); sleep_ms(100);

    // Canal DMA que alimenta el SPI (16 bits por transferencia, ritmo
    // marcado por el propio SPI). Avisa por DMA_IRQ_1 al terminar cada
    // linea (se usa IRQ_1 y un handler compartido para no chocar con la
    // libreria de la SD).
    dma_ch = (int)dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config((uint)dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_dreq(&c, spi_get_dreq(ST7789_SPI_PORT, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure((uint)dma_ch, &c,
                          &spi_get_hw(ST7789_SPI_PORT)->dr,
                          linea[0],
                          ST7789_SCREEN_W,
                          false);

    dma_hw->ints1 = 1u << dma_ch;
    dma_channel_set_irq1_enabled((uint)dma_ch, true);
    irq_add_shared_handler(DMA_IRQ_1, dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    frame_terminado = true;
    dma_en_curso = false;
}

// ---------------------------------------------------------------------
// Presentacion por DMA
// ---------------------------------------------------------------------

void st7789_wait_idle(void) {
    if (!dma_en_curso) return;

    while (!frame_terminado) tight_loop_contents();
    // El DMA termina cuando el ultimo dato entra a la FIFO; hay que esperar
    // a que el SPI lo termine de sacar antes de soltar CS.
    while (spi_is_busy(ST7789_SPI_PORT)) tight_loop_contents();
    cs_high();

    // El SPI es solo-TX: descartamos lo que haya quedado en la FIFO RX.
    while (spi_is_readable(ST7789_SPI_PORT)) (void)spi_get_hw(ST7789_SPI_PORT)->dr;
    spi_get_hw(ST7789_SPI_PORT)->icr = SPI_SSPICR_RORIC_BITS;

    dma_en_curso = false;
    spi_modo8();
}

static void empezar_cuadro(const uint16_t *src, int w, int h, int scale, int x0, int y0) {
    // El DMA todavia puede estar enviando el cuadro anterior.
    st7789_wait_idle();

    g_src = src; g_w = w; g_h = h; g_scale = scale; g_line = w * scale;

    set_addr_window((uint16_t)x0, (uint16_t)y0,
                    (uint16_t)(x0 + w * scale - 1), (uint16_t)(y0 + h * scale - 1));

    spi_modo16();
    dc_data();
    cs_low();

    fila_act = 0;
    rep_act = 0;
    buf_act = 0;
    frame_terminado = false;
    dma_en_curso = true;

    expandir_fila(0, linea[0]);
    lanzar_linea();
    // Sigue en segundo plano (la interrupcion encadena el resto de lineas);
    // CS se suelta en st7789_wait_idle().
}

void st7789_present(const uint16_t *fb) {
    st7789_wait_idle();
    // Copia congelada: a partir de aca la CPU puede dibujar el cuadro
    // siguiente en "fb" sin afectar lo que se esta enviando.
    memcpy(snap, fb, sizeof snap);
    empezar_cuadro(snap, RENDER_W, RENDER_H, RENDER_SCALE, 0, 0);
}

void st7789_present_scaled(const uint16_t *fb, int w, int h, int scale, int x0, int y0) {
    if (w < 1 || h < 1 || scale < 1 || w * scale > ST7789_SCREEN_W || h * scale > ST7789_SCREEN_H) return;
    empezar_cuadro(fb, w, h, scale, x0, y0);
}

// ---------------------------------------------------------------------
// Camino lento (bloqueante)
// ---------------------------------------------------------------------

void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x >= ST7789_SCREEN_W || y >= ST7789_SCREEN_H) return;
    if (x + w > ST7789_SCREEN_W) w = ST7789_SCREEN_W - x;
    if (y + h > ST7789_SCREEN_H) h = ST7789_SCREEN_H - y;
    if (w == 0 || h == 0) return;

    st7789_wait_idle();
    set_addr_window(x, y, x + w - 1, y + h - 1);

    uint8_t hi = color >> 8, lo = color & 0xFF;
    uint8_t line[64 * 2];
    for (int i = 0; i < 64; i++) { line[i * 2] = hi; line[i * 2 + 1] = lo; }

    uint32_t total = (uint32_t)w * h;
    dc_data();
    cs_low();
    while (total > 0) {
        uint32_t chunk = total > 64 ? 64 : total;
        spi_write_blocking(ST7789_SPI_PORT, line, chunk * 2);
        total -= chunk;
    }
    cs_high();
}

void st7789_fill_screen(uint16_t color) {
    st7789_fill_rect(0, 0, ST7789_SCREEN_W, ST7789_SCREEN_H, color);
}

// Pinta una fila horizontal de w pixeles, cada uno con SU PROPIO color.
void st7789_draw_row(uint16_t x, uint16_t y, uint16_t w, const uint16_t *colores) {
    if (x >= ST7789_SCREEN_W || y >= ST7789_SCREEN_H) return;
    if (x + w > ST7789_SCREEN_W) w = ST7789_SCREEN_W - x;
    if (w == 0) return;

    st7789_wait_idle();
    set_addr_window(x, y, x + w - 1, y);

    static uint8_t buf[ST7789_SCREEN_W * 2];
    for (uint16_t i = 0; i < w; i++) {
        buf[i * 2]     = colores[i] >> 8;
        buf[i * 2 + 1] = colores[i] & 0xFF;
    }

    dc_data();
    cs_low();
    spi_write_blocking(ST7789_SPI_PORT, buf, (size_t)w * 2);
    cs_high();
}

void st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    if (x >= ST7789_SCREEN_W || y >= ST7789_SCREEN_H) return;
    st7789_wait_idle();
    set_addr_window(x, y, x, y);
    uint8_t buf[2] = { color >> 8, color & 0xFF };
    write_data(buf, 2);
}
// ---- Solo launcher: copia un rectangulo de pixeles RGB565 (bloqueante) ----
void st7789_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px) {
    if (x >= ST7789_SCREEN_W || y >= ST7789_SCREEN_H || w == 0 || h == 0) return;
    if (x + w > ST7789_SCREEN_W || y + h > ST7789_SCREEN_H) return;
    st7789_wait_idle();
    set_addr_window(x, y, x + w - 1, y + h - 1);
    static uint8_t buf[ST7789_SCREEN_W * 2];
    dc_data();
    cs_low();
    for (uint16_t row = 0; row < h; row++) {
        const uint16_t *src = px + (size_t)row * w;
        for (uint16_t i = 0; i < w; i++) {
            buf[i * 2]     = src[i] >> 8;
            buf[i * 2 + 1] = src[i] & 0xFF;
        }
        spi_write_blocking(ST7789_SPI_PORT, buf, (size_t)w * 2);
    }
    cs_high();
}
