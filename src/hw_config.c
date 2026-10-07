// Configuracion de la tarjeta SD (libreria no-OS-FatFS-SD-SPI-RPi-Pico).
// Numeros de GPIO (no de pin fisico). CAMBIA ESTOS PINES si chocan con los
// de tu pantalla ST7789 o tus botones.
//
// Botones usados: 2,3,4,5,6,8,9,10,11,28.
// Por defecto la SD va en SPI1 con los GPIO 12-15 (libres):
//   MISO -> GP12   CS -> GP13   SCK -> GP14   MOSI -> GP15
//   VCC -> 3V3     GND -> GND

#include "hw_config.h"

#ifdef SD_API_ANTIGUA
// ---------------- Version antigua de la libreria (FatFs_SPI/) ----------------
static spi_t spis[] = {{
    .hw_inst   = spi1,
    .miso_gpio = 12,
    .mosi_gpio = 15,
    .sck_gpio  = 14,
    .baud_rate = 12500 * 1000,
}};

static sd_card_t sd_cards[] = {{
    .pcName          = "0:",
    .spi             = &spis[0],
    .ss_gpio         = 13,
    .use_card_detect = false,
}};

size_t sd_get_num() { return count_of(sd_cards); }
sd_card_t *sd_get_by_num(size_t num) {
    if (num < sd_get_num()) return &sd_cards[num];
    return NULL;
}
size_t spi_get_num() { return count_of(spis); }
spi_t *spi_get_by_num(size_t num) {
    if (num < spi_get_num()) return &spis[num];
    return NULL;
}
#else
// ---------------- Version nueva de la libreria (src/) ----------------
static spi_t spi = {
    .hw_inst   = spi1,
    .miso_gpio = 12,
    .mosi_gpio = 15,
    .sck_gpio  = 14,
    .baud_rate = 12500 * 1000,
};

static sd_spi_if_t spi_if = {
    .spi     = &spi,
    .ss_gpio = 13,
};

static sd_card_t sd_card = {
    .type     = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num() { return 1; }
sd_card_t *sd_get_by_num(size_t num) {
    if (num == 0) return &sd_card;
    return NULL;
}
#endif
