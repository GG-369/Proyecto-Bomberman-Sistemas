/*
 * shm.c - Memoria compartida POSIX y primitivas de sincronización.
 *
 * P0 crea la región con shm_open + ftruncate + mmap ANTES de hacer fork().
 * Los hijos heredan el mapeo MAP_SHARED, así que todos ven la misma
 * shared_state_t. Los semáforos y mutex que coordinan procesos distintos
 * viven DENTRO de esa región:
 *   - sem_init(&s, 1, 0): pshared = 1 indica que el semáforo se comparte
 *     entre procesos (con 0 solo serviría entre hilos de un proceso).
 *   - pthread_mutexattr_setpshared(PTHREAD_PROCESS_SHARED): sin este
 *     atributo el comportamiento de un mutex usado desde otro proceso es
 *     indefinido.
 */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

shared_state_t *shm_create(void)
{
    /* O_EXCL: si quedó una región vieja de una ejecución abortada, se borra. */
    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1 && errno == EEXIST) {
        fprintf(stderr, "[P0] aviso: %s ya existía, se elimina y se recrea\n", SHM_NAME);
        if (shm_unlink(SHM_NAME) == -1)
            perror("shm_unlink");
        fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd == -1) {
        perror("shm_open");
        return NULL;
    }
    if (ftruncate(fd, sizeof(shared_state_t)) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }
    void *p = mmap(NULL, sizeof(shared_state_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }
    /* El descriptor ya no hace falta: el mapeo sigue vivo sin él. */
    if (close(fd) == -1)
        perror("close(shm fd)");
    memset(p, 0, sizeof(shared_state_t));
    return (shared_state_t *)p;
}

static int init_shared_mutex(pthread_mutex_t *m, const pthread_mutexattr_t *attr,
                             const char *name)
{
    int rc = pthread_mutex_init(m, attr);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_init(%s): %s\n", name, strerror(rc));
        return -1;
    }
    return 0;
}

static int init_shared_sem(sem_t *s, const char *name)
{
    if (sem_init(s, 1 /* pshared: entre procesos */, 0) == -1) {
        fprintf(stderr, "sem_init(%s): %s\n", name, strerror(errno));
        return -1;
    }
    return 0;
}

int shm_init_sync(shared_state_t *st)
{
    /* Todos los semáforos empiezan en 0: P1, P2, P3 y P4 quedan
     * bloqueados hasta que P0 les dé permiso con sem_post. */
    if (init_shared_sem(&st->sem_bomber_turn, "sem_bomber_turn") ||
        init_shared_sem(&st->sem_enemy_turn,  "sem_enemy_turn")  ||
        init_shared_sem(&st->sem_bomb_turn,   "sem_bomb_turn")   ||
        init_shared_sem(&st->sem_turn_done,   "sem_turn_done")   ||
        init_shared_sem(&st->sem_render,      "sem_render")      ||
        init_shared_sem(&st->sem_render_done, "sem_render_done"))
        return -1;

    pthread_mutexattr_t attr;
    int rc = pthread_mutexattr_init(&attr);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutexattr_init: %s\n", strerror(rc));
        return -1;
    }
    rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutexattr_setpshared: %s\n", strerror(rc));
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    int err = init_shared_mutex(&st->m_pos,     &attr, "m_pos")     ||
              init_shared_mutex(&st->m_map,     &attr, "m_map")     ||
              init_shared_mutex(&st->m_mailbox, &attr, "m_mailbox") ||
              init_shared_mutex(&st->m_events,  &attr, "m_events")  ||
              init_shared_mutex(&st->m_state,   &attr, "m_state");
    pthread_mutexattr_destroy(&attr);
    return err ? -1 : 0;
}

/* Lo llama solo P0, después de waitpid() a todos los hijos. */
void shm_destroy(shared_state_t *st)
{
    if (st == NULL)
        return;
    sem_t *sems[] = { &st->sem_bomber_turn, &st->sem_enemy_turn, &st->sem_bomb_turn,
                      &st->sem_turn_done, &st->sem_render, &st->sem_render_done };
    for (size_t i = 0; i < sizeof(sems) / sizeof(sems[0]); i++)
        if (sem_destroy(sems[i]) == -1)
            perror("sem_destroy");

    pthread_mutex_t *ms[] = { &st->m_pos, &st->m_map, &st->m_mailbox,
                              &st->m_events, &st->m_state };
    for (size_t i = 0; i < sizeof(ms) / sizeof(ms[0]); i++) {
        int rc = pthread_mutex_destroy(ms[i]);
        if (rc != 0)
            fprintf(stderr, "pthread_mutex_destroy: %s\n", strerror(rc));
    }
    shm_detach(st);
    if (shm_unlink(SHM_NAME) == -1)
        perror("shm_unlink");
}

/* Los hijos solo desmapean; no destruyen nada porque P0 es el dueño. */
void shm_detach(shared_state_t *st)
{
    if (st != NULL && munmap(st, sizeof(shared_state_t)) == -1)
        perror("munmap");
}

/* Envoltorios con comprobación de errores: un fallo aquí es un bug grave. */
void lock(pthread_mutex_t *m)
{
    int rc = pthread_mutex_lock(m);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_lock: %s\n", strerror(rc));
        abort();
    }
}

void unlock(pthread_mutex_t *m)
{
    int rc = pthread_mutex_unlock(m);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_unlock: %s\n", strerror(rc));
        abort();
    }
}

/* sem_wait puede volver con EINTR si llega una señal: se reintenta. */
void sem_wait_retry(sem_t *s)
{
    while (sem_wait(s) == -1) {
        if (errno != EINTR) {
            perror("sem_wait");
            abort();
        }
    }
}

void sem_post_checked(sem_t *s)
{
    if (sem_post(s) == -1) {
        perror("sem_post");
        abort();
    }
}
