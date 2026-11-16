/*
 * p0_scheduler.c - P0 scheduler_process.
 *
 * P0 no mueve personajes ni hace explotar bombas: decide QUIÉN actúa en
 * cada tick y es el ÚNICO que escribe vidas, puntaje, prioridades vigentes,
 * enemy_alive[], game_over y game_result.
 *
 * Hilos (todos dentro de P0):
 *   scheduler_thread : dirige el tick y decide el turno (prioridad / RR / aging).
 *   tick_thread      : paso 1, global_tick++ y aplica SET_PRIORITY pendientes.
 *   signal_thread    : pasos 2 y 3, sem_post al proceso elegido y a P3, y
 *                      espera sem_turn_done de cada uno.
 *   event_thread     : paso 4, cierre del tick.
 *
 * Coordinación interna: semáforos privados de P0 (sem_init con pshared = 0)
 * en pares go_X / done_X. scheduler_thread hace sem_post(go_X) y espera
 * done_X, así los 4 pasos del tick ocurren siempre en el mismo orden.
 *
 * Race conditions del enunciado que se evitan aquí:
 *   - tick_thread actualiza global_tick mientras scheduler_thread lo lee
 *     -> ambos acceden con m_state tomado.
 *   - scheduler_thread modifica turno_actual mientras signal_thread lo
 *     consulta -> m_state, y además signal_thread solo lo lee después de
 *     sem_wait(go_signal), es decir, cuando ya fue escrito.
 *   - prioridades leídas y modificadas a la vez -> m_state (y m_mailbox
 *     para los buzones, tomado ANTES que m_state según el orden global).
 */
#include "common.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern volatile sig_atomic_t g_interrupted;

#define MAX_CHILDREN 4

typedef struct {
    shared_state_t *st;
    pid_t *children;
    int    n_children;
    int    reaped[MAX_CHILDREN];
    int    status[MAX_CHILDREN];

    /* Semáforos privados de P0 (pshared = 0): solo los usan sus hilos. */
    sem_t go_tick, done_tick;
    sem_t go_signal, done_signal;
    sem_t go_event, done_event;

    /* stop se escribe ANTES de un sem_post y se lee DESPUÉS del sem_wait
     * correspondiente: el semáforo ordena el acceso. */
    int stop;

    /* Solo los usa scheduler_thread. */
    int last_turn;          /* para el desempate Round Robin */
    int wait_bomber;        /* ticks seguidos sin turno (aging) */
    int wait_enemy;
} p0_ctx_t;

static p0_ctx_t g;

static const char *child_name(int i)
{
    static const char *names[] = { "P1 bomber_process", "P2 enemy_process",
                                   "P3 bomb_process", "P4 renderer_process" };
    return (i >= 0 && i < MAX_CHILDREN) ? names[i] : "?";
}

static void report_child_status(int i, int status)
{
    if (WIFEXITED(status))
        log_line("[P0] waitpid: %s (PID %d) terminó con exit %d",
                 child_name(i), (int)g.children[i], WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        log_line("[P0] waitpid: %s (PID %d) terminó por la señal %d (%s)",
                 child_name(i), (int)g.children[i], WTERMSIG(status), strsignal(WTERMSIG(status)));
}

/* ------------------------------------------------------------------ */
/* Paso 1: tick_thread                                                 */
/* ------------------------------------------------------------------ */
static void apply_priority_request(int *prio, int requested, const char *who)
{
    if (requested >= PRIORITY_MIN && requested <= PRIORITY_MAX) {
        log_line("[P0] buzón de %s: SET_PRIORITY %d aceptado (%d -> %d)",
                 who, requested, *prio, requested);
        *prio = requested;
    } else {
        log_line("[P0] buzón de %s: SET_PRIORITY %d RECHAZADO (fuera de %d..%d); sigue en %d",
                 who, requested, PRIORITY_MIN, PRIORITY_MAX, *prio);
    }
}

static void *tick_thread(void *arg)
{
    (void)arg;
    shared_state_t *st = g.st;
    for (;;) {
        sem_wait_retry(&g.go_tick);
        if (g.stop)
            break;

        lock(&st->m_state);
        int tick = ++st->global_tick;          /* único escritor: P0 */
        unlock(&st->m_state);
        log_line("\n==================== TICK %d ====================", tick);

        /* Orden global: m_mailbox antes que m_state. P0 solo LIMPIA las
         * banderas de los buzones; el valor lo escribió P1 o P2. */
        lock(&st->m_mailbox);
        lock(&st->m_state);
        if (st->priority_request_active) {
            apply_priority_request(&st->prioridad_bomber, st->pending_priority_bomber, "P1");
            st->priority_request_active = 0;
        }
        if (st->enemy_priority_request_active) {
            apply_priority_request(&st->prioridad_enemy, st->pending_priority_enemy, "P2");
            st->enemy_priority_request_active = 0;
        }
        int pb = st->prioridad_bomber, pe = st->prioridad_enemy;
        unlock(&st->m_state);
        unlock(&st->m_mailbox);

        log_line("[P0] inicio: tick %d | prioridades vigentes P1=%d P2=%d", tick, pb, pe);
        sem_post_checked(&g.done_tick);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* scheduler_thread: decide el turno                                   */
/* ------------------------------------------------------------------ */
static turn_t decide_turn(int pb, int pe, int aging, char *reason, size_t len)
{
    turn_t t;
    if (aging > 0 && pb != pe && (g.wait_bomber >= aging || g.wait_enemy >= aging)) {
        /* Aging: el proceso con menor prioridad lleva 'aging' ticks sin
         * turno; se eleva temporalmente su prioridad efectiva por un tick. */
        t = (g.wait_bomber >= aging) ? TURN_BOMBER : TURN_ENEMY;
        snprintf(reason, len, "aging: %s llevaba %d ticks sin turno",
                 t == TURN_BOMBER ? "P1" : "P2",
                 t == TURN_BOMBER ? g.wait_bomber : g.wait_enemy);
    } else if (pb > pe) {
        t = TURN_BOMBER;
        snprintf(reason, len, "prioridad P1 %d > P2 %d", pb, pe);
    } else if (pe > pb) {
        t = TURN_ENEMY;
        snprintf(reason, len, "prioridad P2 %d > P1 %d", pe, pb);
    } else {
        t = (g.last_turn == TURN_BOMBER) ? TURN_ENEMY : TURN_BOMBER;
        snprintf(reason, len, "empate %d = %d, Round Robin", pb, pe);
    }
    g.last_turn = t;
    if (t == TURN_BOMBER) { g.wait_bomber = 0; g.wait_enemy++;  }
    else                  { g.wait_enemy = 0;  g.wait_bomber++; }
    return t;
}

static void set_game_over(shared_state_t *st, int result)
{
    lock(&st->m_state);
    if (!st->game_over) {
        st->game_over = 1;
        st->game_result = result;
    }
    unlock(&st->m_state);
}

static void *scheduler_thread(void *arg)
{
    (void)arg;
    shared_state_t *st = g.st;
    for (;;) {
        lock(&st->m_state);
        int over = st->game_over;
        unlock(&st->m_state);
        if (!over && g_interrupted) {
            log_line("[P0] SIGINT recibido: se termina la simulación en orden");
            set_game_over(st, RESULT_INTERRUPTED);
            over = 1;
        }
        if (over)
            break;

        /* Paso 1: inicio */
        sem_post_checked(&g.go_tick);
        sem_wait_retry(&g.done_tick);

        /* Paso 2 (decisión): se comparan las prioridades vigentes. */
        char reason[64];
        lock(&st->m_state);
        turn_t turn = decide_turn(st->prioridad_bomber, st->prioridad_enemy,
                                  st->aging_ticks, reason, sizeof reason);
        st->turno_actual = turn;
        snprintf(st->turn_reason, sizeof st->turn_reason, "%s", reason);
        unlock(&st->m_state);
        log_line("[P0] acción: turno -> %s (%s)",
                 turn == TURN_BOMBER ? "P1 bomber_process" : "P2 enemy_process", reason);

        /* Pasos 2 y 3: signal_thread despierta al elegido y luego a P3. */
        sem_post_checked(&g.go_signal);
        sem_wait_retry(&g.done_signal);

        /* Paso 4: cierre */
        sem_post_checked(&g.go_event);
        sem_wait_retry(&g.done_event);

        /* Bonus: P4 dibuja el estado al final del tick. */
        if (st->render_enabled) {
            sem_post_checked(&st->sem_render);
            sem_wait_retry(&st->sem_render_done);
        }
        sleep_ms(st->tick_ms);
    }

    /* Fin: despierta a los otros hilos de P0 para que terminen. */
    g.stop = 1;
    sem_post_checked(&g.go_tick);
    sem_post_checked(&g.go_signal);
    sem_post_checked(&g.go_event);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Pasos 2 y 3: signal_thread                                          */
/* ------------------------------------------------------------------ */
static void *signal_thread(void *arg)
{
    (void)arg;
    shared_state_t *st = g.st;
    for (;;) {
        sem_wait_retry(&g.go_signal);
        if (g.stop)
            break;

        lock(&st->m_state);
        int turn = st->turno_actual;
        unlock(&st->m_state);

        /* Fase de acción: solo uno de P1 o P2. */
        sem_post_checked(turn == TURN_BOMBER ? &st->sem_bomber_turn : &st->sem_enemy_turn);
        sem_wait_retry(&st->sem_turn_done);

        /* Fase de bombas: P3 en TODOS los ticks, sin competir por prioridad. */
        log_line("[P0] bombas: turno -> P3 bomb_process (todos los ticks)");
        sem_post_checked(&st->sem_bomb_turn);
        sem_wait_retry(&st->sem_turn_done);

        sem_post_checked(&g.done_signal);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Paso 4: event_thread                                                */
/* ------------------------------------------------------------------ */
/* Versión inicial del cierre: solo evalúa el límite de ticks. */
static void close_tick(void)
{
    shared_state_t *st = g.st;
    int result = RESULT_NONE;

    lock(&st->m_state);
    int tick = st->global_tick;
    if (tick >= st->max_ticks) {
        result = RESULT_MAX_TICKS;
        st->game_over = 1;
        st->game_result = result;
    }
    unlock(&st->m_state);

    log_line("[P0] cierre: tick %d", tick);
    if (result != RESULT_NONE)
        log_line("[P0] FIN DEL JUEGO en el tick %d: %s", tick, result_name(result));
}

static void *event_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sem_wait_retry(&g.go_event);
        if (g.stop)
            break;
        close_tick();
        sem_post_checked(&g.done_event);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Arranque y cierre                                                   */
/* ------------------------------------------------------------------ */
static int init_private_sems(void)
{
    sem_t *s[] = { &g.go_tick, &g.done_tick, &g.go_signal,
                   &g.done_signal, &g.go_event, &g.done_event };
    for (size_t i = 0; i < sizeof s / sizeof s[0]; i++)
        if (sem_init(s[i], 0 /* solo entre hilos de P0 */, 0) == -1) {
            perror("sem_init (P0)");
            return -1;
        }
    return 0;
}

static void destroy_private_sems(void)
{
    sem_t *s[] = { &g.go_tick, &g.done_tick, &g.go_signal,
                   &g.done_signal, &g.go_event, &g.done_event };
    for (size_t i = 0; i < sizeof s / sizeof s[0]; i++)
        if (sem_destroy(s[i]) == -1)
            perror("sem_destroy (P0)");
}

/* Despierta a todos los procesos para que vean game_over y terminen. */
static void release_children(shared_state_t *st)
{
    log_line("[P0] liberando semáforos para que P1, P2, P3%s terminen",
             st->render_enabled ? " y P4" : "");
    sem_post_checked(&st->sem_bomber_turn);
    sem_post_checked(&st->sem_enemy_turn);
    sem_post_checked(&st->sem_bomb_turn);
    if (st->render_enabled)
        sem_post_checked(&st->sem_render);
}

static void reap_children(void)
{
    for (int i = 0; i < g.n_children; i++) {
        if (g.reaped[i])
            continue;
        pid_t r;
        while ((r = waitpid(g.children[i], &g.status[i], 0)) == -1 && errno == EINTR)
            ;
        if (r == -1) {
            perror("waitpid");
            continue;
        }
        g.reaped[i] = 1;
        report_child_status(i, g.status[i]);
    }
}

int p0_run(shared_state_t *st, pid_t children[], int n_children)
{
    memset(&g, 0, sizeof g);
    g.st = st;
    g.children = children;
    g.n_children = n_children;
    g.last_turn = TURN_ENEMY;          /* con empate, el primer turno es de P1 */
    if (init_private_sems() == -1)
        return -1;

    /* Barrera de arranque: cada hijo avisa que creó sus hilos. */
    for (int i = 0; i < n_children; i++)
        sem_wait_retry(i < 3 ? &st->sem_turn_done : &st->sem_render_done);
    log_line("[P0] los %d procesos hijos están listos: empieza la simulación", n_children);

    /* Primero los hilos auxiliares y al final scheduler_thread, que es el
     * que los pone a trabajar: así nunca espera a un hilo inexistente. */
    pthread_t th[4];
    void *(*fns[4])(void *) = { tick_thread, signal_thread, event_thread, scheduler_thread };
    const char *names[4] = { "tick_thread", "signal_thread", "event_thread", "scheduler_thread" };
    int created = 0;
    for (int i = 0; i < 4; i++) {
        int rc = pthread_create(&th[i], NULL, fns[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_create(%s): %s\n", names[i], strerror(rc));
            break;
        }
        created++;
    }
    if (created < 4) {
        /* Sin todos los hilos no se puede jugar: se despierta a los creados
         * con stop = 1 para que terminen. */
        set_game_over(st, RESULT_ERROR);
        g.stop = 1;
        sem_post_checked(&g.go_tick);
        sem_post_checked(&g.go_signal);
        sem_post_checked(&g.go_event);
    }
    for (int i = 0; i < created; i++) {
        int rc = pthread_join(th[i], NULL);
        if (rc != 0)
            fprintf(stderr, "pthread_join(%s): %s\n", names[i], strerror(rc));
    }

    release_children(st);
    reap_children();

    lock(&st->m_state);
    int result = st->game_result, tick = st->global_tick;
    unlock(&st->m_state);
    log_line("RESULTADO %s | ticks=%d", result_name(result), tick);

    destroy_private_sems();
    return result == RESULT_ERROR ? -1 : 0;
}
