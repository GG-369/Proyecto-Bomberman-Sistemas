/*
 * util.c - Utilidades: log a consola y pausas.
 *
 * log_line hace fflush después de cada línea. Es importante porque varios
 * procesos escriben en el mismo stdout: si un proceso dejara texto en su
 * buffer, aparecería desordenado (o duplicado tras un fork).
 */
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <time.h>

void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

void sleep_ms(int ms)
{
    if (ms <= 0)
        return;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ; /* reintenta si una señal interrumpe la pausa */
}

const char *result_name(int result)
{
    switch (result) {
    case RESULT_VICTORY:     return "VICTORIA (todos los enemigos eliminados)";
    case RESULT_DEFEAT:      return "DERROTA (Bomberman sin vidas)";
    case RESULT_MAX_TICKS:   return "LIMITE DE TICKS (max_ticks)";
    case RESULT_NO_MOVES:    return "SIN INSTRUCCIONES Y SIN BOMBAS ACTIVAS";
    case RESULT_INTERRUPTED: return "INTERRUMPIDO (SIGINT)";
    case RESULT_ERROR:       return "ERROR (un proceso hijo terminó inesperadamente)";
    default:                 return "EN CURSO";
    }
}
