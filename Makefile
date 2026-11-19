# Bomberman concurrente en C con POSIX - IS2021 Computing Systems (UTEC)
#
#   make                         compila ./bomberman
#   make run CASE=tests/01_ejemplo_8_5     ejecuta un caso (solo consola)
#   make render CASE=tests/01_ejemplo_8_5  ejecuta con el renderer P4
#   make test                    corre todos los casos y compara con expected.txt
#   make tsan                    compila con ThreadSanitizer y corre los casos
#   make valgrind                busca fugas de memoria con valgrind
#   make helgrind                busca condiciones de carrera con valgrind --tool=helgrind
#   make test_base               prueba de la Fase 1 (shm, mapa, parser)
#   make stress                  prueba 12: estrés de la cola de P1 (5000 instrucciones, cola de 4)
#   make clean

CC      := gcc
CFLAGS  ?= -Wall -Wextra -std=gnu11 -O2 -g
ALL_CFLAGS = $(CFLAGS) -pthread -Iinclude
LDLIBS  += -pthread -lrt

CASE    ?= tests/01_ejemplo_8_5
TICK_MS ?= 400

BASE_SRC := src/util.c src/shm.c src/map.c src/instr.c
PROC_SRC := src/p0_scheduler.c src/p1_bomber.c src/p2_enemy.c src/p3_bomb.c src/p4_renderer.c
BASE_OBJ := $(BASE_SRC:src/%.c=build/%.o)
PROC_OBJ := $(PROC_SRC:src/%.c=build/%.o)
HEADERS  := include/common.h

.PHONY: all run render test tsan valgrind helgrind stress clean

all: bomberman bomberman_t6

bomberman: build/main.o $(BASE_OBJ) $(PROC_OBJ)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(HEADERS) | build
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

build:
	mkdir -p build logs

# Variante solo para la prueba 07: con BOMB_TIMER = 3 es imposible tener una
# tercera solicitud mientras hay 2 bombas activas (ver README).
bomberman_t6: src/*.c $(HEADERS)
	$(CC) $(ALL_CFLAGS) -DBOMB_TIMER=6 -o $@ src/*.c $(LDLIBS)

test_base: tests/unit/test_base.c $(BASE_OBJ)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDLIBS)
	./test_base tests/01_ejemplo_8_5/map.txt

# Prueba 12: compila el p1_bomber.c real con la cola reducida a 4 espacios.
test_p1_stress: tests/unit/test_p1_stress.c src/p1_bomber.c $(BASE_SRC) $(HEADERS)
	$(CC) $(ALL_CFLAGS) -Wpedantic -Werror -DP1_QUEUE_CAPACITY=4 -o $@ tests/unit/test_p1_stress.c $(BASE_SRC) $(LDLIBS)

stress: test_p1_stress
	./test_p1_stress

run: bomberman
	./bomberman $(CASE)

render: bomberman
	./bomberman $(CASE) --render --tick-ms $(TICK_MS)

test: bomberman bomberman_t6
	./scripts/run_tests.sh

tsan:
	$(MAKE) clean
	$(MAKE) all CFLAGS="-Wall -Wextra -std=gnu11 -O1 -g -fsanitize=thread" LDLIBS="-pthread -lrt -fsanitize=thread"
	./scripts/run_tests.sh tsan
	$(MAKE) clean

valgrind: bomberman bomberman_t6
	./scripts/run_tests.sh valgrind

helgrind: bomberman bomberman_t6
	./scripts/run_tests.sh helgrind

clean:
	rm -rf build bomberman bomberman_t6 test_base test_p1_stress
