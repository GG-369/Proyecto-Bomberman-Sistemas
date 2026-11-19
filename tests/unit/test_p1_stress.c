/*
 * Prueba 12 - Estrés de la cola real de P1 (5000 instrucciones, 4 slots).
 * Incluimos p1_bomber.c deliberadamente para comprobar sus funciones static;
 * no duplicamos una implementación diferente de la cola.
 * Compilar con -DP1_QUEUE_CAPACITY=4.
 */
#include "../../src/p1_bomber.c"

#include <assert.h>
#include <time.h>

#define EXPECTED_INSTRUCTIONS 5000

typedef struct {
    instruction_queue_t queue;
    const char *file;
    int produced;
    int consumed;
} stress_context_t;

static void *stress_reader(void *arg)
{
    stress_context_t *ctx = arg;
    FILE *f = fopen(ctx->file, "r");
    if (!f) { perror("fopen(stress)"); abort(); }

    char line[LINE_LEN];
    int line_no = 0;
    while (fgets(line, sizeof line, f)) {
        instr_t inst;
        ++line_no;
        assert(instr_parse(line, &inst, 1) == 1);
        inst.line_no = line_no;
        assert(queue_push(&ctx->queue, &inst) == 0);
    }
    assert(!ferror(f));
    assert(fclose(f) == 0);
    ctx->produced = line_no;
    queue_finish(&ctx->queue);
    return NULL;
}

static void *stress_executor(void *arg)
{
    stress_context_t *ctx = arg;
    for (int i = 1; i <= EXPECTED_INSTRUCTIONS; i++) {
        instr_t inst;
        assert(queue_pop(&ctx->queue, &inst) == 1);
        assert(inst.line_no == i); /* Comprueba FIFO, sin pérdidas ni duplicados. */
        assert(inst.type == (i % 2 ? INSTR_LEFT : INSTR_RIGHT));
        ctx->consumed++;
    }
    /* Sin bloqueos indefinidos al consumir el token final de EOF. */
    instr_t extra;
    assert(queue_pop(&ctx->queue, &extra) == 0);
    return NULL;
}

static void wait_for_first_block(instruction_queue_t *q, int producer)
{
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000L };
    for (int attempt = 0; attempt < 3000; attempt++) {
        lock(&q->mutex);
        uint64_t count = producer ? q->producer_blocks : q->consumer_blocks;
        unlock(&q->mutex);
        if (count > 0) return;
        nanosleep(&pause, NULL);
    }
    fprintf(stderr, "FALLO: no se observó la espera esperada (%s)\n",
            producer ? "reader" : "executor");
    abort();
}

static void run_stress(const char *file, int producer_first)
{
    stress_context_t ctx = { .file = file };
    assert(queue_init(&ctx.queue) == 0);
    pthread_t producer, consumer;

    if (producer_first) {
        assert(pthread_create(&producer, NULL, stress_reader, &ctx) == 0);
        wait_for_first_block(&ctx.queue, 1); /* Cola llena. */
        assert(pthread_create(&consumer, NULL, stress_executor, &ctx) == 0);
    } else {
        assert(pthread_create(&consumer, NULL, stress_executor, &ctx) == 0);
        wait_for_first_block(&ctx.queue, 0); /* Cola vacía. */
        assert(pthread_create(&producer, NULL, stress_reader, &ctx) == 0);
    }

    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(consumer, NULL) == 0);
    assert(ctx.produced == EXPECTED_INSTRUCTIONS);
    assert(ctx.consumed == EXPECTED_INSTRUCTIONS);
    assert(ctx.queue.enqueued == EXPECTED_INSTRUCTIONS);
    assert(ctx.queue.dequeued == EXPECTED_INSTRUCTIONS);
    assert(ctx.queue.count == 0);
    assert(ctx.queue.max_depth <= P1_QUEUE_CAPACITY);
    assert(ctx.queue.max_depth > 0);
    if (producer_first) assert(ctx.queue.producer_blocks > 0);
    else                assert(ctx.queue.consumer_blocks > 0);

    printf("OK: %s | capacidad=%d | encoladas=%" PRIu64
           " | consumidas=%" PRIu64 " | max=%zu"
           " | bloqueos_reader=%" PRIu64 " | bloqueos_executor=%" PRIu64 "\n",
           producer_first ? "productor primero" : "consumidor primero",
           P1_QUEUE_CAPACITY, ctx.queue.enqueued, ctx.queue.dequeued,
           ctx.queue.max_depth, ctx.queue.producer_blocks,
           ctx.queue.consumer_blocks);
    queue_destroy(&ctx.queue);
}

int main(int argc, char **argv)
{
    const char *path = argc == 2 ? argv[1] :
        "tests/unit/12_estres_cola/bomber_moves.txt";
    assert(P1_QUEUE_CAPACITY == 4);
    run_stress(path, 1);
    run_stress(path, 0);
    puts("OK: 12_estres_cola; 5000 instrucciones FIFO; no hay pérdidas ni bloqueos permanentes");
    return 0;
}
