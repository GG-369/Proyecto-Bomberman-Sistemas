/* Pruebas de Fase 1: 26 comprobaciones de SHM, mapa e instrucciones. */
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;

#define CHECK(expression) do {                                             \
    if (!(expression)) {                                                   \
        fprintf(stderr, "FALLO %d: %s (línea %d)\n",                       \
                checks + 1, #expression, __LINE__);                        \
        failed = 1;                                                        \
        goto cleanup;                                                      \
    }                                                                      \
    checks++;                                                              \
} while (0)

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Uso: %s ruta/a/map.txt\n", argv[0]);
        return EXIT_FAILURE;
    }
    int failed = 0;
    shared_state_t *st = shm_create();
    CHECK(st != NULL);                                  /* 1 */
    CHECK(shm_init_sync(st) == 0);                       /* 2 */

    /* Los semáforos de turno no deben habilitar acciones antes de P0. */
    errno = 0;
    int early = sem_trywait(&st->sem_bomber_turn);
    CHECK(early == -1 && errno == EAGAIN);              /* 3 */
    CHECK(map_load(argv[1], st) == 0);                   /* 4 */
    CHECK(st->map_w >= MIN_DIM);                         /* 5 */
    CHECK(st->map_h >= MIN_DIM);                         /* 6 */
    CHECK(st->map_w <= MAX_X);                          /* 7 */
    CHECK(st->map_h <= MAX_Y);                          /* 8 */
    CHECK(map_in_bounds(st, st->bomber_x, st->bomber_y)); /* 9 */
    CHECK(map_cell_walkable(st->map_grid[st->bomber_y][st->bomber_x])); /* 10 */
    CHECK(st->enemies_alive >= 0 && st->enemies_alive <= NUM_ENEMIES); /* 11 */
    CHECK(map_in_bounds(st, st->map_w-1, st->map_h-1));  /* 12 */
    CHECK(!map_in_bounds(st, -1, 0));                   /* 13 */
    CHECK(!map_in_bounds(st, st->map_w, 0));             /* 14 */
    CHECK(!map_in_bounds(st, 0, st->map_h));             /* 15 */
    CHECK(map_cell_walkable(CELL_FREE));                /* 16 */
    CHECK(!map_cell_walkable(CELL_WALL));               /* 17 */
    CHECK(!map_cell_walkable(CELL_BLOCK));              /* 18 */
    CHECK(!map_cell_walkable(CELL_BOMB));               /* 19 */
    CHECK(!map_cell_walkable(CELL_FIRE));               /* 20 */

    char snapshot[MAX_Y][MAX_X + 1];
    memset(snapshot, 0, sizeof snapshot);
    map_snapshot(st, snapshot);
    CHECK(strlen(snapshot[0]) == (size_t)st->map_w);    /* 21 */

    instr_t inst;
    CHECK(instr_parse("UP", &inst, 0) == 1 && inst.type == INSTR_UP); /* 22 */
    CHECK(instr_parse("BOMB", &inst, 1) == 1 && inst.type == INSTR_BOMB); /* 23 */
    CHECK(instr_parse("BOMB", &inst, 0) == 0);          /* 24 */
    CHECK(instr_parse("SET_PRIORITY 25", &inst, 0) == 1 &&
          inst.type == INSTR_SET_PRIORITY && inst.value == 25); /* 25 */
    int dx = 0, dy = 0;
    CHECK(instr_delta(INSTR_LEFT, &dx, &dy) == 1 && dx == -1 && dy == 0); /* 26 */

cleanup:
    if (st != NULL) shm_destroy(st);
    if (failed) return EXIT_FAILURE;
    printf("OK: test_base %d/26 comprobaciones\n", checks);
    return EXIT_SUCCESS;
}
