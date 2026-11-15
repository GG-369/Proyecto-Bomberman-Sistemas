# P2 · Bomberman concurrente en C con POSIX

IS2021 Computing Systems · UTEC · 2026-2
Grupo: Leon Pantaleon, Jobeth Alexander · Gamboa Sanchez, Guillermo Valentino Ceceu · Solis Cordova, Pedro Nicolas

Simulación de Bomberman con **5 procesos** (P0 scheduler, P1 bomber, P2 enemigos, P3 bombas y P4 renderer como bonus) y **19 hilos**. Se comunican **solo por memoria compartida POSIX**, con semáforos (`pshared = 1`) y mutex `PTHREAD_PROCESS_SHARED` guardados dentro de esa misma memoria.

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

Ctrl+C termina la simulación en orden: P0 marca `game_over`, despierta a los hijos, hace `waitpid` y borra la memoria compartida.

### Carpeta de un caso

| Archivo | Contenido |
|---|---|
| `map.txt` | Mapa con `X # O P A B C D` (máximo 32×32) |
| `bomber_moves.txt` | `UP DOWN LEFT RIGHT BOMB SET_PRIORITY <n>` |
| `enemy_1..4_moves.txt` | Igual, pero sin `BOMB` |
| `config.txt` (opcional) | `max_ticks 20`, `prio_bomber 40`, `prio_enemy 30`, `aging_ticks 0` |
| `expected.txt` | Líneas `RESUMEN`/`RESULTADO` que debe producir el caso |
| `checks.txt` | Frases que deben aparecer en el log |

## Pruebas

```bash
make test        # 13 casos; compara con expected.txt y checks.txt
make valgrind    # memcheck en los 4 procesos (fugas y accesos inválidos)
make helgrind    # detector de carreras de valgrind
make tsan        # recompila con -fsanitize=thread y corre los 13 casos
```

En la VM de Cowork, TSan necesita desactivar ASLR: `setarch $(uname -m) -R make tsan`.

| Caso | Qué demuestra |
|---|---|
| 01_ejemplo_8_5 | La tabla de la sección 8.5, tick por tick |
| 02_round_robin | La variante con prioridades iguales de la sección 8.5 |
| 03_inanicion | P1 con menor prioridad nunca recibe turno |
| 04_reaccion_cadena | 2 bombas en cadena; Bomberman en el fuego de ambas pierde 1 sola vida |
| 05_colision_y_explosion | Colisión y explosión en el mismo tick: 1 sola vida |
| 06_prioridad_y_lineas_invalidas | `SET_PRIORITY` fuera de rango, líneas desconocidas, dos enemigos piden prioridad |
| 07_max_bombs | `MAX_BOMBS` y bomba sobre otra bomba (usa `bomberman_t6`) |
| 08a–08d | Victoria, derrota, `max_ticks` y archivos agotados |
| 09_paredes_y_celdas | El fuego no atraviesa paredes; nadie entra a `X # Q *` |
| 10_aging | Opcional: aging contra la inanición |

**Por qué existe `bomberman_t6`:** con `BOMB_TIMER = 3` es imposible pedir una tercera bomba mientras hay 2 activas. Bomberman hace una acción por tick y no puede poner dos bombas en la misma celda, así que la segunda bomba llega como pronto en el tick t+2 y la tercera en t+4. Para entonces la primera ya explotó en t+3, en el paso 3, antes del registro del paso 4. Por eso la prueba 07 usa un binario compilado con `-DBOMB_TIMER=6`. En el código, `BOMB_TIMER` sigue siendo una constante.

## Estructura

```
include/common.h     constantes, struct shared_state_t, orden de mutex, prototipos
src/main.c           arranque de P0: config, shm, mapa, dup/dup2, SIGINT, fork
src/p0_scheduler.c   P0: tick, scheduler, signal, event (prioridades, RR, aging, cierre)
src/p1_bomber.c      P1: reader, executor, publisher (cola productor-consumidor)
src/p2_enemy.c       P2: controller, enemy_thread_1..4, tracker, collision, publisher
src/p3_bomb.c        P3: controller, fuse, explosion (cadena, bloques, daño)
src/p4_renderer.c    P4: render ANSI sincronizado al tick
src/shm.c            shm_open/ftruncate/mmap, sem_init(pshared=1), mutex compartidos
src/map.c            carga y validación de map.txt
src/instr.c          parser de instrucciones
tests/               13 casos + prueba unitaria de la base (make test_base)
docs/                diagramas, informe, guías por integrante y guía de exposición
```

## Decisiones de diseño (aprobadas por el grupo)

1. **Colisión literal.** Si un enemigo sigue en la celda de Bomberman, se pierde una vida en cada turno de P2. Bomberman no reaparece; el enunciado no lo pide.
2. **P4 con ANSI.** Cada cuadro se escribe completo con un solo `write()`, sin borrar la pantalla, así que no parpadea.
3. **Solo `fork()`.** Un único binario: cada hijo ejecuta su `pX_main()`.
4. **Enemigo ausente.** Si su letra no está en `map.txt`, empieza eliminado y se muestra un aviso.
5. **Empate de prioridades.** Round Robin que empieza con P1, igual que en la sección 8.5.
6. **Línea desconocida.** Se reporta y se lee la siguiente en el mismo turno; no consume turno.
7. **Fuego y bloques.** Un bloque `#` destruido pasa a `O` (no a `*`) y frena el fuego de todas las bombas de ese tick. El fuego que llega a otra bomba la hace explotar en el mismo tick.

## Orden de los mutex (evita deadlocks)

```
mutex locales del proceso  ->  m_pos -> m_map -> m_mailbox -> m_events -> m_state
```
