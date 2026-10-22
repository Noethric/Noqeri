#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT/build-native-selfhost}"
HOST_BUILD="${NOQERI_HOST_BUILD:-$BUILD_DIR/bootstrap-host}"
SEED_COMPILER="${NOQERI_SEED_COMPILER:-}"
NQRI_RUNTIME="${NOQERI_NQRI_RUNTIME:-}"
CXX="${CXX:-c++}"
MANIFEST="$ROOT/Compiler/selfhost/native-stage-sources.txt"
NATIVE_SEED="$ROOT/Compiler/selfhost/native_seed.noqeri"
HOST_SOURCE="$ROOT/Compiler/bootstrap/native_compiler_host.cpp"

if [ "$(uname -s 2>/dev/null || true)" != "Linux" ]; then
  echo "Native self-host qualification currently requires Linux x86-64." >&2
  exit 2
fi
case "$(uname -m 2>/dev/null || true)" in
  x86_64|amd64) ;;
  *) echo "Native self-host qualification currently requires Linux x86-64." >&2; exit 2 ;;
esac

mkdir -p "$BUILD_DIR"

if [ -z "$SEED_COMPILER" ]; then
  for candidate in "$HOST_BUILD/noqeri" "$HOST_BUILD/Release/noqeri" "$ROOT/build/noqeri"; do
    if [ -x "$candidate" ]; then SEED_COMPILER="$candidate"; break; fi
  done
fi
if [ -z "$NQRI_RUNTIME" ]; then
  for candidate in "$HOST_BUILD/noqeri-nqri" "$HOST_BUILD/Release/noqeri-nqri" "$ROOT/build/noqeri-nqri"; do
    if [ -x "$candidate" ]; then NQRI_RUNTIME="$candidate"; break; fi
  done
fi

if [ -z "$SEED_COMPILER" ] || [ -z "$NQRI_RUNTIME" ]; then
  command -v cmake >/dev/null 2>&1 || { echo "cmake is required for Stage-0 bootstrap." >&2; exit 2; }
  cmake -S "$ROOT" -B "$HOST_BUILD" -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build "$HOST_BUILD" --target noqeri noqeri-nqri --config Release --parallel 2
  [ -x "$HOST_BUILD/noqeri" ] && SEED_COMPILER="$HOST_BUILD/noqeri"
  [ -x "$HOST_BUILD/Release/noqeri" ] && SEED_COMPILER="$HOST_BUILD/Release/noqeri"
  [ -x "$HOST_BUILD/noqeri-nqri" ] && NQRI_RUNTIME="$HOST_BUILD/noqeri-nqri"
  [ -x "$HOST_BUILD/Release/noqeri-nqri" ] && NQRI_RUNTIME="$HOST_BUILD/Release/noqeri-nqri"
fi

[ -x "$SEED_COMPILER" ] || { echo "Stage-0 Noqeri seed compiler is missing." >&2; exit 2; }
[ -x "$NQRI_RUNTIME" ] || { echo "NQRI bootstrap runtime is missing." >&2; exit 2; }
command -v "$CXX" >/dev/null 2>&1 || { echo "$CXX is required as the system assembler/linker driver." >&2; exit 2; }

STAGE1="$BUILD_DIR/noqeri-native-stage1.nqri"
STAGE2_ASM="$BUILD_DIR/noqeri-native-stage2.s"
STAGE2="$BUILD_DIR/noqeri-native-stage2"
STAGE3_ASM="$BUILD_DIR/noqeri-native-stage3.s"
STAGE3="$BUILD_DIR/noqeri-native-stage3"
STAGE4_ASM="$BUILD_DIR/noqeri-native-stage4.s"
SMOKE="$BUILD_DIR/native-selfhost-smoke"
PROOF="$BUILD_DIR/native-selfhost-proof.txt"

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

echo "[1/8] Building current-source Stage-1 NQRI seed..."
"$SEED_COMPILER" bootstrap-seed "$NATIVE_SEED" "$STAGE1"
"$NQRI_RUNTIME" has-export "$STAGE1" stage2CompileToNativeAssembly

echo "[2/8] Stage-1 NQRI seed emits native Stage-2 assembly..."
"$NQRI_RUNTIME" compile-native-manifest "$STAGE1" "$ROOT" "$MANIFEST" "$STAGE2_ASM" 3
test -s "$STAGE2_ASM"

echo "[3/8] Linking native Stage-2 compiler..."
"$CXX" -std=c++17 -O2 "$HOST_SOURCE" "$STAGE2_ASM" -o "$STAGE2"
"$STAGE2" check "$ROOT/tests/native_selfhost_smoke.noqeri" 3

echo "[4/8] Native Stage-2 rebuilds compiler as Stage-3 assembly..."
"$STAGE2" compile-manifest "$ROOT" "$MANIFEST" "$STAGE3_ASM" 3
test -s "$STAGE3_ASM"
if ! cmp -s "$STAGE2_ASM" "$STAGE3_ASM"; then
  echo "Native self-host FAILED: Stage-1-produced Stage-2 assembly and Stage-2-produced Stage-3 assembly differ." >&2
  cmp -l "$STAGE2_ASM" "$STAGE3_ASM" | head -n 20 >&2 || true
  exit 1
fi

echo "[5/8] Linking native Stage-3 compiler..."
"$CXX" -std=c++17 -O2 "$HOST_SOURCE" "$STAGE3_ASM" -o "$STAGE3"

echo "[6/8] Native Stage-3 rebuilds itself without NQRI execution..."
"$STAGE3" compile-manifest "$ROOT" "$MANIFEST" "$STAGE4_ASM" 3
if ! cmp -s "$STAGE3_ASM" "$STAGE4_ASM"; then
  echo "Native self-host FAILED: Stage-3 and Stage-4 assembly differ." >&2
  cmp -l "$STAGE3_ASM" "$STAGE4_ASM" | head -n 20 >&2 || true
  exit 1
fi

echo "[7/8] Native Stage-3 builds and executes a standalone Noqeri program..."
"$STAGE3" build "$ROOT/tests/native_selfhost_smoke.noqeri" "$SMOKE" 3
smoke_output=$("$SMOKE")
if [ "$smoke_output" != "42" ]; then
  echo "Native self-host smoke program returned '$smoke_output' instead of 42." >&2
  exit 1
fi

echo "[8/8] Recording native self-host proof..."
{
  echo "schema=1"
  echo "verified=true"
  echo "execution_model=native-aot"
  echo "native_codegen=true"
  echo "native_selfhost=true"
  echo "target=linux-x86_64-sysv"
  echo "seed_execution_model=nqri-vm"
  echo "seed_compiler=$SEED_COMPILER"
  echo "seed_compiler_sha256=$(sha256_file "$SEED_COMPILER")"
  echo "stage1_nqri=$STAGE1"
  echo "stage1_nqri_sha256=$(sha256_file "$STAGE1")"
  echo "stage2_assembly=$STAGE2_ASM"
  echo "stage2_assembly_sha256=$(sha256_file "$STAGE2_ASM")"
  echo "stage2_compiler=$STAGE2"
  echo "stage2_compiler_sha256=$(sha256_file "$STAGE2")"
  echo "stage3_assembly=$STAGE3_ASM"
  echo "stage3_assembly_sha256=$(sha256_file "$STAGE3_ASM")"
  echo "stage3_compiler=$STAGE3"
  echo "stage3_compiler_sha256=$(sha256_file "$STAGE3")"
  echo "stage4_assembly_sha256=$(sha256_file "$STAGE4_ASM")"
  echo "stage2_stage3_byte_equal=true"
  echo "stage3_stage4_byte_equal=true"
  echo "smoke_output=42"
  echo "release_needs_cpp_codegen=false"
  echo "host_role=file_io_and_system_linker_only"
} > "$PROOF"

echo "Native Noqeri self-host verified."
echo "Stage-3 compiler: $STAGE3"
echo "Proof: $PROOF"
