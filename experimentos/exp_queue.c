/* Experimento: cola real de P1 con y sin mutex. 1 productor, 1 consumidor, N instrucciones. */
#include "src/p1_bomber.c"
#include <time.h>
#define N 50000
typedef struct { instruction_queue_t q; long fifo_err, consumed, early_stop; } ctx_t;
static void *prod(void *a){ ctx_t *c=a; for(int i=1;i<=N;i++){ instr_t in={0}; in.type=INSTR_LEFT; in.line_no=i; if(queue_push(&c->q,&in)==-1) break; } queue_finish(&c->q); return NULL; }
static void *cons(void *a){ ctx_t *c=a; int expect=1; for(;;){ instr_t in; int r=queue_pop(&c->q,&in); if(r==1){ if(in.line_no!=expect) c->fifo_err++; expect=in.line_no+1; c->consumed++; } else { if(r==-1 || c->consumed<N) c->early_stop=1; if(c->consumed<N) queue_cancel(&c->q); break; } } return NULL; }
int main(void){ ctx_t c={0}; queue_init(&c.q); pthread_t p,k; pthread_create(&k,NULL,cons,&c); pthread_create(&p,NULL,prod,&c); pthread_join(p,NULL); pthread_join(k,NULL);
  printf("EXPQ consumidas=%ld perdidas=%ld fifo_err=%ld count_final=%ld corte=%ld\n", c.consumed, (long)N-c.consumed, c.fifo_err, (long)c.q.count, c.early_stop); return 0; }
