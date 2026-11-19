"""Aplica a una COPIA del repo (argv[1]) la instrumentación y las variantes #ifdef de los experimentos."""
import re,sys
root=sys.argv[1]
def rd(p): return open(f"{root}/{p}",encoding="utf-8").read()
def wr(p,s): open(f"{root}/{p}","w",encoding="utf-8",newline="\n").write(s)

# ---------- P2: barrera workers_done ----------
s=rd("src/p2_enemy.c")
s=s.replace('#include "common.h"','#include "common.h"\n#include <stdatomic.h>\nstatic atomic_int exp_moving = 0;\nstatic atomic_int exp_dirty = 0;\nstatic atomic_int exp_turns = 0;',1)
assert 'exp_moving' in s
# enemigo: termina su movimiento -> decrementa
s=s.replace("""            read_next(w, file); /* detectar EOF y preparar el siguiente turno */
        }
        sem_post_checked(&ctx->workers_done);""","""            read_next(w, file); /* detectar EOF y preparar el siguiente turno */
        }
        atomic_fetch_sub(&exp_moving, 1);
        sem_post_checked(&ctx->workers_done);""")
# colision: ¿corre con enemigos aun moviendose?
s=s.replace("""        int found = -1;
        for (int i = 0; i < NUM_ENEMIES; i++) {""","""        atomic_fetch_add(&exp_turns, 1);
        if (atomic_load(&exp_moving) > 0) atomic_fetch_add(&exp_dirty, 1);
        int found = -1;
        for (int i = 0; i < NUM_ENEMIES; i++) {""")
s=s.replace("""        int n_active = 0;
        lock(&ctx.local_mutex);
        for (int i = 0; i < NUM_ENEMIES; i++) ctx.worker[i].request_priority = 0;""","""        int n_active = 0;
        for (int i = 0; i < NUM_ENEMIES; i++) if (ctx.alive[i]) n_active++;
        atomic_store(&exp_moving, n_active);
        n_active = 0;
        lock(&ctx.local_mutex);
        for (int i = 0; i < NUM_ENEMIES; i++) ctx.worker[i].request_priority = 0;""")
old="""        unlock(&ctx.local_mutex);
        for (int i = 0; i < n_active; i++) sem_wait_retry(&ctx.workers_done);

        publish_priority_request(&ctx);"""
assert old in s
s=s.replace(old,"""        unlock(&ctx.local_mutex);
#ifndef EXP_NO_BARRIER
        for (int i = 0; i < n_active; i++) sem_wait_retry(&ctx.workers_done);
#endif

        publish_priority_request(&ctx);""")
old="""        publish_exhausted(&ctx);
        sem_post_checked(&st->sem_turn_done);
    }"""
assert old in s
s=s.replace(old,"""#ifdef EXP_NO_BARRIER
        /* variante sin barrera: se espera al final solo para no desbalancear el semáforo */
        for (int i = 0; i < n_active; i++) sem_wait_retry(&ctx.workers_done);
#endif
        publish_exhausted(&ctx);
        sem_post_checked(&st->sem_turn_done);
    }""")
s=s.replace("""    stop_workers(&ctx);
    for (int i = 0; i < created; i++) {""","""    fprintf(stderr, "EXP p2 dirty=%d turns=%d\\n", atomic_load(&exp_dirty), atomic_load(&exp_turns));
    stop_workers(&ctx);
    for (int i = 0; i < created; i++) {""",1)
assert 'EXP p2' in s
wr("src/p2_enemy.c",s)

# ---------- P1: mutex de la cola ----------
s=rd("src/p1_bomber.c")
a=s.index("static void queue_wait_counted"); b=s.index("static int queue_exhausted")
b=s.index("}\n",b)+2
seg=s[a:b].replace("lock(&q->mutex)","QLOCK(&q->mutex)").replace("unQLOCK(&q->mutex)","QUNLOCK(&q->mutex)")
s=s[:a]+seg+s[b:]
s=s.replace('#include "common.h"','#include "common.h"\n#ifdef EXP_NO_QUEUE_MUTEX\n#define QLOCK(m) ((void)0)\n#define QUNLOCK(m) ((void)0)\n#else\n#define QLOCK(m) lock(m)\n#define QUNLOCK(m) unlock(m)\n#endif',1)
assert "QUNLOCK" in s
wr("src/p1_bomber.c",s)

# ---------- P0: liberar a los hijos al terminar ----------
s=rd("src/p0_scheduler.c")
old="""    release_children(st);
    reap_children();"""
assert old in s
s=s.replace(old,"""#ifndef EXP_NO_RELEASE
    release_children(st);
#endif
    reap_children();""")
wr("src/p0_scheduler.c",s)
print("ok")
