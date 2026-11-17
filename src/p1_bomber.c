/*
 * p1_bomber.c - Bomberman: lector, ejecutor y publicador.
 *
 * El hilo inicial del proceso es el executor. Creamos dos hilos POSIX:
 * reader y publisher. Total en P1: 3 hilos, como pide el enunciado.
 *
 * La cola es PRIVADA de P1: mutex + sem_slots + sem_items (pshared=0).
 * El estado compartido usa los mutex del contrato common.h.
 */
#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef P1_QUEUE_CAPACITY
#define P1_QUEUE_CAPACITY 64
#endif

#if P1_QUEUE_CAPACITY < 1
#error "P1_QUEUE_CAPACITY debe ser mayor que cero"
#endif

typedef struct {
    instr_t entries[P1_QUEUE_CAPACITY];
    size_t head, tail, count;
    int finished;               /* el productor terminó (EOF o error) */
    int cancelled;              /* cierre solicitado por el proceso */
    pthread_mutex_t mutex;
    sem_t slots;                /* espacios disponibles */
    sem_t items;                /* elementos disponibles + señal de EOF */
} instruction_queue_t;

typedef struct {
    shared_state_t *st;
    instruction_queue_t queue;
    pthread_mutex_t pos_mutex;  /* posición local, entre executor/publisher */
    int x, y;
    int read_error;
    sem_t publish_request;
    sem_t publish_done;
} bomber_context_t;

static int queue_init(instruction_queue_t *q)
{
    memset(q, 0, sizeof *q);
    int rc = pthread_mutex_init(&q->mutex, NULL);
    if (rc != 0) {
        fprintf(stderr, "[P1] pthread_mutex_init(cola): %s\n", strerror(rc));
        return -1;
    }
    if (sem_init(&q->slots, 0, P1_QUEUE_CAPACITY) == -1) {
        perror("[P1] sem_init(slots)");
        pthread_mutex_destroy(&q->mutex);
        return -1;
    }
    if (sem_init(&q->items, 0, 0) == -1) {
        perror("[P1] sem_init(items)");
        sem_destroy(&q->slots);
        pthread_mutex_destroy(&q->mutex);
        return -1;
    }
    return 0;
}

static void queue_destroy(instruction_queue_t *q)
{
    if (sem_destroy(&q->slots) == -1) perror("[P1] sem_destroy(slots)");
    if (sem_destroy(&q->items) == -1) perror("[P1] sem_destroy(items)");
    int rc = pthread_mutex_destroy(&q->mutex);
    if (rc != 0) fprintf(stderr, "[P1] mutex_destroy(cola): %s\n", strerror(rc));
}

static int queue_push(instruction_queue_t *q, const instr_t *item)
{
    /* Esperar SIN retener el mutex: de lo contrario el consumidor no podría liberar espacio. */
    sem_wait_retry(&q->slots);
    lock(&q->mutex);
    if (q->cancelled) {
        unlock(&q->mutex);
        return -1;
    }
    q->entries[q->tail] = *item;
    q->tail = (q->tail + 1) % P1_QUEUE_CAPACITY;
    q->count++;
    unlock(&q->mutex);
    sem_post_checked(&q->items);
    return 0;
}

static void queue_finish(instruction_queue_t *q)
{
    lock(&q->mutex);
    q->finished = 1;
    unlock(&q->mutex);
    /* Token de fin: permite que queue_pop descubra EOF incluso con cola vacía. */
    sem_post_checked(&q->items);
}

static void queue_cancel(instruction_queue_t *q)
{
    lock(&q->mutex);
    q->cancelled = 1;
    q->finished = 1;
    unlock(&q->mutex);
    /* Despertar tanto al productor bloqueado en cola llena como al consumidor vacío. */
    sem_post_checked(&q->slots);
    sem_post_checked(&q->items);
}

/* 1: instrucción obtenida; 0: EOF/fin; -1: cierre o estado inválido. */
static int queue_pop(instruction_queue_t *q, instr_t *out)
{
    /* EOF persistente: no volver a esperar un token ya consumido. */
    lock(&q->mutex);
    int drained = q->finished && q->count == 0;
    int stopped = q->cancelled;
    unlock(&q->mutex);
    if (stopped) return -1;
    if (drained) return 0;

    sem_wait_retry(&q->items);
    lock(&q->mutex);
    if (q->cancelled) {
        unlock(&q->mutex);
        return -1;
    }
    if (q->count == 0) {
        int done = q->finished;
        unlock(&q->mutex);
        return done ? 0 : -1;
    }
    *out = q->entries[q->head];
    q->head = (q->head + 1) % P1_QUEUE_CAPACITY;
    q->count--;
    unlock(&q->mutex);
    sem_post_checked(&q->slots);
    return 1;
}

static int queue_exhausted(instruction_queue_t *q)
{
    lock(&q->mutex);
    int exhausted = q->finished && q->count == 0;
    unlock(&q->mutex);
    return exhausted;
}

static void publish_done_flag(bomber_context_t *ctx)
{
    if (!queue_exhausted(&ctx->queue)) return;
    lock(&ctx->st->m_pos);
    ctx->st->bomber_moves_done = 1;
    unlock(&ctx->st->m_pos);
}

static void *movement_reader_thread(void *arg)
{
    bomber_context_t *ctx = arg;
    char path[PATH_LEN + 32];
    int n = snprintf(path, sizeof path, "%s/bomber_moves.txt", ctx->st->case_dir);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "[P1] ruta de bomber_moves.txt demasiado larga\n");
        ctx->read_error = 1;
        queue_finish(&ctx->queue);
        return NULL;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror("[P1] fopen(bomber_moves.txt)");
        ctx->read_error = 1;
        queue_finish(&ctx->queue);
        return NULL;
    }

    char line[LINE_LEN];
    int line_no = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        line_no++;
        size_t len = strlen(line);
        if (len > 0 && line[len-1] != '\n' && !feof(f)) {
            /* Descartar el resto de una línea larga: no convertirla en varias órdenes. */
            int c = fgetc(f);
            if (c != '\n' && c != EOF) {
                while ((c = fgetc(f)) != '\n' && c != EOF) {}
                log_line("[P1] línea %d demasiado larga: ignorada", line_no);
                continue;
            }
        }
        instr_t inst;
        int parsed = instr_parse(line, &inst, 1);
        if (parsed != 1) {
            log_line("[P1] línea %d desconocida: ignorada (sin consumir turno)", line_no);
            continue;
        }
        inst.line_no = line_no;
        if (queue_push(&ctx->queue, &inst) == -1) break;
    }
    if (ferror(f)) {
        perror("[P1] fgets(bomber_moves.txt)");
        ctx->read_error = 1;
    }
    if (fclose(f) == EOF) {
        perror("[P1] fclose(bomber_moves.txt)");
        ctx->read_error = 1;
    }
    queue_finish(&ctx->queue);
    return NULL;
}

static void *bomber_publisher_thread(void *arg)
{
    bomber_context_t *ctx = arg;
    for (;;) {
        sem_wait_retry(&ctx->publish_request);
        /* El executor activa el cierre después del último turno. */
        lock(&ctx->queue.mutex);
        int cancelled = ctx->queue.cancelled;
        unlock(&ctx->queue.mutex);
        if (cancelled) break;

        lock(&ctx->pos_mutex);
        int x = ctx->x, y = ctx->y;
        unlock(&ctx->pos_mutex);

        lock(&ctx->st->m_pos);
        ctx->st->bomber_x = x;
        ctx->st->bomber_y = y;
        unlock(&ctx->st->m_pos);
        sem_post_checked(&ctx->publish_done);
    }
    return NULL;
}

static void move_bomber(bomber_context_t *ctx, instr_type_t type)
{
    int dx = 0, dy = 0;
    if (instr_delta(type, &dx, &dy) != 1) return;

    lock(&ctx->pos_mutex);
    int nx = ctx->x + dx, ny = ctx->y + dy;

    /* El único escritor de map_grid durante la simulación es P3. */
    lock(&ctx->st->m_map);
    int valid = map_in_bounds(ctx->st, nx, ny) &&
                map_cell_walkable(ctx->st->map_grid[ny][nx]);

    /* Verificar si hay power-up en la celda destino */
    int has_powerup = 0;
    if (valid) {
        for (int i = 0; i < ctx->st->powerup_count; i++) {
            if (ctx->st->powerup_x[i] == nx && ctx->st->powerup_y[i] == ny) {
                has_powerup = 1;
                break;
            }
        }
    }
    unlock(&ctx->st->m_map);

    if (valid) {
        ctx->x = nx;
        ctx->y = ny;

        /* Si hay power-up, recogerlo */
        if (has_powerup) {
            lock(&ctx->st->m_map);
            for (int i = 0; i < ctx->st->powerup_count; i++) {
                if (ctx->st->powerup_x[i] == nx && ctx->st->powerup_y[i] == ny) {
                    /* Mover el último power-up a esta posición y decrementar count */
                    ctx->st->powerup_x[i] = ctx->st->powerup_x[ctx->st->powerup_count - 1];
                    ctx->st->powerup_y[i] = ctx->st->powerup_y[ctx->st->powerup_count - 1];
                    ctx->st->powerup_count--;
                    ctx->st->bomber_powerup = 1;
                    ctx->st->bomb_range_bonus = 1;  /* +1 de alcance */
                    log_line("[P1] ¡Power-up recogido en (%d,%d)! Alcance de bomba +1", nx, ny);
                    break;
                }
            }
            unlock(&ctx->st->m_map);
        }
    }
    unlock(&ctx->pos_mutex);
    log_line("[P1] movimiento %s -> (%d,%d)%s",
             type == INSTR_UP ? "UP" : type == INSTR_DOWN ? "DOWN" :
             type == INSTR_LEFT ? "LEFT" : "RIGHT",
             valid ? nx : nx-dx, valid ? ny : ny-dy,
             valid ? "" : " [bloqueado; consume turno]");
}

static void execute_instruction(bomber_context_t *ctx, const instr_t *inst)
{
    if (inst->type == INSTR_UP || inst->type == INSTR_DOWN ||
        inst->type == INSTR_LEFT || inst->type == INSTR_RIGHT) {
        move_bomber(ctx, inst->type);
        return;
    }
    if (inst->type == INSTR_BOMB) {
        lock(&ctx->pos_mutex);
        int x = ctx->x, y = ctx->y;
        unlock(&ctx->pos_mutex);
        lock(&ctx->st->m_mailbox);
        ctx->st->bomb_request_x = x;
        ctx->st->bomb_request_y = y;
        ctx->st->bomb_request_active = 1;
        unlock(&ctx->st->m_mailbox);
        log_line("[P1] BOMB solicitado en (%d,%d)", x, y);
    } else if (inst->type == INSTR_SET_PRIORITY) {
        lock(&ctx->st->m_mailbox);
        ctx->st->pending_priority_bomber = inst->value;
        ctx->st->priority_request_active = 1;
        unlock(&ctx->st->m_mailbox);
        log_line("[P1] SET_PRIORITY %d solicitado a P0", inst->value);
    }
}

void p1_main(shared_state_t *st)
{
    bomber_context_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.st = st;
    lock(&st->m_pos);
    ctx.x = st->bomber_x;
    ctx.y = st->bomber_y;
    unlock(&st->m_pos);

    if (queue_init(&ctx.queue) == -1) _exit(EXIT_FAILURE);
    int rc = pthread_mutex_init(&ctx.pos_mutex, NULL);
    if (rc != 0) {
        fprintf(stderr, "[P1] pthread_mutex_init(pos): %s\n", strerror(rc));
        queue_destroy(&ctx.queue);
        _exit(EXIT_FAILURE);
    }
    if (sem_init(&ctx.publish_request, 0, 0) == -1) {
        perror("[P1] sem_init(publish_request)");
        pthread_mutex_destroy(&ctx.pos_mutex);
        queue_destroy(&ctx.queue);
        _exit(EXIT_FAILURE);
    }
    if (sem_init(&ctx.publish_done, 0, 0) == -1) {
        perror("[P1] sem_init(publish_done)");
        sem_destroy(&ctx.publish_request);
        pthread_mutex_destroy(&ctx.pos_mutex);
        queue_destroy(&ctx.queue);
        _exit(EXIT_FAILURE);
    }

    pthread_t reader, publisher;
    rc = pthread_create(&reader, NULL, movement_reader_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "[P1] pthread_create(reader): %s\n", strerror(rc));
        goto init_failure;
    }
    rc = pthread_create(&publisher, NULL, bomber_publisher_thread, &ctx);
    if (rc != 0) {
        fprintf(stderr, "[P1] pthread_create(publisher): %s\n", strerror(rc));
        queue_cancel(&ctx.queue);
        pthread_join(reader, NULL);
        goto init_failure;
    }
    /* P0 espera una señal de que cada hijo creó sus hilos antes del tick 1. */
    sem_post_checked(&st->sem_turn_done);
    log_line("[P1] listo: reader + executor + publisher (3 threads)");

    for (;;) {
        sem_wait_retry(&st->sem_bomber_turn);
        lock(&st->m_state);
        int game_over = st->game_over;
        unlock(&st->m_state);
        if (game_over) break;

        instr_t inst;
        int got = queue_pop(&ctx.queue, &inst);
        if (got == 1) {
            execute_instruction(&ctx, &inst);
        } else if (got == -1) {
            log_line("[P1] cola interrumpida");
        }
        /* Barrera interna: P0 no recibe el ack hasta que la posición está publicada. */
        sem_post_checked(&ctx.publish_request);
        sem_wait_retry(&ctx.publish_done);
        publish_done_flag(&ctx);
        sem_post_checked(&st->sem_turn_done);
    }

    /* Cierre: P0 despertó el turno tras establecer game_over. */
    queue_cancel(&ctx.queue);
    sem_post_checked(&ctx.publish_request);
    pthread_join(reader, NULL);
    pthread_join(publisher, NULL);
    if (ctx.read_error) {
        log_line("[P1] ERROR leyendo bomber_moves.txt");
    }
    sem_destroy(&ctx.publish_done);
    sem_destroy(&ctx.publish_request);
    pthread_mutex_destroy(&ctx.pos_mutex);
    queue_destroy(&ctx.queue);
    log_line("[P1] terminado");
    if (ctx.read_error) _exit(EXIT_FAILURE);
    return;

init_failure:
    sem_destroy(&ctx.publish_done);
    sem_destroy(&ctx.publish_request);
    pthread_mutex_destroy(&ctx.pos_mutex);
    queue_destroy(&ctx.queue);
    _exit(EXIT_FAILURE);
}
