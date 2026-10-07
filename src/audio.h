// =====================================================================
//  audio.h - sonido de los videos por PWM + DMA en un solo pin (GP7).
//
//  Conexion (la del dibujo sound.png):
//      GP7 --[1 kOhm]--> base del transistor NPN
//      emisor del transistor --> GND
//      colector del transistor --> parlante --> VBUS (5 V)
//
//  Como funciona:
//   * El PWM de GP7 trabaja como portadora de ~61 kHz (inaudible, 8 bits).
//   * Un canal DMA, marcado por un temporizador DMA al ritmo de muestreo
//     del video (p. ej. 11025 Hz), copia las muestras de un anillo de RAM
//     al registro de nivel del PWM. La CPU no interviene en cada muestra:
//     solo rellena el anillo de vez en cuando (audio_service) leyendo de
//     la SD, asi que el audio no se corta mientras se dibujan cuadros.
//   * El audio va dentro del propio .pvd (PCM 8 bits sin signo, mono) y se
//     lee con un segundo archivo abierto sobre el mismo .pvd.
//
//  Cuando no suena nada GP7 queda en nivel bajo (transistor apagado, el
//  parlante no consume corriente).
// =====================================================================
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_PIN      7        // GPIO del parlante (via resistencia + transistor)
#define AUDIO_VOL_MAX  8        // volumen 0..8 (0 = mudo)
#define AUDIO_VOL_DEF  5

// Deja GP7 como salida en bajo. Llamar una vez al arrancar (evita que el
// pin quede flotando y el transistor conduzca solo).
void audio_init_pin(void);

// Abre el audio de un .pvd: "off" = posicion en el archivo de la pista
// (multiplo de 512), "nbytes" = numero de muestras (8 bits, mono) y
// "rate" = muestras por segundo. Devuelve false si algo falla (el video
// se puede reproducir igual, sin sonido).
bool audio_open(const char *path, uint32_t rate, uint32_t off, uint32_t nbytes);

// Empieza (o reanuda) a sonar desde la muestra "sample_pos". Rellena el
// anillo antes de arrancar y hace un fundido de entrada corto.
void audio_start(uint32_t sample_pos);

// Para el sonido con un fundido de salida corto y deja GP7 en bajo.
void audio_stop(void);

// Hay que llamarla seguido (al menos cada ~150 ms): rellena el anillo
// desde la SD. Si el anillo se vacio (la SD se quedo pillada), reengancha
// el audio con el tiempo real para no perder la sincronia.
void audio_service(void);

// Para y cierra el archivo de audio.
void audio_close(void);

bool audio_is_open(void);

// Volumen 0..AUDIO_VOL_MAX (se recuerda entre videos).
int  audio_volume(void);
void audio_set_volume(int v);

#endif
