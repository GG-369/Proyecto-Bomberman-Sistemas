# P2 · Bomberman concurrente en C con POSIX

IS2021 Computing Systems · UTEC · 2026-2
Grupo: Leon Pantaleon, Jobeth Alexander · Gamboa Sanchez, Guillermo Valentino Ceceu · Solis Cordova, Pedro Nicolas

Simulación de Bomberman con **5 procesos** (P0 scheduler, P1 bomber, P2 enemigos, P3 bombas y P4 renderer como bonus) y **19 hilos**. Se comunican **solo por memoria compartida POSIX**, con semáforos (`pshared = 1`) y mutex `PTHREAD_PROCESS_SHARED` guardados dentro de esa misma memoria.

## Integrantes y módulos

| Integrante | Módulo | Archivos | Mejora original |
|---|---|---|---|
| Leon Pantaleon, Jobeth Alexander | P0 scheduler + IPC | `src/p0_scheduler.c`, `src/main.c`, `src/shm.c`, `src/util.c`, `Makefile` | Reporte del planificador |
| Gamboa Sanchez, Guillermo Valentino Ceceu | P1 bomber + P2 enemigos | `src/p1_bomber.c`, `src/p2_enemy.c`, `src/instr.c`, `tests/unit/` | Estrés de la cola productor-consumidor |
| Solis Cordova, Pedro Nicolas | Mapa, P3 bombas, P4 renderer y pruebas | `src/map.c`, `src/p3_bomb.c`, `src/p4_renderer.c`, `scripts/run_tests.sh` | Power-ups `+` |
| Los tres | Contrato común | `include/common.h` | — |

## Requisitos

- Linux o WSL Ubuntu con `gcc`, `make` y la biblioteca de hilos POSIX.
- Opcional, para revisar la concurrencia: `valgrind`.
- **No** necesita ncurses: P4 dibuja con secuencias ANSI.

## Compilar

```bash
make            # compila ./bomberman y ./bomberman_t6 con -Wall -Wextra -pthread (0 warnings)
make clean
```

## Ejecutar

```bash
./bomberman tests/01_ejemplo_8_5                  # log por tick en la consola
./bomberman tests/01_ejemplo_8_5 --render         # P4 dibuja el juego; el log va a logs/01_ejemplo_8_5.log
./bomberman tests/02_round_robin --render --tick-ms 700   # más lento, para la exposición
./bomberman tests/03_inanicion --max-ticks 15     # cambia el límite de ticks
make run CASE=tests/04_reaccion_cadena
make render CASE=tests/05_colision_y_explosion TICK_MS=800
```

Ctrl+C termina la simulación en orden: P0 marca `game_over`, despierta a los hijos, hace `waitpid` y borra la memoria compartida (en `/dev/shm` no queda nada).

### Carpeta de un caso

| Archivo | Contenido |
|---|---|
| `map.txt` | Mapa con `X # O P A B C D +` (máximo 32×32) |
| `bomber_moves.txt` | `UP DOWN LEFT RIGHT BOMB SET_PRIORITY <n>` |
| `enemy_1..4_moves.txt` | Igual, pero sin `BOMB` |
| `config.txt` (opcional) | `max_ticks 20`, `prio_bomber 40`, `prio_enemy 30`, `aging_ticks 0` |
| `expected.txt` | Líneas `RESUMEN` (una por tick) y `RESULTADO` que debe producir el caso |
| `checks.txt` | Frases que deben aparecer en el log |
| `binary.txt` (opcional) | Binario a usar, si no es `./bomberman` |

## Mejoras originales

### Reporte del planificador (P0)

Al terminar, P0 imprime métricas de planificación en líneas `REPORTE`: turnos de P1 y P2 con su porcentaje, fases de bombas de P3, la racha más larga de ticks sin turno (inanición medida), los turnos otorgados por aging (total y por proceso) y las solicitudes `SET_PRIORITY` aceptadas y rechazadas. Cada contador tiene un solo hilo escritor y el reporte se imprime después de `pthread_join`, así que no necesita mutex.

```bash
./bomberman tests/10_aging | grep REPORTE
```

### Estrés de la cola productor-consumidor (P1)

P1 cuenta cuántas veces el lector se bloqueó en `sem_slots` (cola llena) y el ejecutor en `sem_items` (cola vacía), además de las instrucciones encoladas y consumidas y la ocupación máxima. Lo imprime al terminar (`[P1] COLA ...`). La prueba compila el `p1_bomber.c` real con la cola reducida a 4 espacios y le pasa 5000 instrucciones: verifica el orden FIFO, que no se pierda ninguna y que no haya bloqueos permanentes.

```bash
gcc -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -Werror -pthread -DP1_QUEUE_CAPACITY=4 -Iinclude \
    tests/unit/test_p1_stress.c src/util.c src/shm.c src/map.c src/instr.c -o /tmp/test_p1_stress
/tmp/test_p1_stress
```

### Power-ups `+` (opcional de la sección 7)

Si Bomberman pisa un `+`, su siguiente bomba tiene alcance 3 en vez de 2. `map.c` carga los power-ups, P1 los recoge al moverse, P3 marca la bomba al registrarla y la explosión usa `BOMB_RANGE + 1`.

```bash
./bomberman tests/13_powerup | grep -E 'P1|P3'
```

## Pruebas

```bash
make test        # los 15 casos de tests/; compara con expected.txt y checks.txt
make test_base   # prueba unitaria del mapa y del parser
make valgrind    # memcheck en todos los procesos (fugas y accesos inválidos)
make helgrind    # detector de carreras de valgrind
make tsan        # recompila con -fsanitize=thread y corre los casos
```

En WSL, ThreadSanitizer necesita desactivar ASLR: `setarch $(uname -m) -R make tsan`.

Cada caso tiene un límite de tiempo (`CASE_TIMEOUT`, 120 s por defecto): si algún proceso se cuelga, el caso falla en vez de dejar `make test` bloqueado.

| Caso | Qué demuestra |
|---|---|
| 01_ejemplo_8_5 | La tabla de la sección 8.5, tick por tick, con el mapa del enunciado |
| 02_round_robin | La variante con prioridades iguales de la sección 8.5 |
| 03_inanicion | P1 con menor prioridad nunca recibe turno |
| 04_reaccion_cadena | 2 bombas en cadena; Bomberman en el fuego de ambas pierde 1 sola vida |
| 05_colision_y_explosion | Colisión y explosión en el mismo tick: 1 sola vida |
| 06_prioridad_y_lineas_invalidas | `SET_PRIORITY` fuera de rango, líneas desconocidas, dos enemigos piden prioridad y gana el de mayor número |
| 07_max_bombs | `MAX_BOMBS` y bomba sobre otra bomba (usa `bomberman_t6`) |
| 08a–08d | Victoria, derrota, `max_ticks` y archivos agotados |
| 09_paredes_y_celdas | El fuego no atraviesa paredes; nadie entra a `X # Q *` |
| 10_aging | Opcional: aging contra la inanición |
| 11_reporte_scheduler | Mejora de P0: el reporte mide la inanición (P1 con 0 turnos y una racha de 10) |
| 13_powerup | Mejora de P3: la bomba con power-up tiene alcance 3 |
| unit/test_base.c | Mapa y parser (26 comprobaciones) |
| unit/test_p1_stress.c | Mejora de P1: 5000 instrucciones con una cola de 4 espacios |

**Por qué existe `bomberman_t6`:** con `BOMB_TIMER = 3` es imposible pedir una tercera bomba mientras hay 2 activas. Bomberman hace una acción por tick y no puede poner dos bombas en la misma celda, así que la segunda bomba llega como pronto en el tick t+2 y la tercera en t+4. Para entonces la primera ya explotó en t+3, en el paso 3, antes del registro del paso 4. Por eso la prueba 07 usa un binario compilado con `-DBOMB_TIMER=6`. En el código, `BOMB_TIMER` sigue siendo una constante.

## Estructura

```
include/common.h       constantes, struct shared_state_t, orden de mutex, prototipos
src/main.c             arranque de P0: config, shm, mapa, dup/dup2, SIGINT, fork
src/p0_scheduler.c     P0: tick, scheduler, signal, event (prioridades, RR, aging, cierre, reporte)
src/p1_bomber.c        P1: reader, executor, publisher (cola productor-consumidor con métricas)
src/p2_enemy.c         P2: controller, enemy_thread x4, tracker, collision, publisher
src/p3_bomb.c          P3: controller, fuse, explosion (cadena, bloques, daño, power-ups)
src/p4_renderer.c      P4: render ANSI sincronizado al tick
src/shm.c              shm_open/ftruncate/mmap, sem_init(pshared=1), mutex compartidos
src/map.c              carga y validación de map.txt, power-ups
src/instr.c            parser de instrucciones
scripts/run_tests.sh   corre los casos y los compara con expected.txt y checks.txt
tests/                 15 casos + pruebas unitarias (tests/unit/)
```

## Decisiones de diseño (aprobadas por el grupo)

1. **Colisión literal.** Si un enemigo sigue en la celda de Bomberman, se pierde una vida en cada turno de P2. Bomberman no reaparece; el enunciado no lo pide.
2. **P4 con ANSI.** El renderer escribe directamente en la terminal con secuencias ANSI, sincronizado al final de cada tick con `sem_render`/`sem_render_done`.
3. **Solo `fork()`.** Un único binario: cada hijo ejecuta su `pX_main()`.
4. **Enemigo ausente.** Si su letra no está en `map.txt`, empieza eliminado, no lee instrucciones y no participa en colisiones.
5. **Empate de prioridades.** Round Robin que empieza con P1, igual que en la sección 8.5.
6. **Línea desconocida.** Se reporta en consola y se descarta sin consumir turno.
7. **Fuego y bloques.** Un bloque `#` alcanzado pasa a `O` (no a `*`) y detiene el fuego en esa dirección. El fuego que llega a otra bomba la hace explotar en el mismo tick.
8. **Hilos internos con semáforos privados.** Dentro de cada proceso, los hilos se coordinan con semáforos `pshared = 0`. Solo un hilo por proceso usa `sem_turn_done` para avisar a P0, y al terminar el juego ese mismo hilo detiene a los demás.

## Orden de los mutex (evita deadlocks)

```
mutex locales del proceso  ->  m_pos -> m_map -> m_mailbox -> m_events -> m_state
```
