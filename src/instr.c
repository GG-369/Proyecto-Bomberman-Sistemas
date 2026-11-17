#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Las funciones auxiliares solo se usan dentro de instr.c. */
static const char *skip_spaces(const char *p)
{
    while (isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static int only_spaces(const char *p)
{
    return *skip_spaces(p) == '\0';
}

/* Devuelve 1 si el texto contiene exactamente el comando. */
static int exact_command(const char *p, const char *name)
{
    size_t n = strlen(name);
    return strncmp(p, name, n) == 0 && only_spaces(p + n);
}

/* 1: reconocido; 0: inválido; -1: argumentos nulos. */
int instr_parse(const char *line, instr_t *out, int allow_bomb)
{
    if (line == NULL || out == NULL) {
        return -1;
    }

    const char *p = skip_spaces(line);
    const struct {
        const char *name;
        instr_type_t type;
    } simple[] = {
        { "UP", INSTR_UP },
        { "DOWN", INSTR_DOWN },
        { "LEFT", INSTR_LEFT },
        { "RIGHT", INSTR_RIGHT },
        { "BOMB", INSTR_BOMB }
    };

    for (size_t i = 0; i < sizeof(simple) / sizeof(simple[0]); ++i) {
        if (!exact_command(p, simple[i].name)) {
            continue;
        }
        if (simple[i].type == INSTR_BOMB && !allow_bomb) {
            return 0;
        }
        *out = (instr_t){ .type = simple[i].type, .value = 0, .line_no = 0 };
        return 1;
    }

    const char keyword[] = "SET_PRIORITY";
    size_t n = sizeof(keyword) - 1;
    if (strncmp(p, keyword, n) != 0 || !isspace((unsigned char)p[n])) {
        return 0;
    }

    const char *number = skip_spaces(p + n);
    if (*number == '\0') {
        return 0;
    }

    errno = 0;
    char *end;
    long value = strtol(number, &end, 10);
    if (number == end || errno == ERANGE ||
        value < INT_MIN || value > INT_MAX || !only_spaces(end)) {
        return 0;
    }

    *out = (instr_t){ .type = INSTR_SET_PRIORITY,
                      .value = (int)value, .line_no = 0 };
    return 1;
}

const char *instr_name(const instr_t *in, char *buf, size_t len)
{
    if (in == NULL || buf == NULL || len == 0) {
        return NULL;
    }

    const char *name;
    switch (in->type) {
        case INSTR_UP:           name = "UP"; break;
        case INSTR_DOWN:         name = "DOWN"; break;
        case INSTR_LEFT:         name = "LEFT"; break;
        case INSTR_RIGHT:        name = "RIGHT"; break;
        case INSTR_BOMB:         name = "BOMB"; break;
        case INSTR_SET_PRIORITY: name = "SET_PRIORITY"; break;
        default:                 return NULL;
    }

    int written;
    if (in->type == INSTR_SET_PRIORITY) {
        written = snprintf(buf, len, "%s %d", name, in->value);
    } else {
        written = snprintf(buf, len, "%s", name);
    }
    return written >= 0 && (size_t)written < len ? buf : NULL;
}

int instr_delta(instr_type_t t, int *dx, int *dy)
{
    if (dx == NULL || dy == NULL) {
        return -1;
    }

    *dx = 0;
    *dy = 0;

    switch (t) {
        case INSTR_UP:    *dy = -1; return 1;
        case INSTR_DOWN:  *dy =  1; return 1;
        case INSTR_LEFT:  *dx = -1; return 1;
        case INSTR_RIGHT: *dx =  1; return 1;
        default: return 0;
    }
}
