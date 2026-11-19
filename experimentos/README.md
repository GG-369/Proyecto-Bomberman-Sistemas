# Experimentos: con y sin cada primitiva de sincronización

Evidencia de la sección 5 del informe. Cada experimento compara el código real con una variante a la que se le quita **una sola** primitiva, y repite cada versión cientos de veces. El código del juego (`src/`) **no** contiene estas variantes: `variantes.py` las agrega con `#ifdef` sobre una copia en `/tmp`.

| Experimento | Variante sin la primitiva | Métrica | Repeticiones |
|---|---|---|---|
| E1 · barrera `workers_done` de P2 | `-DEXP_NO_BARRIER`: la colisión y la publicación corren sin esperar a los 4 enemigos | lecturas a medio turno (contador atómico de enemigos que aún se mueven) y salida distinta de `expected.txt` | 500 por caso (02, 05, 08b) y variante |
| E2 · `queue.mutex` de P1 | `-DEXP_NO_QUEUE_MUTEX`: la cola real de `p1_bomber.c` sin su mutex (`exp_queue.c`, 50 000 instrucciones, cola de 4) | instrucciones perdidas, `count` final, ejecuciones colgadas | 500 por variante |
| E3 · cierre de P3 | versión `d2c520a` (antes del PR #13) contra la versión final | ejecuciones colgadas (timeout externo de 10 s) y tiempo hasta terminar | 100 por caso y versión (15 casos) |
| E4 · `release_children` de P0 | `-DEXP_NO_RELEASE`: P0 no despierta a los hijos al terminar | ejecuciones colgadas | 50 por variante |

## Cómo reproducir

```bash
./experimentos/run_experimentos.sh                    # desde la raíz del repo; deja los CSV en experimentos/data
N=100 ./experimentos/run_experimentos.sh              # versión rápida
python3 experimentos/graficos.py experimentos/data /tmp/figuras     # figuras 5, 6 y 8-10 + resumen_exp.json
python3 experimentos/diagramas.py /tmp/figuras                      # figuras 1, 2, 4 y 7 (diagramas)
```

## Archivos

| Archivo | Contenido |
|---|---|
| `run_experimentos.sh` | Compila las versiones en `/tmp/bomberman-exp` y corre E1–E4, recursos por caso y los `RESUMEN` de los casos 01, 02 y 10 |
| `variantes.py` | Agrega a una copia de `src/` la instrumentación (contador atómico en P2) y las banderas `EXP_NO_BARRIER`, `EXP_NO_QUEUE_MUTEX` y `EXP_NO_RELEASE` |
| `exp_queue.c` | Incluye el `p1_bomber.c` real y prueba su cola con 1 productor y 1 consumidor |
| `graficos.py`, `diagramas.py` | Generan las figuras del informe |
| `data/` | Datos crudos usados en el informe (CSV) y su resumen (`resumen_exp.json`) |

## Resultados (datos de `data/`)

| Experimento | Sin la primitiva | Con la primitiva |
|---|---|---|
| E1 barrera | lecturas sucias en el 64,8 % (caso 02), 4,6 % (05) y 4,0 % (08b) de las partidas; 54 de 1500 con salida incorrecta | 0 lecturas sucias; 1500/1500 correctas |
| E2 cola | 0 % de ejecuciones íntegras; 47 955 instrucciones perdidas de media; 22 colgadas | 500/500 íntegras |
| E3 cierre de P3 | 58/1500 colgadas (3,9 %) | 0/1500 |
| E4 `release_children` | 50/50 colgadas | 0/50 |

**Entorno de medición:** Intel Core i7-13620H (16 hilos), 7,6 GB de RAM en WSL2, Ubuntu 24.04.3 LTS, gcc 13.3.0. Los datos se tomaron sobre el commit `3a5347d`, cuyo código de los procesos es el mismo que el de la versión final (los cambios posteriores solo agregan validaciones de entrada y pruebas). Al tratarse de errores de concurrencia, los porcentajes varían un poco entre máquinas y ejecuciones; lo que se mantiene es la diferencia entre la versión con la primitiva (siempre 0 fallos) y la versión sin ella.
