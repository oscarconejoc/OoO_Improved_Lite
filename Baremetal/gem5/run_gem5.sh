#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<EOF
Uso: $0 [opciones]

Opciones:
  --gem5-bin=PATH     Ruta al binario de gem5
  --gem5-src=PATH      Ruta a la raíz del checkout de gem5
  --config=PATH        Ruta explícita a un script de config de gem5 (.py)
  --reset-vect=ADDR    Dirección de arranque del CPU (default: 0x80000000)
  --maxinsts=N         Máximo de instrucciones (default: 5000000)
  --riscv-32bits       Fuerza ejecución RV32 (default: habilitado)
  --no-riscv-32bits    Deshabilita la bandera --riscv-32bits
  -h, --help           Muestra esta ayuda
EOF
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${TARGET_BIN:-${ROOT_DIR}/build/bare_metal_test}"
OUTDIR="${OUTDIR:-${ROOT_DIR}/gem5/out}"

GEM5_BIN="${GEM5_BIN:-}"
GEM5_SRC="${GEM5_SRC:-}"
GEM5_CONFIG="${GEM5_CONFIG:-}"
RESET_VECT="${RESET_VECT:-0x80000000}"
MAXINSTS="${MAXINSTS:-5000000}"
RISCV_32BITS="${RISCV_32BITS:-1}"

for arg in "$@"; do
  case "$arg" in
    --gem5-bin=*) GEM5_BIN="${arg#*=}" ;;
    --gem5-src=*) GEM5_SRC="${arg#*=}" ;;
    --config=*) GEM5_CONFIG="${arg#*=}" ;;
    --reset-vect=*) RESET_VECT="${arg#*=}" ;;
    --maxinsts=*) MAXINSTS="${arg#*=}" ;;
    --riscv-32bits) RISCV_32BITS="1" ;;
    --no-riscv-32bits) RISCV_32BITS="0" ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "Opción desconocida: $arg" >&2; usage; exit 1 ;;
    *) GEM5_BIN="$arg" ;;
  esac
done

if [[ ! -f "$BIN" ]]; then
  echo "Binary not found at $BIN" >&2
  echo "Build the project first with: make" >&2
  exit 1
fi

if [[ -z "$GEM5_BIN" ]]; then
  for candidate in \
    "${ROOT_DIR}/../build/RISCV/gem5.opt" \
    "${ROOT_DIR}/../build/RISCV/gem5" \
    "$(command -v gem5.opt || true)" \
    "$(command -v gem5 || true)"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      GEM5_BIN="$candidate"
      break
    fi
  done
fi

if [[ -z "$GEM5_BIN" || ! -x "$GEM5_BIN" ]]; then
  echo "gem5 binary not found o no ejecutable: '$GEM5_BIN'" >&2
  echo "Pasa la ruta con --gem5-bin=PATH, exporta GEM5_BIN, o instala gem5." >&2
  exit 1
fi

GEM5_BIN="$(cd "$(dirname "$GEM5_BIN")" && pwd)/$(basename "$GEM5_BIN")"

if [[ -z "$GEM5_SRC" ]]; then
  GEM5_SRC="$(cd "$(dirname "$GEM5_BIN")/../.." && pwd)"
fi

if [[ -z "$GEM5_CONFIG" ]]; then
  for candidate in \
    "${GEM5_SRC}/configs/deprecated/example/riscv/fs_linux.py" \
    "${GEM5_SRC}/configs/example/riscv/fs_linux.py"; do
    if [[ -f "$candidate" ]]; then
      GEM5_CONFIG="$candidate"
      break
    fi
  done
fi

if [[ -z "$GEM5_CONFIG" ]]; then
  echo "No se encontró un config de gem5 automáticamente." >&2
  echo "Pasa la ruta explícita con --config=PATH." >&2
  exit 1
fi

if [[ ! -f "$GEM5_CONFIG" ]]; then
  echo "El archivo de configuración no existe: $GEM5_CONFIG" >&2
  exit 1
fi

CONFIG_BASENAME="$(basename "$GEM5_CONFIG")"

if [[ "$CONFIG_BASENAME" != "simple_riscv_baremetal.py" ]]; then
  OPTIONS_PY="$(find "${GEM5_SRC}/configs" -maxdepth 4 -path '*/common/Options.py' -print -quit)"
  if [[ -z "$OPTIONS_PY" ]]; then
    echo "No pude localizar configs*/common/Options.py bajo ${GEM5_SRC}/configs" >&2
    exit 1
  fi
  COMMON_PARENT_DIR="$(dirname "$(dirname "$OPTIONS_PY")")"
  export PYTHONPATH="${COMMON_PARENT_DIR}:${PYTHONPATH:-}"
  echo "PYTHONPATH ajustado con: $COMMON_PARENT_DIR"
fi

BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"

echo "Running gem5 with: $BIN"
echo "Using gem5 binary: $GEM5_BIN"
echo "Using gem5 source: $GEM5_SRC"
echo "Using config: $GEM5_CONFIG"
echo "Reset vector: $RESET_VECT"
echo "Max insts: $MAXINSTS"
echo "Force RV32: $RISCV_32BITS"
echo "Output dir: $OUTDIR"

cd "$GEM5_SRC"

if [[ "$CONFIG_BASENAME" == "simple_riscv_baremetal.py" ]]; then
  "$GEM5_BIN" \
    --outdir="$OUTDIR" \
    "$GEM5_CONFIG" \
    --binary="$BIN" \
    --maxinsts="$MAXINSTS"
else
  EXTRA_ARGS=()
  if [[ "$RISCV_32BITS" == "1" ]]; then
    EXTRA_ARGS+=(--riscv-32bits)
  fi
  if [[ -n "$MAXINSTS" ]]; then
    EXTRA_ARGS+=(--maxinsts="$MAXINSTS")
  fi

  "$GEM5_BIN" \
    --outdir="$OUTDIR" \
    "$GEM5_CONFIG" \
    --bare-metal \
    --kernel="$BIN" \
    --cpu-type=AtomicSimpleCPU \
    --mem-size=512MB \
    "${EXTRA_ARGS[@]}" \
    --param "system.workload.reset_vect=${RESET_VECT}"
fi