/*
 * map.c - Carga y validación del mapa (map.txt).
 *
 * Sección 7 del enunciado: mapa de 3x3 a 32x32 con símbolos:
 *   X  pared indestructible
 *   #  bloque destructible
 *   O  camino libre
 *   P  Bomberman (exactamente uno)
 *   A..D  enemigos (máx. 4, opcionales)
 *   Q  bomba activa (P3 lo escribe en runtime)
 *   *  fuego (P3 lo escribe en runtime, dura 1 tick)
 */
#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Símbolos válidos en map.txt (Q y * solo aparecen en runtime, no en archivo) */
static int is_valid_map_char(char c)
{
    return c == CELL_WALL || c == CELL_BLOCK || c == CELL_FREE ||
           c == 'P' || (c >= 'A' && c <= 'D') || c == CELL_POWERUP;
}

/* Verifica que una celda sea transitable (no pared, no bloque, no bomba, no fuego) */
int map_cell_walkable(char c)
{
    return c == CELL_FREE || c == 'P' || (c >= 'A' && c <= 'D');
}

/* Verifica coordenadas dentro del mapa cargado */
int map_in_bounds(const shared_state_t *st, int x, int y)
{
    return x >= 0 && x < st->map_w && y >= 0 && y < st->map_h;
}

/*
 * Carga map.txt en shared_state_t.
 * Valida: dimensiones, un solo P, enemigos A-D únicos, solo símbolos válidos.
 * Inicializa posiciones de Bomberman y enemigos, contadores de vivos.
 * Retorna 0 en éxito, -1 en error (loggea el detalle).
 */
int map_load(const char *path, shared_state_t *st)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "[P0] map_load: no se puede abrir %s: %s\n", path, strerror(errno));
        return -1;
    }

    char *grid[MAX_Y];
    int allocated = 0;   /* filas reservadas: se liberan en cualquier error */
    int row = 0, cols = -1;
    int bomber_count = 0;
    int enemy_seen[4] = {0};
    int enemy_count = 0;
    ssize_t n;

    /* Leer línea a línea */
    char buf[4096];
    size_t buf_len = 0;
    while ((n = read(fd, buf + buf_len, sizeof(buf) - buf_len - 1)) > 0) {
        buf_len += n;
        buf[buf_len] = '\0';

        char *start = buf;
        char *end;
        while ((end = strchr(start, '\n')) != NULL) {
            *end = '\0';
            if (row >= MAX_Y) {
                fprintf(stderr, "[P0] map_load: más de %d filas (máx. %d)\n", MAX_Y, MAX_Y);
                goto fail_fd;
            }
            size_t len = end - start;
            if (len == 0) {
                start = end + 1;
                continue;  /* ignora líneas vacías */
            }
            if (cols == -1) {
                cols = (int)len;
                if (cols < MIN_DIM || cols > MAX_X) {
                    fprintf(stderr, "[P0] map_load: ancho %d fuera de rango [%d..%d]\n",
                            cols, MIN_DIM, MAX_X);
                    goto fail_fd;
                }
            } else if ((int)len != cols) {
                fprintf(stderr, "[P0] map_load: fila %d tiene ancho %zu, se esperaba %d\n",
                        row + 1, len, cols);
                goto fail_fd;
            }

            grid[row] = malloc(len + 1);
            if (!grid[row]) {
                fprintf(stderr, "[P0] map_load: malloc falló\n");
                goto fail_fd;
            }
            allocated = row + 1;
            memcpy(grid[row], start, len);
            grid[row][len] = '\0';

            /* Validar caracteres y contar P / A-D / + */
            for (int c = 0; c < cols; c++) {
                char ch = grid[row][c];
                if (!is_valid_map_char(ch)) {
                    fprintf(stderr, "[P0] map_load: fila %d col %d: carácter inválido '%c' (0x%02x)\n",
                            row + 1, c + 1, ch, (unsigned char)ch);
                    goto fail_fd;
                }
                if (ch == 'P') {
                    bomber_count++;
                    st->bomber_x = c;
                    st->bomber_y = row;
                } else if (ch >= 'A' && ch <= 'D') {
                    int idx = ch - 'A';
                    if (enemy_seen[idx]) {
                        fprintf(stderr, "[P0] map_load: enemigo duplicado '%c'\n", ch);
                        goto fail_fd;
                    }
                    enemy_seen[idx] = 1;
                    st->enemy_x[idx] = c;
                    st->enemy_y[idx] = row;
                    st->enemy_alive[idx] = 1;
                    enemy_count++;
                } else if (ch == CELL_POWERUP) {
                    /* Power-up: se guardará la posición al copiar a shared_state_t */
                }
            }
            row++;
            start = end + 1;
        }

        /* Mover lo que queda al inicio del buffer */
        size_t remaining = buf_len - (start - buf);
        if (remaining > 0)
            memmove(buf, start, remaining);
        buf_len = remaining;
    }

    if (n == -1) {
        fprintf(stderr, "[P0] map_load: error leyendo %s: %s\n", path, strerror(errno));
        goto fail_fd;
    }

    /* Procesar última línea sin \n */
    if (buf_len > 0) {
        if (row >= MAX_Y) {
            fprintf(stderr, "[P0] map_load: más de %d filas\n", MAX_Y);
            goto fail_fd;
        }
        if (cols == -1) {
            cols = (int)buf_len;
            if (cols < MIN_DIM || cols > MAX_X) {
                fprintf(stderr, "[P0] map_load: ancho %d fuera de rango [%d..%d]\n",
                        cols, MIN_DIM, MAX_X);
                goto fail_fd;
            }
        } else if ((int)buf_len != cols) {
            fprintf(stderr, "[P0] map_load: última fila ancho %zu, se esperaba %d\n",
                    buf_len, cols);
            goto fail_fd;
        }
        grid[row] = malloc(buf_len + 1);
        if (!grid[row]) {
            fprintf(stderr, "[P0] map_load: malloc falló\n");
            goto fail_fd;
        }
        allocated = row + 1;
        memcpy(grid[row], buf, buf_len);
        grid[row][buf_len] = '\0';

        for (int c = 0; c < cols; c++) {
            char ch = grid[row][c];
            if (!is_valid_map_char(ch)) {
                fprintf(stderr, "[P0] map_load: fila %d col %d: carácter inválido '%c'\n",
                        row + 1, c + 1, ch);
                goto fail_fd;
            }
            if (ch == 'P') {
                bomber_count++;
                st->bomber_x = c;
                st->bomber_y = row;
            } else if (ch >= 'A' && ch <= 'D') {
                int idx = ch - 'A';
                if (enemy_seen[idx]) {
                    fprintf(stderr, "[P0] map_load: enemigo duplicado '%c'\n", ch);
                    goto fail_fd;
                }
                enemy_seen[idx] = 1;
                st->enemy_x[idx] = c;
                st->enemy_y[idx] = row;
                st->enemy_alive[idx] = 1;
                enemy_count++;
            } else if (ch == CELL_POWERUP) {
                /* Power-up: se guardará la posición al copiar a shared_state_t */
            }
        }
        row++;
    }

    close(fd);

    if (row < MIN_DIM) {
        fprintf(stderr, "[P0] map_load: menos de %d filas (mín. %d)\n", MIN_DIM, MIN_DIM);
        goto fail;
    }
    if (bomber_count != 1) {
        fprintf(stderr, "[P0] map_load: debe haber exactamente un 'P' (Bomberman), hay %d\n", bomber_count);
        goto fail;
    }
    if (enemy_count > NUM_ENEMIES) {
        fprintf(stderr, "[P0] map_load: máximo %d enemigos (A-D), hay %d\n", NUM_ENEMIES, enemy_count);
        goto fail;
    }

    /* Copiar a shared_state_t (bajo m_map, pero P0 aún no ha hecho fork, así que sin lock) */
    st->map_w = cols;
    st->map_h = row;
    st->powerup_count = 0;
    for (int y = 0; y < row; y++) {
        for (int x = 0; x < cols; x++) {
            char ch = grid[y][x];
            /* Sección 7: celdas iniciales P y A-D quedan como camino libre (O) en map_grid */
            if (ch == 'P' || (ch >= 'A' && ch <= 'D')) {
                st->map_grid[y][x] = CELL_FREE;
            } else if (ch == CELL_POWERUP) {
                st->map_grid[y][x] = CELL_FREE;  /* power-up se recoge, celda queda libre */
                if (st->powerup_count < MAX_Y) {
                    st->powerup_x[st->powerup_count] = x;
                    st->powerup_y[st->powerup_count] = y;
                    st->powerup_count++;
                }
            } else {
                st->map_grid[y][x] = ch;
            }
        }
        free(grid[y]);
    }
    /* Rellenar resto con espacios por si acaso */
    for (int y = row; y < MAX_Y; y++)
        for (int x = 0; x < MAX_X; x++)
            st->map_grid[y][x] = ' ';

    st->bomber_moves_done = 0;
    st->enemy_moves_done = 0;
    for (int i = 0; i < NUM_ENEMIES; i++)
        if (!enemy_seen[i])
            st->enemy_alive[i] = 0;
    st->enemies_alive = enemy_count;
    st->initial_enemies = enemy_count;
    st->active_bombs = 0;
    st->blocks_destroyed = 0;
    st->bomber_powerup = 0;
    st->bomb_range_bonus = 0;

    log_line("[P0] mapa cargado: %dx%d, Bomberman en (%d,%d), %d enemigos, %d power-ups",
             st->map_w, st->map_h, st->bomber_x, st->bomber_y, st->enemies_alive, st->powerup_count);
    return 0;

fail_fd:
    close(fd);
fail:
    for (int i = 0; i < allocated; i++)
        free(grid[i]);
    return -1;
}

/*
 * Genera snapshot del mapa para P4 (renderer) y logs.
 * Combina mapa base + posiciones actuales + bombas activas + fuego.
 * Precondición: caller tiene m_pos y m_map tomados (en ese orden).
 */
void map_snapshot(const shared_state_t *st, char out[MAX_Y][MAX_X + 1])
{
    /* Base: mapa estático (paredes, bloques, camino) */
    for (int y = 0; y < st->map_h; y++) {
        for (int x = 0; x < st->map_w; x++) {
            out[y][x] = st->map_grid[y][x];
        }
        out[y][st->map_w] = '\0';
    }

    /* Bombas activas (Q) */
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (st->bomb_timer[i] > 0) {
            int bx = st->bomb_x[i];
            int by = st->bomb_y[i];
            if (map_in_bounds(st, bx, by))
                out[by][bx] = CELL_BOMB;
        }
    }

    /* Fuego (*) - se escribe en m_map por explosion_thread, pero aquí
     * solo mostramos lo que haya en map_grid si P3 lo puso como CELL_FIRE.
     * Nota: el fuego dura 1 tick; P3 lo pone y lo quita en el mismo cierre. */

    /* Power-ups (+) que no se han recogido aún */
    for (int i = 0; i < st->powerup_count; i++) {
        int px = st->powerup_x[i];
        int py = st->powerup_y[i];
        if (map_in_bounds(st, px, py) && st->map_grid[py][px] == CELL_FREE)
            out[py][px] = CELL_POWERUP;
    }

    /* Bomberman (P) - siempre visible por encima de todo */
    if (map_in_bounds(st, st->bomber_x, st->bomber_y))
        out[st->bomber_y][st->bomber_x] = 'P';

    /* Enemigos vivos (A-D) */
    for (int i = 0; i < NUM_ENEMIES; i++) {
        if (st->enemy_alive[i]) {
            int ex = st->enemy_x[i];
            int ey = st->enemy_y[i];
            if (map_in_bounds(st, ex, ey))
                out[ey][ex] = 'A' + i;
        }
    }

    /* Filas restantes vacías */
    for (int y = st->map_h; y < MAX_Y; y++)
        out[y][0] = '\0';
}