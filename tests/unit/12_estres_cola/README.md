# T11 - Estrés de cola productor-consumidor (Guillermo)

Este caso se encuentra dentro de `tests/unit/` para **no alterar** el runner
`make test` (propiedad de Pedro), que espera `map.txt` y `expected.txt` por
cada carpeta directamente dentro de `tests/`.

- Entrada: `bomber_moves.txt` (exactamente 5000 instrucciones LEFT/RIGHT).
- Prueba: `tests/unit/test_p1_stress.c` incorpora el código real `src/p1_bomber.c`.
- Compilación: `-DP1_QUEUE_CAPACITY=4`.
- Verifica FIFO por `line_no` y tipo, 5000 inserciones/extracciones, ocupación
  máxima <= 4, detección de cola llena y cola vacía, y EOF sin bloqueo.

Comandos desde la raíz del repositorio:

```bash
gcc -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
  -pthread -DP1_QUEUE_CAPACITY=4 -Iinclude \
  tests/unit/test_p1_stress.c src/util.c src/shm.c src/map.c src/instr.c \
  -o /tmp/test_p1_stress
timeout 20s /tmp/test_p1_stress
```

Esta es una prueba *aislada de la cola*, no una simulación de 5000 ticks
completos con P0/P2/P3. En el programa real, P1 imprime un resumen de la cola
al terminar (`[P1] COLA ...`), para ayudar en la defensa.
