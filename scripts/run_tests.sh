#!/usr/bin/env bash
# scripts/run_tests.sh - Ejecuta la batería de pruebas y compara con expected.txt/checks.txt
#
# Uso: ./scripts/run_tests.sh [tsan|valgrind|helgrind]
#   sin args: ejecución normal
#   tsan:     usa bomberman compilado con -fsanitize=thread
#   valgrind: memcheck en los 4 procesos
#   helgrind: detector de carreras

set -euo pipefail

MODE="${1:-normal}"
BIN="./bomberman"
BIN_T6="./bomberman_t6"

# Colores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_err()  { echo -e "${RED}[ERR]${NC} $*"; }

# Verificar binarios
if [[ ! -x "$BIN" ]]; then
    log_err "No se encuentra $BIN. Ejecuta 'make' primero."
    exit 1
fi

# Modo TSan necesita bomberman (no bomberman_t6) recompilado con sanitizer
if [[ "$MODE" == "tsan" ]]; then
    if [[ ! -x "$BIN" ]]; then
        log_err "Para tsan necesitas: make tsan (recompila con -fsanitize=thread)"
        exit 1
    fi
fi

# Descubrir casos de prueba
TEST_DIRS=(tests/*/)
if [[ ! -d "${TEST_DIRS[0]}" ]]; then
    log_err "No hay casos en tests/"
    exit 1
fi

TOTAL=0
PASSED=0
FAILED=0

run_case() {
    local case_dir="$1"
    local case_name=$(basename "$case_dir")
    local use_t6=0

    # Caso 07 usa bomberman_t6 (BOMB_TIMER=6)
    if [[ "$case_name" == "07_max_bombs" ]]; then
        use_t6=1
    fi

    local bin="$BIN"
    [[ $use_t6 -eq 1 ]] && bin="$BIN_T6"

    if [[ ! -x "$bin" ]]; then
        log_err "[$case_name] Binario no encontrado: $bin"
        return 1
    fi

    # Archivos requeridos
    local map_file="$case_dir/map.txt"
    local bomber_moves="$case_dir/bomber_moves.txt"
    local expected="$case_dir/expected.txt"
    local checks="$case_dir/checks.txt"

    for f in "$map_file" "$bomber_moves" "$expected" "$checks"; do
        if [[ ! -f "$f" ]]; then
            log_err "[$case_name] Falta archivo: $f"
            return 1
        fi
    done

    # Ejecutar
    local log_file="logs/${case_name}.log"
    mkdir -p logs

    local cmd=("$bin" "$case_dir")
    if [[ "$MODE" == "valgrind" ]]; then
        cmd=(valgrind --leak-check=full --error-exitcode=42 --log-file="logs/${case_name}.valgrind" "$bin" "$case_dir")
    elif [[ "$MODE" == "helgrind" ]]; then
        cmd=(valgrind --tool=helgrind --error-exitcode=42 --log-file="logs/${case_name}.helgrind" "$bin" "$case_dir")
    fi

    log_info "[$case_name] Ejecutando: ${cmd[*]}"
    # Límite de tiempo: si algún proceso se cuelga (deadlock), el caso falla
    # en lugar de dejar make test bloqueado para siempre.
    timeout "${CASE_TIMEOUT:-120}" "${cmd[@]}" >"$log_file" 2>&1
    local exit_code=$?
    if [[ $exit_code -eq 124 ]]; then
        log_err "[$case_name] Se colgó: superó ${CASE_TIMEOUT:-120} s (posible deadlock)"
        return 1
    fi

    if [[ "$MODE" == "valgrind" || "$MODE" == "helgrind" ]]; then
        if [[ $exit_code -eq 42 ]]; then
            log_err "[$case_name] $MODE detectó errores (ver logs/${case_name}.$MODE)"
            return 1
        fi
    elif [[ $exit_code -ne 0 ]]; then
        log_err "[$case_name] Binario terminó con código $exit_code"
        return 1
    fi

    # Comparar RESUMEN/RESULTADO con expected.txt
    local actual_summary=$(grep -E '^RESUMEN|^RESULTADO' "$log_file" || true)
    local expected_summary=$(cat "$expected")

    if [[ "$actual_summary" != "$expected_summary" ]]; then
        log_err "[$case_name] Salida no coincide con expected.txt"
        echo "  --- ESPERADO ---"
        echo "$expected_summary"
        echo "  --- OBTENIDO ---"
        echo "$actual_summary"
        return 1
    fi

    # Verificar checks.txt (frases que deben aparecer en el log)
    while IFS= read -r pattern; do
        [[ -z "$pattern" || "$pattern" =~ ^# ]] && continue
        if ! grep -qF "$pattern" "$log_file"; then
            log_err "[$case_name] Check fallido: '$pattern' no encontrado en log"
            return 1
        fi
    done < "$checks"

    log_ok "[$case_name] PASÓ"
    return 0
}

# Main loop
for case_dir in "${TEST_DIRS[@]}"; do
    case_name=$(basename "$case_dir")
    [[ "$case_name" == "*" ]] && continue
    # tests/unit tiene la prueba unitaria (make test_base), no es un caso de simulación
    [[ "$case_name" == "unit" ]] && continue

    TOTAL=$((TOTAL + 1))
    if run_case "$case_dir"; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
    fi
done

echo
echo "==================== RESUMEN ===================="
echo "Total:  $TOTAL"
echo "Pasaron: $PASSED"
echo "Fallaron: $FAILED"

if [[ $FAILED -eq 0 ]]; then
    log_ok "TODOS LOS TESTS PASARON"
    exit 0
else
    log_err "ALGUNOS TESTS FALLARON"
    exit 1
fi