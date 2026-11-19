#!/usr/bin/env bash
# Experimentos de la sección 5 del informe: cada primitiva de sincronización
# con y sin ella. No modifica el repositorio: trabaja sobre copias en $WORK.
#
# Uso:  ./experimentos/run_experimentos.sh            (desde la raíz del repo)
#       N=100 ./experimentos/run_experimentos.sh      (menos repeticiones)
# Requiere: gcc, python3, timeout, /usr/bin/time, git.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
EXP=$REPO/experimentos
OUT=${OUT:-$EXP/data}
WORK=${WORK:-/tmp/bomberman-exp}
N=${N:-500}              # repeticiones por variante (barrera y cola)
N_CIERRE=${N_CIERRE:-100} # repeticiones por caso en el experimento de cierre
VERSION_FINAL=${VERSION_FINAL:-HEAD}
VERSION_ANTERIOR=d2c520a  # antes del arreglo del cierre de P3 (PR #13)
CF="-std=gnu11 -O2 -Wall -Wextra -pthread -Iinclude -I."

mkdir -p "$OUT"; rm -rf "$WORK"; mkdir -p "$WORK"/{var,final,anterior}
cd "$REPO"
git archive "$VERSION_FINAL"    | tar -x -C "$WORK/var"
git archive "$VERSION_FINAL"    | tar -x -C "$WORK/final"
git archive "$VERSION_ANTERIOR" | tar -x -C "$WORK/anterior"

for v in final anterior; do
    (cd "$WORK/$v" && gcc $CF src/*.c -o bm && gcc $CF -DBOMB_TIMER=6 src/*.c -o bm6) || exit 1
done
cd "$WORK/var"
python3 "$EXP/variantes.py" "$WORK/var" || exit 1
gcc $CF src/*.c -o bm_base
gcc $CF -DEXP_NO_BARRIER src/*.c -o bm_nobar
gcc $CF -DEXP_NO_RELEASE -Wno-unused-function src/*.c -o bm_norel
gcc $CF -DP1_QUEUE_CAPACITY=4 -Wno-unused-function "$EXP/exp_queue.c" src/util.c src/shm.c src/map.c src/instr.c -o q_mutex
gcc $CF -DP1_QUEUE_CAPACITY=4 -Wno-unused-function -DEXP_NO_QUEUE_MUTEX "$EXP/exp_queue.c" src/util.c src/shm.c src/map.c src/instr.c -o q_nomutex
limpiar() { pkill -x "$1" 2>/dev/null; rm -f /dev/shm/bomberman_p2_shm; }

# E1: barrera workers_done de P2 (casos con turnos de P2)
echo "variante,caso,iter,dirty,turns,correcto" > "$OUT/e1_barrera.csv"
for v in base nobar; do for c in 02_round_robin 05_colision_y_explosion 08b_derrota; do
    for i in $(seq 1 "$N"); do
        timeout 10 ./bm_$v tests/$c > /tmp/e1o.txt 2>/tmp/e1e.txt || limpiar bm_$v
        d=$(grep -o 'dirty=[0-9]*' /tmp/e1e.txt | cut -d= -f2); t=$(grep -o 'turns=[0-9]*' /tmp/e1e.txt | cut -d= -f2)
        ok=0; grep -E '^(RESUMEN|RESULTADO)' /tmp/e1o.txt | cmp -s - tests/$c/expected.txt && ok=1
        echo "$v,$c,$i,${d:-NA},${t:-NA},$ok" >> "$OUT/e1_barrera.csv"
    done; done; done
echo "E1 listo"

# E2: mutex de la cola de P1 (50 000 instrucciones, cola de 4)
echo "variante,iter,consumidas,perdidas,fifo_err,count_final,corte,rc" > "$OUT/e2_cola.csv"
for v in mutex nomutex; do for i in $(seq 1 "$N"); do
    r=$(timeout 10 ./q_$v); rc=$?; l=$(echo "$r" | grep EXPQ)
    if [ -n "$l" ]; then
        echo "$v,$i,$(echo "$l" | sed -E 's/.*consumidas=([-0-9]+) perdidas=([-0-9]+) fifo_err=([-0-9]+) count_final=([-0-9]+) corte=([-0-9]+)/\1,\2,\3,\4,\5/'),$rc" >> "$OUT/e2_cola.csv"
    else echo "$v,$i,NA,NA,NA,NA,NA,$rc" >> "$OUT/e2_cola.csv"; fi
done; done
echo "E2 listo"

# E3: cierre de P3, versión anterior al arreglo contra la final
echo "version,caso,iter,ms,colgado" > "$OUT/e3_cierre.csv"
for v in anterior final; do cd "$WORK/$v"
    for c in $(ls tests | grep -v unit); do
        [ -f tests/$c/expected_rc.txt ] && continue
        b=./bm; [ -f tests/$c/binary.txt ] && b=./bm6
        for i in $(seq 1 "$N_CIERRE"); do
            t0=$(date +%s%N); timeout 10 $b tests/$c >/dev/null 2>&1; rc=$?; t1=$(date +%s%N)
            h=0; [ $rc -eq 124 ] && { h=1; limpiar bm; limpiar bm6; }
            echo "$([ $v = anterior ] && echo old || echo clean),$c,$i,$(( (t1-t0)/1000000 )),$h" >> "$OUT/e3_cierre.csv"
        done; done; done
echo "E3 listo"

# E4: cierre sin release_children
cd "$WORK/var"
echo "variante,iter,ms,colgado" > "$OUT/e4_release.csv"
for v in base norel; do for i in $(seq 1 50); do
    t0=$(date +%s%N); timeout 3 ./bm_$v tests/08c_max_ticks >/dev/null 2>&1; rc=$?; t1=$(date +%s%N)
    h=0; [ $rc -eq 124 ] && { h=1; limpiar bm_$v; }
    echo "$v,$i,$(( (t1-t0)/1000000 )),$h" >> "$OUT/e4_release.csv"
done; done
echo "E4 listo"

# Recursos por caso y RESUMEN para los diagramas temporales
cd "$WORK/final"
echo "caso,iter,seg,cpu,maxrss_kb" > "$OUT/recursos.csv"
for c in $(ls tests | grep -v unit); do
    [ -f tests/$c/expected_rc.txt ] && continue
    b=./bm; [ -f tests/$c/binary.txt ] && b=./bm6
    for i in $(seq 1 20); do
        /usr/bin/time -f "%e,%P,%M" -o /tmp/tm.txt $b tests/$c >/dev/null 2>&1
        echo "$c,$i,$(tail -1 /tmp/tm.txt | tr -d '%')" >> "$OUT/recursos.csv"
    done; done
for c in 01_ejemplo_8_5 02_round_robin 10_aging; do
    ./bm tests/$c 2>/dev/null | grep -E '^(RESUMEN|RESULTADO)' > "$OUT/resumen_${c%%_*}.txt"
done
echo "Datos en $OUT. Figuras: python3 experimentos/graficos.py $OUT <carpeta_salida>"
