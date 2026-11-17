/*
 * common.h - Definiciones compartidas por todos los procesos del Bomberman concurrente.
 *
 * Contiene:
 *   - Constantes del juego (sección 8 del enunciado).
 *   - La struct shared_state_t que vive en memoria compartida POSIX (sección 13).
 *   - Los prototipos de cada módulo.
 *
 * ORDEN GLOBAL DE ADQUISICIÓN DE MUTEX (evita deadlocks por espera circular):
 *
 *      m_pos  ->  m_map  ->  m_mailbox  ->  m_events  ->  m_state
 *
 * Un hilo que necesite varios mutex SIEMPRE los toma en ese orden y los
 * suelta en orden inverso. Ejemplo: explosion_thread toma m_pos (lee
 * posiciones), luego m_map (escribe fuego) y luego m_events (publica daño).
 * Como nadie toma un mutex "anterior" mientras tiene uno "posterior", no
 * puede formarse un ciclo de espera.
 */
#ifndef COMMON_H
#define COMMON_H

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* Constantes                                                          */
/* ------------------------------------------------------------------ */
#define SHM_NAME            "/bomberman_p2_shm"

#define MAX_X               32      /* columnas máximas del mapa */
#define MAX_Y               32      /* filas máximas del mapa    */
#define MIN_DIM             3       /* mapa mínimo 3x3           */
#define NUM_ENEMIES         4

#ifndef BOMB_TIMER                  /* se puede cambiar al compilar: -DBOMB_TIMER=6 */
#define BOMB_TIMER          3       /* ticks entre registro y explosión */
#endif
#define BOMB_RANGE          2       /* alcance del fuego por dirección  */
#define MAX_BOMBS           2       /* bombas activas simultáneas       */

#define INITIAL_LIVES       3
#define SCORE_PER_BLOCK     10
#define SCORE_PER_ENEMY     100

#define PRIORITY_P0         50      /* P0 está por encima de todos       */
#define PRIORITY_MIN        1       /* rango válido para SET_PRIORITY    */
#define PRIORITY_MAX        49
#define DEFAULT_PRIO_BOMBER 20
#define DEFAULT_PRIO_ENEMY  30
#define DEFAULT_MAX_TICKS   50

#define PATH_LEN            512
#define LINE_LEN            128

/* Símbolos del mapa (sección 7) */
#define CELL_WALL       'X'   /* pared indestructible        */
#define CELL_BLOCK      '#'   /* bloque destructible         */
#define CELL_FREE       'O'   /* camino libre                */
#define CELL_BOMB       'Q'   /* bomba activa (lo escribe P3) */
#define CELL_FIRE       '*'   /* fuego, dura 1 tick (P3)      */
#define CELL_POWERUP    '+'   /* power-up: mejora bomba (P0 carga, P1 recoge) */

/* ------------------------------------------------------------------ */
/* Tipos                                                               */
/* ------------------------------------------------------------------ */
typedef enum { TURN_NONE = 0, TURN_BOMBER = 1, TURN_ENEMY = 2 } turn_t;

typedef enum {
    RESULT_NONE = 0,
    RESULT_VICTORY,       /* todos los enemigos eliminados          */
    RESULT_DEFEAT,        /* Bomberman sin vidas                    */
    RESULT_MAX_TICKS,     /* se alcanzó max_ticks                   */
    RESULT_NO_MOVES,      /* archivos agotados y sin bombas activas */
    RESULT_INTERRUPTED,   /* Ctrl+C (SIGINT)                        */
    RESULT_ERROR          /* un proceso hijo terminó inesperadamente */
} game_result_t;

typedef enum {
    INSTR_UP, INSTR_DOWN, INSTR_LEFT, INSTR_RIGHT,
    INSTR_BOMB, INSTR_SET_PRIORITY
} instr_type_t;

typedef struct {
    instr_type_t type;
    int          value;   /* solo para SET_PRIORITY */
    int          line_no; /* línea del archivo (para el log) */
} instr_t;

/*
 * Memoria compartida. Cada bloque indica QUIÉN ESCRIBE y QUÉ MUTEX lo
 * protege. Regla: un único proceso escritor por variable; en buzones y
 * eventos el receptor solo limpia la bandera.
 */
typedef struct {
    /* --- Semáforos entre procesos (sem_init con pshared = 1) --- */
    sem_t sem_bomber_turn;   /* P0 -> P1: "te toca"                     */
    sem_t sem_enemy_turn;    /* P0 -> P2: "te toca"                     */
    sem_t sem_bomb_turn;     /* P0 -> P3: "fase de bombas"              */
    sem_t sem_turn_done;     /* P1/P2/P3 -> P0: "terminé mi turno"      */
    sem_t sem_render;        /* P0 -> P4: "dibuja el tick"              */
    sem_t sem_render_done;   /* P4 -> P0: "ya dibujé"                   */

    /* --- Mutex entre procesos (PTHREAD_PROCESS_SHARED) --- */
    pthread_mutex_t m_pos;      /* bomber_x/y, enemy_x/y, *_moves_done   */
    pthread_mutex_t m_map;      /* map_grid, active_bombs, blocks, bomb_*[] */
    pthread_mutex_t m_mailbox;  /* buzones de bomba y de prioridad       */
    pthread_mutex_t m_events;   /* eventos de colisión y de daño         */
    pthread_mutex_t m_state;    /* tick, prioridades, vidas, game_over   */

    /* --- Escribe P0 (m_state) --- */
    int global_tick;
    int max_ticks;
    int game_over;
    int game_result;                 /* game_result_t */
    int bomber_lives;
    int score;
    int prioridad_bomber;
    int prioridad_enemy;
    int turno_actual;                /* turn_t */
    int enemy_alive[NUM_ENEMIES];
    int enemies_alive;
    int render_enabled;              /* 1 si P4 está activo */
    char turn_reason[64];            /* por qué ganó el turno (log y P4) */
    char last_events[160];           /* eventos del último cierre (P4)  */

    /* --- Escribe P1 (m_pos) --- */
    int bomber_x, bomber_y;
    int bomber_moves_done;

    /* --- Buzón de prioridad de P1: escribe P1, P0 limpia (m_mailbox) --- */
    int pending_priority_bomber;
    int priority_request_active;

    /* --- Buzón de bomba: escribe P1, P3 limpia (m_mailbox) --- */
    int bomb_request_x, bomb_request_y;
    int bomb_request_active;

    /* --- Escribe P2 (m_pos) --- */
    int enemy_x[NUM_ENEMIES], enemy_y[NUM_ENEMIES];
    int enemy_moves_done;

    /* --- Evento de colisión: escribe P2, P0 limpia (m_events) --- */
    int collision_detected;
    int collision_tick;
    int collision_enemy_id;          /* 0..3 */

    /* --- Buzón de prioridad de P2: escribe P2, P0 limpia (m_mailbox) --- */
    int pending_priority_enemy;
    int enemy_priority_request_active;

    /* --- Mapa: P0 al iniciar, luego solo P3 (m_map) --- */
    int  map_w, map_h;
    char map_grid[MAX_Y][MAX_X];
    int  active_bombs;
    int  blocks_destroyed;
    /* Tabla de bombas publicada para P4 (bonus). bomb_timer == 0: vacío */
    int  bomb_x[MAX_BOMBS], bomb_y[MAX_BOMBS], bomb_timer[MAX_BOMBS];
    int  bomb_powerup[MAX_BOMBS];  /* 1 si la bomba tiene power-up de alcance */

    /* --- Eventos de daño: escribe P3, P0 limpia (m_events) --- */
    int blast_hit_bomber;
    int blast_tick;
    int blast_hit_enemy[NUM_ENEMIES];

    /* --- Configuración de solo lectura (P0 antes del fork) --- */
    char case_dir[PATH_LEN];
    int  tick_ms;                    /* pausa por tick (0 = sin pausa) */
    int  aging_ticks;                /* aging opcional (0 = desactivado) */
    int  initial_enemies;            /* enemigos presentes en map.txt   */

    /* --- Power-ups: P0 carga del mapa, P1 recoge, P3 usa en explosión --- */
    int  powerup_x[MAX_Y], powerup_y[MAX_Y];  /* posiciones de power-ups en el mapa */
    int  powerup_count;                       /* número de power-ups activos */
    int  bomber_powerup;                      /* 1 si Bomberman tiene power-up activo */
    int  bomb_range_bonus;                    /* bonificador de alcance (0 o 1) */
} shared_state_t;

/* ------------------------------------------------------------------ */
/* Prototipos                                                          */
/* ------------------------------------------------------------------ */
/* util.c */
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sleep_ms(int ms);
const char *result_name(int result);

/* shm.c */
shared_state_t *shm_create(void);
int  shm_init_sync(shared_state_t *st);
void shm_destroy(shared_state_t *st);
void shm_detach(shared_state_t *st);
void lock(pthread_mutex_t *m);
void unlock(pthread_mutex_t *m);
void sem_wait_retry(sem_t *s);
void sem_post_checked(sem_t *s);

/* map.c */
int  map_load(const char *path, shared_state_t *st);
int  map_cell_walkable(char c);
int  map_in_bounds(const shared_state_t *st, int x, int y);
void map_snapshot(const shared_state_t *st, char out[MAX_Y][MAX_X + 1]);

/* instr.c */
int  instr_parse(const char *line, instr_t *out, int allow_bomb);
const char *instr_name(const instr_t *in, char *buf, size_t len);
int  instr_delta(instr_type_t t, int *dx, int *dy);

/* procesos */
int  p0_run(shared_state_t *st, pid_t children[], int n_children);
void p1_main(shared_state_t *st);
void p2_main(shared_state_t *st);
void p3_main(shared_state_t *st);
void p4_main(shared_state_t *st, int tty_fd);

#endif /* COMMON_H */
