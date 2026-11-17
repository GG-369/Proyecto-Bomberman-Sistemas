/*
 * P2 - enemigos. Ocho threads en un proceso:
 *   controlador (thread inicial), cuatro enemy_thread, tracker,
 *   collision y publisher.
 *
 * Semáforos locales (pshared=0): ordenan fases y la barrera de enemigos.
 * Mutex local: protege posiciones, estado de lectura y solicitudes.
 * Mutex de shared_state_t: protegen el intercambio entre procesos.
 */
#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct enemy_context enemy_context_t;

typedef struct {
    enemy_context_t *ctx;
    int id;                 /* índice 0..3; nombre de archivo id+1 */
    sem_t start;
    int exhausted;          /* EOF + ningún comando pendiente */
    int has_next;
    instr_t next;
    int request_priority;
    int priority_value;
    int read_error;
} enemy_worker_t;

struct enemy_context {
    shared_state_t *st;
    pthread_mutex_t local_mutex; /* protege estado local entre los 8 hilos */
    int x[NUM_ENEMIES], y[NUM_ENEMIES];
    int alive[NUM_ENEMIES]; /* snapshot al inicio de cada turno de P2 */
    int bomber_x, bomber_y;
    int stopping;
    int collision_id;      /* -1 si no hay colisión */
    enemy_worker_t worker[NUM_ENEMIES];
    sem_t ready;           /* cada worker avisa que abrió su archivo */
    sem_t workers_done;    /* barrera de los enemigos vivos */
    sem_t track_go, track_done;
    sem_t collision_go, collision_done;
    sem_t publish_go, publish_done;
};

static int init_sem(sem_t *s, unsigned value, const char *label)
{
    if (sem_init(s, 0, value) == -1) {
        perror(label);
        return -1;
    }
    return 0;
}

/* Descarta instrucciones desconocidas; no consumen turnos de juego. */
static void read_next(enemy_worker_t *w, FILE *file)
{
    char line[LINE_LEN];
    w->has_next = 0;
    if (w->exhausted) return;
    while (fgets(line, sizeof line, file) != NULL) {
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] != '\n' && !feof(file)) {
            /* Una línea larga no equivale a múltiples instrucciones. */
            int c = fgetc(file);
            if (c != '\n' && c != EOF) {
                while ((c = fgetc(file)) != '\n' && c != EOF) {}
                log_line("[P2] enemigo %d: línea demasiado larga, ignorada", w->id + 1);
                continue;
            }
        }
        instr_t inst;
        if (instr_parse(line, &inst, 0) != 1) {
            log_line("[P2] enemigo %d: instrucción desconocida, ignorada", w->id + 1);
            continue;
        }
        w->next = inst;
        w->has_next = 1;
        return;
    }
    if (ferror(file)) {
        perror("[P2] fgets(enemy_moves)");
        w->read_error = 1;
    }
    w->exhausted = 1;
}

static void apply_move(enemy_worker_t *w, instr_type_t type)
{
    enemy_context_t *ctx = w->ctx;
    int dx = 0, dy = 0;
    if (instr_delta(type, &dx, &dy) != 1) return;

    lock(&ctx->local_mutex);
    int nx = ctx->x[w->id] + dx;
    int ny = ctx->y[w->id] + dy;
    /* El único escritor del mapa durante el juego es P3. */
    lock(&ctx->st->m_map);
    int valid = map_in_bounds(ctx->st, nx, ny) &&
                map_cell_walkable(ctx->st->map_grid[ny][nx]);
    unlock(&ctx->st->m_map);
    if (valid) {
        ctx->x[w->id] = nx;
        ctx->y[w->id] = ny;
    }
    unlock(&ctx->local_mutex);
    log_line("[P2] enemigo %d: movimiento (%+d,%+d)%s",
             w->id + 1, dx, dy, valid ? "" : " [bloqueado; consume turno]");
}

static void *enemy_thread(void *arg)
{
    enemy_worker_t *w = arg;
    enemy_context_t *ctx = w->ctx;
    char path[PATH_LEN + 40];
    FILE *file = NULL;

    /* Un enemigo ausente nunca lee instrucciones ni participa en colisiones. */
    if (ctx->alive[w->id]) {
        int n = snprintf(path, sizeof path, "%s/enemy_%d_moves.txt",
                         ctx->st->case_dir, w->id + 1);
        if (n < 0 || (size_t)n >= sizeof path) {
            fprintf(stderr, "[P2] ruta de enemigo %d demasiado larga\n", w->id + 1);
            w->read_error = 1;
            w->exhausted = 1;
        } else {
            file = fopen(path, "r");
            if (file == NULL) {
                perror("[P2] fopen(enemy_moves)");
                w->read_error = 1;
                w->exhausted = 1;
            } else {
                read_next(w, file); /* lookahead sin consumir un turno */
            }
        }
    } else {
        w->exhausted = 1;
    }
    sem_post_checked(&ctx->ready);

    for (;;) {
        sem_wait_retry(&w->start);
        lock(&ctx->local_mutex);
        int stop = ctx->stopping;
        unlock(&ctx->local_mutex);
        if (stop) break;

        if (w->has_next) {
            instr_t inst = w->next;
            if (inst.type == INSTR_SET_PRIORITY) {
                lock(&ctx->local_mutex);
                w->request_priority = 1;
                w->priority_value = inst.value;
                unlock(&ctx->local_mutex);
            } else {
                apply_move(w, inst.type);
            }
            read_next(w, file); /* detectar EOF y preparar el siguiente turno */
        }
        sem_post_checked(&ctx->workers_done);
    }
    if (file != NULL && fclose(file) == EOF) {
        perror("[P2] fclose(enemy_moves)");
        w->read_error = 1;
    }
    return NULL;
}

static void *bomber_tracker_thread(void *arg)
{
    enemy_context_t *ctx = arg;
    for (;;) {
        sem_wait_retry(&ctx->track_go);
        lock(&ctx->local_mutex);
        int stop = ctx->stopping;
        unlock(&ctx->local_mutex);
        if (stop) break;
        lock(&ctx->st->m_pos);
        int x = ctx->st->bomber_x, y = ctx->st->bomber_y;
        unlock(&ctx->st->m_pos);
        lock(&ctx->local_mutex);
        ctx->bomber_x = x;
        ctx->bomber_y = y;
        unlock(&ctx->local_mutex);
        sem_post_checked(&ctx->track_done);
    }
    return NULL;
}

static void *collision_thread(void *arg)
{
    enemy_context_t *ctx = arg;
    for (;;) {
        sem_wait_retry(&ctx->collision_go);
        lock(&ctx->local_mutex);
        if (ctx->stopping) {
            unlock(&ctx->local_mutex);
            break;
        }
        int found = -1;
        for (int i = 0; i < NUM_ENEMIES; i++) {
            if (ctx->alive[i] && ctx->x[i] == ctx->bomber_x &&
                ctx->y[i] == ctx->bomber_y) {
                found = i;
                break; /* un solo evento; P0 quita a lo sumo una vida */
            }
        }
        ctx->collision_id = found;
        unlock(&ctx->local_mutex);
        if (found >= 0) {
            lock(&ctx->st->m_state);
            int tick = ctx->st->global_tick;
            unlock(&ctx->st->m_state);
            lock(&ctx->st->m_events);
            ctx->st->collision_detected = 1;
            ctx->st->collision_enemy_id = found;
            ctx->st->collision_tick = tick;
            unlock(&ctx->st->m_events);
        }
        sem_post_checked(&ctx->collision_done);
    }
    return NULL;
}

static void *enemy_publisher_thread(void *arg)
{
    enemy_context_t *ctx = arg;
    for (;;) {
        sem_wait_retry(&ctx->publish_go);
        lock(&ctx->local_mutex);
        if (ctx->stopping) {
            unlock(&ctx->local_mutex);
            break;
        }
        int x[NUM_ENEMIES], y[NUM_ENEMIES];
        for (int i = 0; i < NUM_ENEMIES; i++) {
            x[i] = ctx->x[i];
            y[i] = ctx->y[i];
        }
        unlock(&ctx->local_mutex);
        lock(&ctx->st->m_pos);
        for (int i = 0; i < NUM_ENEMIES; i++) {
            ctx->st->enemy_x[i] = x[i];
            ctx->st->enemy_y[i] = y[i];
        }
        unlock(&ctx->st->m_pos);
        sem_post_checked(&ctx->publish_done);
    }
    return NULL;
}

static void publish_priority_request(enemy_context_t *ctx)
{
    int chosen = -1, value = 0;
    lock(&ctx->local_mutex);
    /* Recorremos desde el enemigo 4: prevalece el ID más alto. */
    for (int i = NUM_ENEMIES - 1; i >= 0; i--) {
        if (ctx->alive[i] && ctx->worker[i].request_priority) {
            chosen = i;
            value = ctx->worker[i].priority_value;
            break;
        }
    }
    unlock(&ctx->local_mutex);
    if (chosen >= 0) {
        lock(&ctx->st->m_mailbox);
        ctx->st->pending_priority_enemy = value;
        ctx->st->enemy_priority_request_active = 1;
        unlock(&ctx->st->m_mailbox);
        log_line("[P2] enemigo %d solicita SET_PRIORITY %d", chosen + 1, value);
    }
}

static void publish_exhausted(enemy_context_t *ctx)
{
    int exhausted = 1;
    lock(&ctx->local_mutex);
    for (int i = 0; i < NUM_ENEMIES; i++) {
        if (ctx->alive[i] && !ctx->worker[i].exhausted) exhausted = 0;
    }
    unlock(&ctx->local_mutex);
    if (exhausted) {
        lock(&ctx->st->m_pos);
        ctx->st->enemy_moves_done = 1;
        unlock(&ctx->st->m_pos);
    }
}

static void stop_workers(enemy_context_t *ctx)
{
    lock(&ctx->local_mutex);
    ctx->stopping = 1;
    unlock(&ctx->local_mutex);
    for (int i = 0; i < NUM_ENEMIES; i++)
        sem_post_checked(&ctx->worker[i].start);
    sem_post_checked(&ctx->track_go);
    sem_post_checked(&ctx->collision_go);
    sem_post_checked(&ctx->publish_go);
}

/* Cada semáforo creado se destruye únicamente después de pthread_join. */
void p2_main(shared_state_t *st)
{
    enemy_context_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.st = st;
    ctx.collision_id = -1;
    lock(&st->m_pos);
    for (int i = 0; i < NUM_ENEMIES; i++) {
        ctx.x[i] = st->enemy_x[i];
        ctx.y[i] = st->enemy_y[i];
    }
    unlock(&st->m_pos);
    lock(&st->m_state);
    for (int i = 0; i < NUM_ENEMIES; i++) ctx.alive[i] = st->enemy_alive[i];
    unlock(&st->m_state);

    int rc = pthread_mutex_init(&ctx.local_mutex, NULL);
    if (rc) {
        fprintf(stderr, "[P2] mutex_init: %s\n", strerror(rc));
        _exit(EXIT_FAILURE);
    }
    sem_t *all[] = {
        &ctx.ready, &ctx.workers_done,
        &ctx.track_go, &ctx.track_done,
        &ctx.collision_go, &ctx.collision_done,
        &ctx.publish_go, &ctx.publish_done,
        &ctx.worker[0].start, &ctx.worker[1].start,
        &ctx.worker[2].start, &ctx.worker[3].start
    };
    const size_t ns = sizeof all / sizeof all[0];
    for (size_t i = 0; i < ns; i++) {
        if (init_sem(all[i], 0, "[P2] sem_init") == -1) {
            while (i > 0) sem_destroy(all[--i]);
            pthread_mutex_destroy(&ctx.local_mutex);
            _exit(EXIT_FAILURE);
        }
    }
    pthread_t threads[NUM_ENEMIES + 3];
    int created = 0;
    for (int i = 0; i < NUM_ENEMIES; i++) {
        ctx.worker[i].ctx = &ctx;
        ctx.worker[i].id = i;
        rc = pthread_create(&threads[created], NULL, enemy_thread, &ctx.worker[i]);
        if (rc) goto create_failure;
        created++;
    }
    rc = pthread_create(&threads[created], NULL, bomber_tracker_thread, &ctx);
    if (rc) goto create_failure;
    created++;
    rc = pthread_create(&threads[created], NULL, collision_thread, &ctx);
    if (rc) goto create_failure;
    created++;
    rc = pthread_create(&threads[created], NULL, enemy_publisher_thread, &ctx);
    if (rc) goto create_failure;
    created++;

    /* Asegurar que los cuatro workers tienen lista su entrada antes del primer turno. */
    for (int i = 0; i < NUM_ENEMIES; i++) sem_wait_retry(&ctx.ready);
    publish_exhausted(&ctx);
    sem_post_checked(&st->sem_turn_done); /* barrera inicial de P0 */
    log_line("[P2] listo: controller + 4 enemy + tracker + collision + publisher (8 threads)");

    for (;;) {
        sem_wait_retry(&st->sem_enemy_turn);
        lock(&st->m_state);
        int game_over = st->game_over;
        for (int i = 0; i < NUM_ENEMIES; i++) ctx.alive[i] = st->enemy_alive[i];
        unlock(&st->m_state);
        if (game_over) break;

        int n_active = 0;
        lock(&ctx.local_mutex);
        for (int i = 0; i < NUM_ENEMIES; i++) ctx.worker[i].request_priority = 0;
        for (int i = 0; i < NUM_ENEMIES; i++) {
            if (ctx.alive[i]) {
                n_active++;
                sem_post_checked(&ctx.worker[i].start);
            }
        }
        unlock(&ctx.local_mutex);
        for (int i = 0; i < n_active; i++) sem_wait_retry(&ctx.workers_done);

        publish_priority_request(&ctx);
        sem_post_checked(&ctx.track_go);
        sem_wait_retry(&ctx.track_done);
        sem_post_checked(&ctx.collision_go);
        sem_wait_retry(&ctx.collision_done);
        sem_post_checked(&ctx.publish_go);
        sem_wait_retry(&ctx.publish_done);
        publish_exhausted(&ctx);
        sem_post_checked(&st->sem_turn_done);
    }

    stop_workers(&ctx);
    for (int i = 0; i < created; i++) {
        rc = pthread_join(threads[i], NULL);
        if (rc) fprintf(stderr, "[P2] pthread_join: %s\n", strerror(rc));
    }
    int read_error = 0;
    for (int i = 0; i < NUM_ENEMIES; i++) read_error |= ctx.worker[i].read_error;
    for (size_t i = 0; i < ns; i++) sem_destroy(all[i]);
    pthread_mutex_destroy(&ctx.local_mutex);
    log_line("[P2] terminado%s", read_error ? " con error de lectura" : "");
    if (read_error) _exit(EXIT_FAILURE);
    return;

create_failure:
    fprintf(stderr, "[P2] pthread_create: %s\n", strerror(rc));
    stop_workers(&ctx);
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
    for (size_t i = 0; i < ns; i++) sem_destroy(all[i]);
    pthread_mutex_destroy(&ctx.local_mutex);
    _exit(EXIT_FAILURE);
}
