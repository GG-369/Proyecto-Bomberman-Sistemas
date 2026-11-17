/*
 * main.c - Punto de entrada. Es el arranque de P0 (scheduler_process).
 *
 * Inicialización (sección 4 del enunciado), en este orden:
 *   1. Leer argumentos y config.txt del caso.
 *   2. Crear la memoria compartida y los semáforos/mutex compartidos.
 *   3. Leer y validar map.txt dentro de la memoria compartida.
 *   4. (Si hay renderer) guardar la terminal con dup() y redirigir el log
 *      a logs/<caso>.log con dup2(): P4 dibuja en la terminal y el log no
 *      se mezcla con el dibujo.
 *   5. fork() de P1, P2, P3 (y P4). Cada hijo ejecuta su pX_main().
 *   6. P0 entra a su ciclo de planificación (p0_run), espera a los hijos
 *      con waitpid() y libera todos los recursos IPC.
 *
 * Uso: ./bomberman <carpeta_del_caso> [--render] [--tick-ms N] [--max-ticks N]
 */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char *case_dir;
    int render;
    int tick_ms;
    int max_ticks_override;
} options_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Uso: %s <carpeta_del_caso> [--render] [--tick-ms N] [--max-ticks N]\n"
            "  La carpeta debe tener map.txt, bomber_moves.txt, enemy_1..4_moves.txt\n"
            "  y opcionalmente config.txt (max_ticks, prio_bomber, prio_enemy, aging_ticks).\n",
            prog);
}

static int parse_int(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || end == s || v < 0 || v > 1000000)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_args(int argc, char **argv, options_t *o)
{
    memset(o, 0, sizeof *o);
    o->max_ticks_override = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--render") == 0) {
            o->render = 1;
            if (o->tick_ms == 0)
                o->tick_ms = 400;
        } else if (strcmp(argv[i], "--tick-ms") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &o->tick_ms) == -1)
                return -1;
        } else if (strcmp(argv[i], "--max-ticks") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &o->max_ticks_override) == -1)
                return -1;
        } else if (argv[i][0] == '-' || o->case_dir != NULL) {
            return -1;
        } else {
            o->case_dir = argv[i];
        }
    }
    return o->case_dir ? 0 : -1;
}

/* config.txt: líneas "clave valor". Si no existe, se usan los valores por defecto. */
static int load_config(shared_state_t *st, const char *case_dir)
{
    st->max_ticks        = DEFAULT_MAX_TICKS;
    st->prioridad_bomber = DEFAULT_PRIO_BOMBER;
    st->prioridad_enemy  = DEFAULT_PRIO_ENEMY;
    st->aging_ticks      = 0;

    char path[PATH_LEN + 16];
    snprintf(path, sizeof path, "%s/config.txt", case_dir);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    char key[64];
    int  val, line_no = 0, ok = 1;
    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        line_no++;
        if (strchr(line, '\n') == NULL && !feof(f)) {
            int c;                         /* línea muy larga: se descarta el resto */
            while ((c = fgetc(f)) != '\n' && c != EOF)
                ;
        }
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (sscanf(line, "%63s %d", key, &val) != 2) {
            fprintf(stderr, "config.txt:%d: línea inválida\n", line_no);
            ok = 0;
            continue;
        }
        if (strcmp(key, "max_ticks") == 0 && val > 0)
            st->max_ticks = val;
        else if (strcmp(key, "prio_bomber") == 0 && val >= PRIORITY_MIN && val <= PRIORITY_MAX)
            st->prioridad_bomber = val;
        else if (strcmp(key, "prio_enemy") == 0 && val >= PRIORITY_MIN && val <= PRIORITY_MAX)
            st->prioridad_enemy = val;
        else if (strcmp(key, "aging_ticks") == 0 && val >= 0)
            st->aging_ticks = val;
        else {
            fprintf(stderr, "config.txt:%d: clave o valor inválido: %s %d\n", line_no, key, val);
            ok = 0;
        }
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* Redirige stdout al archivo de log y devuelve un descriptor a la terminal. */
static int redirect_log(const char *case_dir)
{
    const char *name = strrchr(case_dir, '/');
    name = (name && name[1]) ? name + 1 : case_dir;
    if (mkdir("logs", 0755) == -1 && errno != EEXIST)
        perror("mkdir logs");
    char path[PATH_LEN + 16];
    snprintf(path, sizeof path, "logs/%s.log", name);

    int tty_fd = dup(STDOUT_FILENO);          /* copia de la terminal para P4 */
    if (tty_fd == -1) {
        perror("dup");
        return -1;
    }
    int log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd == -1) {
        perror(path);
        close(tty_fd);
        return -1;
    }
    if (dup2(log_fd, STDOUT_FILENO) == -1) {   /* stdout -> archivo de log */
        perror("dup2");
        close(log_fd);
        close(tty_fd);
        return -1;
    }
    close(log_fd);
    fprintf(stderr, "[P0] log de la simulación en %s\n", path);
    return tty_fd;
}

/* SIGINT (Ctrl+C): P0 termina el juego en orden y libera la memoria compartida. */
volatile sig_atomic_t g_interrupted = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_interrupted = 1;   /* solo se marca: el ciclo de P0 hace la limpieza */
}

static int install_sigint(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction(SIGINT)");
        return -1;
    }
    return 0;
}

typedef void (*child_fn)(shared_state_t *);

static void p4_entry(shared_state_t *st);
static int g_tty_fd = -1;

static pid_t spawn_child(shared_state_t *st, child_fn fn, const char *name)
{
    fflush(stdout);              /* evita que el hijo herede texto sin imprimir */
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* Hijo: ignora Ctrl+C; P0 se encarga de terminar el juego en orden. */
        signal(SIGINT, SIG_IGN);
        fn(st);
        fflush(stdout);
        shm_detach(st);
        _exit(EXIT_SUCCESS);
    }
    log_line("[P0] fork() -> %s con PID %d", name, (int)pid);
    return pid;
}

static void p4_entry(shared_state_t *st)
{
    p4_main(st, g_tty_fd);
}

int main(int argc, char **argv)
{
    options_t opt;
    if (parse_args(argc, argv, &opt) == -1) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    shared_state_t *st = shm_create();
    if (st == NULL)
        return EXIT_FAILURE;
    if (shm_init_sync(st) == -1) {
        shm_destroy(st);
        return EXIT_FAILURE;
    }

    snprintf(st->case_dir, sizeof st->case_dir, "%s", opt.case_dir);
    char map_path[PATH_LEN + 16];
    snprintf(map_path, sizeof map_path, "%s/map.txt", opt.case_dir);
    if (load_config(st, opt.case_dir) == -1 || map_load(map_path, st) == -1) {
        fprintf(stderr, "[P0] error de inicialización: se cancela la simulación\n");
        shm_destroy(st);
        return EXIT_FAILURE;
    }
    if (opt.max_ticks_override > 0)
        st->max_ticks = opt.max_ticks_override;
    st->bomber_lives    = INITIAL_LIVES;
    st->global_tick     = 0;
    st->tick_ms         = opt.tick_ms;
    st->render_enabled  = opt.render;
    st->initial_enemies = st->enemies_alive;

    if (opt.render && (g_tty_fd = redirect_log(opt.case_dir)) == -1) {
        shm_destroy(st);
        return EXIT_FAILURE;
    }
    if (install_sigint() == -1) {
        shm_destroy(st);
        return EXIT_FAILURE;
    }

    log_line("[P0] caso %s | mapa %dx%d | max_ticks %d | prioridad P1=%d P2=%d | aging %s",
             opt.case_dir, st->map_w, st->map_h, st->max_ticks,
             st->prioridad_bomber, st->prioridad_enemy,
             st->aging_ticks ? "activado" : "desactivado");

    struct { child_fn fn; const char *name; } table[] = {
        { p1_main, "P1 bomber_process" },
        { p2_main, "P2 enemy_process" },
        { p3_main, "P3 bomb_process" },
        { p4_entry, "P4 renderer_process" },
    };
    int n_children = opt.render ? 4 : 3;
    pid_t children[4];
    int created = 0;
    for (int i = 0; i < n_children; i++) {
        children[i] = spawn_child(st, table[i].fn, table[i].name);
        if (children[i] == -1)
            break;
        created++;
    }
    if (g_tty_fd != -1)
        close(g_tty_fd);         /* P0 ya no necesita la terminal; P4 tiene su copia */

    if (created < n_children) {
        /* Falló un fork: se marca el fin para que los hijos creados terminen. */
        lock(&st->m_state);
        st->game_over = 1;
        st->game_result = RESULT_ERROR;
        unlock(&st->m_state);
    }
    int rc = p0_run(st, children, created);

    shm_destroy(st);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
