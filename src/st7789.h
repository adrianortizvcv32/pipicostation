#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>
#include <stddef.h>

// ---- Pines según tu hardware ----
#define ST7789_PIN_CS   17
#define ST7789_PIN_DC   20
#define ST7789_PIN_RST  21
#define ST7789_PIN_BL   22
#define ST7789_PIN_SCK  18
#define ST7789_PIN_MOSI 19
#define ST7789_SPI_PORT spi0

// Frecuencia SPI. OJO: el SDK redondea HACIA ABAJO al divisor mas cercano
// que alcance con clk_peri = 125 MHz, asi que:
//   62500000 -> 62.5 MHz reales (maximo del RP2040, dentro de la spec del ST7789)
//   40000000 -> 31.25 MHz reales (lo que tenias antes sin saberlo)
// Si con 62.5 MHz ves basura / colores raros (cables largos, protoboard),
// baja a 40 * 1000 * 1000.
#define ST7789_SPI_FREQ_HZ 62500000

// Resolución física del panel (tal como lo inicializas en Arduino: 240x320)
#define ST7789_PANEL_W 240
#define ST7789_PANEL_H 320

// Resolución lógica tras rotar a horizontal (equivalente a setRotation(1))
#define ST7789_SCREEN_W 320
#define ST7789_SCREEN_H 240

// ---------------------------------------------------------------------
// LOOK PIXELADO: el juego se renderiza en un framebuffer chico
// (RENDER_W x RENDER_H) y se agranda RENDER_SCALE veces al enviarlo.
//   RENDER_SCALE 2 -> 160x120 (pixelado marcado, ~ el de tu imagen)
//   RENDER_SCALE 4 ->  80x60  (MUY pixelado, y mucho mas rapido)
// Valores validos: 1, 2, 4, 5, 8 (deben dividir 320 y 240).
// ---------------------------------------------------------------------
#define RENDER_SCALE 5
#define RENDER_W (ST7789_SCREEN_W / RENDER_SCALE)
#define RENDER_H (ST7789_SCREEN_H / RENDER_SCALE)

#if (ST7789_SCREEN_W % RENDER_SCALE) != 0 || (ST7789_SCREEN_H % RENDER_SCALE) != 0
#error "RENDER_SCALE debe dividir 320 y 240 (usa 1, 2, 4, 5 u 8)"
#endif

#define ST7789_INVERT_COLORS 0

// El panel SI respeta el orden RGB estandar (se confirmo con la prueba de
// colores al inicio: pedir rojo mostraba azul y viceversa), asi que el
// macro NO debe intercambiar R y B. Si en algun otro panel volviera a
// salir invertido, ahi si tendria sentido volver a agregar el swap.
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void st7789_init(void);

// ---- Camino rapido: framebuffer + DMA ----

// Agranda fb (RENDER_W x RENDER_H, RGB565) a 320x240 y lo envia al panel
// por DMA. Retorna casi de inmediato: la transferencia sigue en segundo
// plano mientras la CPU dibuja el siguiente cuadro. Si el cuadro anterior
// todavia se esta enviando, espera a que termine.
void st7789_present(const uint16_t *fb);

// Presenta un cuadro de w x h pixeles (RGB565 en orden del CPU) agrandado
// "scale" veces (1, 2, ...) con su esquina superior izquierda en (x0,y0).
// Va por DMA y retorna al instante. OJO: NO copia el cuadro: "fb" debe
// permanecer intacto hasta que st7789_wait_idle() (o la siguiente llamada
// a una funcion de dibujo) confirme que termino. w*scale <= 320, h*scale <= 240.
void st7789_present_scaled(const uint16_t *fb, int w, int h, int scale, int x0, int y0);

// Espera a que termine cualquier transferencia DMA en curso.
void st7789_wait_idle(void);

// ---- Camino lento (bloqueante, solo para cosas puntuales) ----
void st7789_fill_screen(uint16_t color);
void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void st7789_draw_row(uint16_t x, uint16_t y, uint16_t w, const uint16_t *colores);
void st7789_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *px);

#endif