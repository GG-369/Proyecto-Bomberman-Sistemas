/*
 * p3_bomb.c - P3 bomb_process.
 *
 * Tres hilos:
 *   1. controller_thread: espera sem_bomb_turn, lee buzón de bomba (m_mailbox),
 *      registra bomba si hay slot libre y celda válida, responde sem_turn_done.
 *   2. fuse_thread: espera semáforo privado, decrementa timers; cuando llega a 0,
 *      encola la bomba para explosión.
 *   3. explosion_thread: procesa cola de explosión, propaga fuego (range 2),
 *      reacciones en cadena, daño a Bomberman/enemigos, destruye bloques.
 *
 * Comunicación solo por memoria compartida + semáforos.
 * Orden de mutex global: m_pos -> m_map -> m_mailbox -> m_events -> m_state
 *
 * CORRECCIONES:
 * - Los hilos internos de P3 se coordinan con semáforos PRIVADOS (sem_init pshared=0).
 *   fuse_thread NO usa sem_turn_done (ese es el canal P1/P2/P3 -> P0).
 * - Orden de fase de bombas (sección 8.2):
 *     1. Limpiar fuego del tick anterior
 *     2. Avanzar timers
 *     3. Explotar las que llegan a 0 (con reacción en cadena)
 *     4. Registrar la solicitud pendiente (y marcar Q en map_grid)
 * - Solo controller_thread hace sem_post(sem_turn_done) una vez al final de la fase.
 * - Validar que la celda no tenga ya bomba y sea transitable; marcar con Q.
 */
#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Máximo de bombas que pueden estar en cola de explosión simultáneamente */
#define MAX_EXPLOSION_QUEUE 8

typedef struct {
    int x, y;
} explosion_job_t;

static shared_state_t *g_st;
static pthread_t th_controller, th_fuse, th_explosion;

/* Semáforos PRIVADOS de P3 (pshared = 0) para coordinación interna */
static sem_t sem_fuse_go;          /* controller -> fuse: "empieza fase fuse" */
static sem_t sem_fuse_done;        /* fuse -> controller: "terminé fuse" */
static sem_t sem_explosion_go;     /* controller -> explosion: "empieza fase explosión" */
static sem_t sem_explosion_done;   /* explosion -> controller: "terminé explosión" */
static sem_t explosion_queue_sem;  /* cuenta trabajos en cola de explosión */

static explosion_job_t explosion_queue[MAX_EXPLOSION_QUEUE];
static int queue_head = 0, queue_tail = 0;
static atomic_int g_stop = 0;

/* ------------------------------------------------------------------ */
/* Utilidades de cola de explosión (protegida por m_map)              */
/* ------------------------------------------------------------------ */
static int enqueue_explosion(int x, int y)
{
    int next = (queue_tail + 1) % MAX_EXPLOSION_QUEUE;
    if (next == queue_head)
        return -1;  /* cola llena (no debería pasar con MAX_BOMBS=2) */
    explosion_queue[queue_tail].x = x;
    explosion_queue[queue_tail].y = y;
    queue_tail = next;
    return 0;
}

static int dequeue_explosion(int *x, int *y)
{
    if (queue_head == queue_tail)
        return -1;  /* cola vacía */
    *x = explosion_queue[queue_head].x;
    *y = explosion_queue[queue_head].y;
    queue_head = (queue_head + 1) % MAX_EXPLOSION_QUEUE;
    return 0;
}

/* ------------------------------------------------------------------ */
/* controller_thread: registra bomba solicitada por P1                */
/* ORDEN DE FASE (sección 8.2):
 *   1. Limpiar fuego del tick anterior
 *   2. Avanzar timers (fuse_thread)
 *   3. Explotar las que llegan a 0 (explosion_thread)
 *   4. Registrar solicitud pendiente (marcar Q)
 * Solo controller_thread hace sem_post(sem_turn_done) al final.
 */
/* ------------------------------------------------------------------ */
static void *controller_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sem_wait_retry(&g_st->sem_bomb_turn);

        /* Verificar si el juego terminó (P0 marcó game_over) */
        lock(&g_st->m_state);
        int over = g_st->game_over;
        unlock(&g_st->m_state);
        if (over) {
            atomic_store(&g_stop, 1);
            /* Despertar a fuse y explosion para que salgan limpiamente */
            sem_post_checked(&sem_fuse_go);
            sem_post_checked(&sem_explosion_go);
            break;
        }

        /* --- FASE 1: Limpiar fuego del tick anterior --- */
        lock(&g_st->m_map);
        for (int y = 0; y < g_st->map_h; y++) {
            for (int x = 0; x < g_st->map_w; x++) {
                if (g_st->map_grid[y][x] == CELL_FIRE)
                    g_st->map_grid[y][x] = CELL_FREE;
            }
        }
        unlock(&g_st->m_map);

        /* --- FASE 2: Avanzar timers (despertar fuse_thread) --- */
        sem_post_checked(&sem_fuse_go);
        sem_wait_retry(&sem_fuse_done);

        /* --- FASE 3: Explotar las que llegaron a 0 (despertar explosion_thread) ---
 * Siempre se hace post a sem_explosion_go; explosion_thread maneja cola vacía
 * (el while no itera) y responde con sem_explosion_done. Así evitamos deadlock
 * cuando no hay bombas para explotar (ej. tick 1).
 * queue_head/queue_tail se modifican en fuse_thread (con m_map tomado) y se leen
 * aquí después de sem_wait(&sem_fuse_done) -> no hay carrera.
 */
        sem_post_checked(&sem_explosion_go);
        sem_wait_retry(&sem_explosion_done);

        /* --- FASE 4: Registrar solicitud pendiente --- */
        /* Leer buzón de bomba (P1 escribe, P3 limpia) */
        lock(&g_st->m_mailbox);
        int has_request = g_st->bomb_request_active;
        int req_x = g_st->bomb_request_x;
        int req_y = g_st->bomb_request_y;
        if (has_request)
            g_st->bomb_request_active = 0;  /* consumimos la petición */
        unlock(&g_st->m_mailbox);

        if (has_request) {
            /* Validaciones: slot libre, coordenadas válidas, celda transitable, sin bomba existente */
            lock(&g_st->m_map);
            int slot = -1;
            for (int i = 0; i < MAX_BOMBS; i++) {
                if (g_st->bomb_timer[i] == 0) {
                    slot = i;
                    break;
                }
            }
            int valid = 0;
            if (slot != -1 && map_in_bounds(g_st, req_x, req_y)) {
                char cell = g_st->map_grid[req_y][req_x];
                int has_bomb = 0;
                for (int i = 0; i < MAX_BOMBS; i++) {
                    if (g_st->bomb_timer[i] > 0 &&
                        g_st->bomb_x[i] == req_x && g_st->bomb_y[i] == req_y) {
                        has_bomb = 1;
                        break;
                    }
                }
                /* Celda debe ser camino libre (O) y no tener bomba activa */
                if (cell == CELL_FREE && !has_bomb) {
                    valid = 1;
                }
            }
            if (valid) {
                /* Registrar bomba */
                g_st->bomb_x[slot] = req_x;
                g_st->bomb_y[slot] = req_y;
                g_st->bomb_timer[slot] = BOMB_TIMER;
                /* Si Bomberman tiene power-up, esta bomba tiene alcance extra */
                if (g_st->bomber_powerup) {
                    g_st->bomb_powerup[slot] = 1;
                    g_st->bomber_powerup = 0;
                    g_st->bomb_range_bonus = 0;  /* se consume al colocar la bomba */
                    log_line("[P3] bomba CON POWER-UP registrada en slot %d: (%d,%d) timer=%d range=%d",
                             slot, req_x, req_y, BOMB_TIMER, BOMB_RANGE + 1);
                } else {
                    g_st->bomb_powerup[slot] = 0;
                    log_line("[P3] bomba registrada en slot %d: (%d,%d) timer=%d",
                             slot, req_x, req_y, BOMB_TIMER);
                }
                g_st->active_bombs++;
                /* Marcar Q en map_grid */
                g_st->map_grid[req_y][req_x] = CELL_BOMB;
            } else {
                log_line("[P3] bomba RECHAZADA: %s (slots=%d/%d)",
                         slot == -1 ? "sin slots libres" :
                         (!map_in_bounds(g_st, req_x, req_y) ? "coordenadas inválidas" :
                          (g_st->map_grid[req_y][req_x] != CELL_FREE ? "celda no transitable" : "ya hay bomba")),
                         g_st->active_bombs, MAX_BOMBS);
            }
            unlock(&g_st->m_map);
        }

        /* Avisar a P0 que terminó la fase de bombas (UNA SOLA VEZ por tick) */
        sem_post_checked(&g_st->sem_turn_done);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* fuse_thread: decrementa timers cada tick, encola las que explotan  */
/* Espera sem_fuse_go (privado), no sem_turn_done (compartido con P0) */
/* ------------------------------------------------------------------ */
static void *fuse_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sem_wait_retry(&sem_fuse_go);
        if (atomic_load(&g_stop))
            break;

        lock(&g_st->m_map);
        for (int i = 0; i < MAX_BOMBS; i++) {
            if (g_st->bomb_timer[i] > 0) {
                g_st->bomb_timer[i]--;
                if (g_st->bomb_timer[i] == 0) {
                    /* ¡Explota en este tick! Encolar para explosion_thread */
                    enqueue_explosion(g_st->bomb_x[i], g_st->bomb_y[i]);
                    log_line("[P3] fuse: bomba en (%d,%d) timer=0 -> encolada para explosión",
                             g_st->bomb_x[i], g_st->bomb_y[i]);
                }
            }
        }
        unlock(&g_st->m_map);

        /* Avisar a controller_thread que terminó fuse */
        sem_post_checked(&sem_fuse_done);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Propagación de fuego en una dirección                              */
/* Retorna 1 si el fuego se detiene (pared/bloque), 0 si sigue        */
/* ------------------------------------------------------------------ */
static int propagate_fire(int x, int y, int dx, int dy, int range, int *hit_bomber, int hit_enemy[NUM_ENEMIES])
{
    for (int step = 1; step <= range; step++) {
        int nx = x + dx * step;
        int ny = y + dy * step;

        if (!map_in_bounds(g_st, nx, ny))
            return 1;  /* fuera de mapa -> parar */

        lock(&g_st->m_map);
        char cell = g_st->map_grid[ny][nx];
        unlock(&g_st->m_map);

        if (cell == CELL_WALL) {
            return 1;  /* pared indestructible frena */
        }
        if (cell == CELL_BLOCK) {
            /* Bloque destructible: se destruye (pasa a O) y frena el fuego */
            lock(&g_st->m_map);
            g_st->map_grid[ny][nx] = CELL_FREE;
            g_st->blocks_destroyed++;
            unlock(&g_st->m_map);
            log_line("[P3] bloque destruido en (%d,%d)", nx, ny);
            return 1;  /* frena el fuego de TODAS las bombas de este tick */
        }

        /* Marcar fuego en la celda (visual, dura 1 tick) */
        lock(&g_st->m_map);
        g_st->map_grid[ny][nx] = CELL_FIRE;
        unlock(&g_st->m_map);

        /* Verificar daño a Bomberman (m_pos + m_events) */
        lock(&g_st->m_pos);
        int bx = g_st->bomber_x, by = g_st->bomber_y;
        unlock(&g_st->m_pos);
        if (bx == nx && by == ny) {
            lock(&g_st->m_events);
            g_st->blast_hit_bomber = 1;
            g_st->blast_tick = g_st->global_tick;
            unlock(&g_st->m_events);
            *hit_bomber = 1;
            log_line("[P3] fuego alcanza a Bomberman en (%d,%d)", nx, ny);
        }

        /* Verificar daño a enemigos (m_pos + m_events) */
        lock(&g_st->m_pos);
        for (int i = 0; i < NUM_ENEMIES; i++) {
            if (g_st->enemy_alive[i] &&
                g_st->enemy_x[i] == nx && g_st->enemy_y[i] == ny) {
                lock(&g_st->m_events);
                g_st->blast_hit_enemy[i] = 1;
                unlock(&g_st->m_events);
                hit_enemy[i] = 1;
                log_line("[P3] fuego alcanza a enemigo %d en (%d,%d)", i + 1, nx, ny);
            }
        }
        unlock(&g_st->m_pos);

        /* Reacción en cadena: si hay bomba activa en esta celda, explotar AHORA */
        lock(&g_st->m_map);
        int chain = 0;
        for (int i = 0; i < MAX_BOMBS; i++) {
            if (g_st->bomb_timer[i] > 0 &&
                g_st->bomb_x[i] == nx && g_st->bomb_y[i] == ny) {
                g_st->bomb_timer[i] = 0;  /* forzar explosión inmediata */
                enqueue_explosion(nx, ny);
                chain = 1;
                log_line("[P3] REACCIÓN EN CADENA: bomba en (%d,%d) detonada por fuego", nx, ny);
                break;
            }
        }
        unlock(&g_st->m_map);
        if (chain) {
            sem_post_checked(&explosion_queue_sem);
        }
    }
    return 0;  /* fuego llegó al final del rango sin chocar */
}

/* ------------------------------------------------------------------ */
/* explosion_thread: procesa cola, propaga fuego, limpia              */
/* Espera sem_explosion_go (privado)                                  */
/* ------------------------------------------------------------------ */
static void *explosion_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sem_wait_retry(&sem_explosion_go);
        if (atomic_load(&g_stop))
            break;

        int ex, ey;
        while (dequeue_explosion(&ex, &ey) == 0) {
            int hit_bomber = 0;
            int hit_enemy[NUM_ENEMIES] = {0};

            /* Encontrar el slot de esta bomba para saber si tiene power-up */
            int bomb_slot = -1;
            for (int i = 0; i < MAX_BOMBS; i++) {
                if (g_st->bomb_x[i] == ex && g_st->bomb_y[i] == ey && g_st->bomb_timer[i] == 0) {
                    bomb_slot = i;
                    break;
                }
            }
            int has_powerup = (bomb_slot >= 0) ? g_st->bomb_powerup[bomb_slot] : 0;
            int range = BOMB_RANGE + has_powerup;
            log_line("[P3] explosión en (%d,%d) range=%d (base=%d powerup=%d)", ex, ey, range, BOMB_RANGE, has_powerup);

            /* Centro de la explosión */
            lock(&g_st->m_map);
            g_st->map_grid[ey][ex] = CELL_FIRE;
            unlock(&g_st->m_map);

            /* Verificar daño en el centro */
            lock(&g_st->m_pos);
            if (g_st->bomber_x == ex && g_st->bomber_y == ey) {
                lock(&g_st->m_events);
                g_st->blast_hit_bomber = 1;
                g_st->blast_tick = g_st->global_tick;
                unlock(&g_st->m_events);
                hit_bomber = 1;
            }
            for (int i = 0; i < NUM_ENEMIES; i++) {
                if (g_st->enemy_alive[i] &&
                    g_st->enemy_x[i] == ex && g_st->enemy_y[i] == ey) {
                    lock(&g_st->m_events);
                    g_st->blast_hit_enemy[i] = 1;
                    unlock(&g_st->m_events);
                    hit_enemy[i] = 1;
                }
            }
            unlock(&g_st->m_pos);

            /* Propagar en 4 direcciones */
            propagate_fire(ex, ey, 1, 0, range, &hit_bomber, hit_enemy);   /* derecha */
            propagate_fire(ex, ey, -1, 0, range, &hit_bomber, hit_enemy);  /* izquierda */
            propagate_fire(ex, ey, 0, 1, range, &hit_bomber, hit_enemy);   /* abajo */
            propagate_fire(ex, ey, 0, -1, range, &hit_bomber, hit_enemy);  /* arriba */

            /* Limpiar bomba de la tabla (slot libre) y quitar Q del mapa */
            lock(&g_st->m_map);
            for (int i = 0; i < MAX_BOMBS; i++) {
                if (g_st->bomb_x[i] == ex && g_st->bomb_y[i] == ey && g_st->bomb_timer[i] == 0) {
                    g_st->bomb_x[i] = 0;
                    g_st->bomb_y[i] = 0;
                    g_st->active_bombs--;
                    /* La celda del centro queda como FIRE (se limpiará en fase 1 del próximo tick) */
                    break;
                }
            }
            unlock(&g_st->m_map);
        }

        /* Avisar a controller_thread que terminó explosión */
        sem_post_checked(&sem_explosion_done);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Arranque y parada                                                  */
/* ------------------------------------------------------------------ */
void p3_main(shared_state_t *st)
{
    g_st = st;

    /* Inicializar semáforos PRIVADOS de P3 (pshared = 0) */
    if (sem_init(&sem_fuse_go, 0, 0) == -1 ||
        sem_init(&sem_fuse_done, 0, 0) == -1 ||
        sem_init(&sem_explosion_go, 0, 0) == -1 ||
        sem_init(&sem_explosion_done, 0, 0) == -1 ||
        sem_init(&explosion_queue_sem, 0, 0) == -1) {
        perror("sem_init (P3 privados)");
        _exit(EXIT_FAILURE);
    }

    /* Avisar a P0 que P3 está listo (barrera de arranque) */
    sem_post_checked(&st->sem_turn_done);

    /* Crear hilos */
    pthread_create(&th_controller, NULL, controller_thread, NULL);
    pthread_create(&th_fuse, NULL, fuse_thread, NULL);
    pthread_create(&th_explosion, NULL, explosion_thread, NULL);

    /* Esperar a que controller_thread termine (cuando game_over) */
    pthread_join(th_controller, NULL);

    /* Parar hilos fuse y explosion (controller ya los despertó con sem_post) */
    pthread_join(th_fuse, NULL);
    pthread_join(th_explosion, NULL);

    sem_destroy(&sem_fuse_go);
    sem_destroy(&sem_fuse_done);
    sem_destroy(&sem_explosion_go);
    sem_destroy(&sem_explosion_done);
    sem_destroy(&explosion_queue_sem);
}