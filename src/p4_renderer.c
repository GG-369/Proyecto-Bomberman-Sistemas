/*
 * p4_renderer.c - P4 renderer_process.
 *
 * Dibuja el estado del juego en la terminal (stdout original, guardado por P0 con dup()).
 * Un solo hilo: espera sem_render, toma snapshot con map_snapshot, dibuja, avisa sem_render_done.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BOARD_LINES (MAX_Y + 2)

static int g_tty_fd = -1;
static shared_state_t *g_st = NULL;

/* Mueve el cursor a (fila, col) 1-indexado */
static void cursor_goto(int row, int col)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "\033[%d;%dH", row, col);
    if (n > 0) {
        if (write(g_tty_fd, buf, n) == -1)
            perror("write cursor_goto");
    }
}

/* Limpia la pantalla y mueve a (1,1) */
static void clear_screen(void)
{
    const char *seq = "\033[2J\033[H";
    if (write(g_tty_fd, seq, strlen(seq)) == -1)
        perror("write clear_screen");
}

/* Oculta el cursor */
static void hide_cursor(void)
{
    const char *seq = "\033[?25l";
    if (write(g_tty_fd, seq, strlen(seq)) == -1)
        perror("write hide_cursor");
}

/* Muestra el cursor */
static void show_cursor(void)
{
    const char *seq = "\033[?25h";
    if (write(g_tty_fd, seq, strlen(seq)) == -1)
        perror("write show_cursor");
}

/* Dibuja el tablero y la tabla de bombas. Precondición: m_pos, m_map y m_state tomados. */
static int format_board(const shared_state_t *st, char lines[BOARD_LINES][MAX_X + 80])
{
    char snap[MAX_Y][MAX_X + 1];
    map_snapshot(st, snap);
    int n = 0;
    for (int y = 0; y < st->map_h; y++) {
        char side[64] = "";
        if (y == 0)
            snprintf(side, sizeof side, "bombas activas: %d", st->active_bombs);
        else if (y <= MAX_BOMBS) {
            int b = y - 1;
            if (st->bomb_timer[b] > 0)
                snprintf(side, sizeof side, "  bomba (%d, %d) timer %d",
                         st->bomb_x[b], st->bomb_y[b], st->bomb_timer[b]);
        }
        snprintf(lines[n++], MAX_X + 80, "     %.*s   %.60s", MAX_X, snap[y], side);
    }
    return n;
}

static void render_frame(shared_state_t *st)
{
    char board[BOARD_LINES][MAX_X + 80];
    char summary[256];
    char positions[96] = "";
    char events[160] = "";

    lock(&st->m_pos);
    lock(&st->m_map);
    lock(&st->m_events);
    lock(&st->m_state);

    int n_lines = format_board(st, board);
    int tick = st->global_tick;

    /* Copiar eventos para imprimir fuera de la sección crítica */
    if (st->last_events[0])
        snprintf(events, sizeof events, "%s", st->last_events);
    else
        snprintf(events, sizeof events, "sin eventos");

    size_t off = 0;
    for (int i = 0; i < NUM_ENEMIES && off < sizeof positions; i++) {
        int w;
        if (st->enemy_alive[i])
            w = snprintf(positions + off, sizeof positions - off, "%s%c(%d,%d)",
                         i ? " " : "", 'A' + i, st->enemy_x[i], st->enemy_y[i]);
        else
            w = snprintf(positions + off, sizeof positions - off, "%s%c(-)", i ? " " : "", 'A' + i);
        if (w > 0)
            off += (size_t)w;
    }

    snprintf(summary, sizeof summary,
             "RESUMEN tick=%d turno=%s bomber=(%d,%d) enemigos=[%s] vidas=%d vivos=%d "
             "bombas=%d bloques=%d puntaje=%d prio=%d/%d",
             tick, st->turno_actual == TURN_BOMBER ? "P1" : "P2",
             st->bomber_x, st->bomber_y, positions, st->bomber_lives, st->enemies_alive,
             st->active_bombs, st->blocks_destroyed, st->score,
             st->prioridad_bomber, st->prioridad_enemy);

    unlock(&st->m_state);
    unlock(&st->m_events);
    unlock(&st->m_map);
    unlock(&st->m_pos);

    /* Dibujar en la terminal */
    clear_screen();
    hide_cursor();

    /* Cabecera */
    cursor_goto(1, 1);
    char header[256];
    int hlen = snprintf(header, sizeof header,
                        "Bomberman Concurrente | Tick %d | %s | %s\n",
                        tick, events, summary);
    if (write(g_tty_fd, header, hlen) == -1)
        perror("write header");

    /* Tablero */
    for (int i = 0; i < n_lines; i++) {
        cursor_goto(i + 2, 1);
        char line[MAX_X + 82];
        int len = snprintf(line, sizeof line, "%s\n", board[i]);
        if (write(g_tty_fd, line, len) == -1)
            perror("write board line");
    }

    /* Leyenda */
    cursor_goto(n_lines + 3, 1);
    const char *legend = "Leyenda: X=pared #=bloque O=libre P=Bomberman A-D=enemigos Q=bomba *=fuego +=power-up\n";
    if (write(g_tty_fd, legend, strlen(legend)) == -1)
        perror("write legend");

    show_cursor();
}

void p4_main(shared_state_t *st, int tty_fd)
{
    g_st = st;
    g_tty_fd = tty_fd;

    /* Avisar a P0 que P4 está listo (barrera de arranque) */
    sem_post_checked(&st->sem_render_done);

    for (;;) {
        sem_wait_retry(&st->sem_render);
        lock(&st->m_state);
        int over = st->game_over;
        unlock(&st->m_state);
        if (over)
            break;

        render_frame(st);
        sem_post_checked(&st->sem_render_done);
    }

    /* Mostrar resultado final */
    lock(&st->m_state);
    int result = st->game_result;
    int tick = st->global_tick;
    int lives = st->bomber_lives;
    int alive = st->enemies_alive;
    int blocks = st->blocks_destroyed;
    int score = st->score;
    unlock(&st->m_state);

    clear_screen();
    cursor_goto(1, 1);
    char final_msg[256];
    int flen = snprintf(final_msg, sizeof final_msg,
                        "=== SIMULACIÓN TERMINADA ===\n"
                        "Resultado: %s\n"
                        "Ticks: %d | Vidas: %d | Enemigos vivos: %d | Bloques: %d | Puntaje: %d\n",
                        result_name(result), tick, lives, alive, blocks, score);
    if (write(g_tty_fd, final_msg, flen) == -1)
        perror("write final_msg");

    log_line("[P4] renderer terminado");
}