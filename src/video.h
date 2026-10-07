// =====================================================================
//  video.h - reproductor de videos .pvd desde la SD.
//
//  La Pico no puede decodificar MP4/AVI, asi que los videos se convierten
//  una sola vez en el PC con herramientas/convertir_video.py a un formato
//  muy simple: cuadros RGB565 crudos (sin compresion), uno tras otro.
//
//  Formato .pvd (todo little-endian):
//    0x000  "PVD1"                      4 bytes
//    0x004  ancho del cuadro            u16
//    0x006  alto del cuadro             u16
//    0x008  escala (1,2,3..)            u8   (el cuadro se agranda con DMA)
//    0x009  formato de pixel (0=RGB565) u8
//    0x00A  fps numerador               u16
//    0x00C  fps denominador             u16
//    0x00E  reservado                   u16
//    0x010  numero de cuadros           u32
//    0x014  bytes por cuadro (stride)   u32  (multiplo de 512)
//    0x018  ... relleno hasta 512 bytes
//    0x200  cuadro 0, cuadro 1, ...     (cada uno ocupa "stride" bytes)
//
//  Formato .pvd con AUDIO ("PVD2"): igual que el anterior en los primeros
//  24 bytes (solo cambia la firma) y ademas:
//    0x018  muestras por segundo del audio   u32   (p. ej. 11025)
//    0x01C  posicion de la pista de audio    u32   (multiplo de 512)
//    0x020  numero de muestras del audio     u32   (PCM 8 bits sin signo, mono)
//    0x024  posicion del cuadro 0            u32   (multiplo de 512)
//    ...    relleno hasta 512 bytes
//    audio_off:   la pista de audio entera (antes de los cuadros, para que
//                 buscar en el audio no obligue a recorrer todo el archivo)
//    frames_off:  cuadro 0, cuadro 1, ...
//  Los "PVD1" antiguos (sin audio) se siguen reproduciendo igual.
// =====================================================================
#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

typedef struct {
    uint16_t w, h;              // tamano del cuadro
    uint16_t fps_num, fps_den;
    uint8_t  scale;
    uint32_t frames;
    uint32_t stride;            // bytes por cuadro en el archivo
    uint32_t dur_s;             // duracion en segundos
    uint32_t frames_off;        // posicion del cuadro 0 en el archivo
    uint32_t a_rate;            // audio: muestras por segundo (0 = sin audio)
    uint32_t a_off;             // audio: posicion de la pista en el archivo
    uint32_t a_samples;         // audio: numero de muestras
} pvd_info_t;

enum { PVD_OK = 0, PVD_E_ABRIR, PVD_E_FORMATO, PVD_E_SOPORTE };

// Lee y valida el encabezado de un .pvd.
int pvd_info(const char *path, pvd_info_t *out);

// Reproduce el video. Vuelve cuando el usuario sale (B / SELECT).
// Controles: A/START pausa, IZQ/DER -/+ 5 s, ARRIBA/ABAJO volumen,
// B/SELECT salir.
void pvd_play(const char *path, const char *title);

#endif
