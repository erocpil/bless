#!/bin/bash -eu
set -eu
#
# build.sh -- OSS-Fuzz build script for BLESS
#
# Invoked by OSS-Fuzz's build infrastructure.  Compiles each fuzz
# target with libFuzzer + ASan instrumentation and copies the
# resulting binaries to $OUT.
#
# Environment (set by OSS-Fuzz):
#   $CC, $CXX, $CFLAGS, $CXXFLAGS, $LIB_FUZZING_ENGINE, $OUT, $WORK
#
# Dependencies:
#   DPDK headers -- NOT required (all targets use local stubs)

if [ -z "${FUZZ_TARGETS:-}" ]; then
	FUZZ_TARGETS="
		fuzz_yaml
		fuzz_dist
		fuzz_ip_range
		fuzz_http_payload
		fuzz_dns_encode
		fuzz_bless
		fuzz_ws_frame
	"
fi

# Common flags: ASan, UBSan, fuzzer linkage, debug symbols
SAN_FLAGS="-fsanitize=fuzzer-no-link,address,undefined -g -O1"
OBJ_DIR="${WORK}/bless-fuzz-obj"
ZIP_TOOL="${ZIP_TOOL:-zip}"
mkdir -p "${OUT}" "${OBJ_DIR}"

# Build fuzz_yaml against the repository-pinned libyaml sources.  Compiling
# them with the OSS-Fuzz toolchain instruments the parser itself and avoids a
# dependency on whichever libyaml version happens to be installed in the base
# image.  configure normally supplies these version definitions; keep them in
# sync when updating the pinned submodule.
LIBYAML_DIR="third_party/libyaml/upstream"
LIBYAML_CPPFLAGS=(
	"-I${LIBYAML_DIR}/include"
	"-I${LIBYAML_DIR}/src"
	"-DHAVE_CONFIG_H=0"
	"-DYAML_VERSION_MAJOR=0"
	"-DYAML_VERSION_MINOR=2"
	"-DYAML_VERSION_PATCH=5"
	'-DYAML_VERSION_STRING="0.2.5"'
)
LIBYAML_SOURCES=(
	"${LIBYAML_DIR}/src/api.c"
	"${LIBYAML_DIR}/src/reader.c"
	"${LIBYAML_DIR}/src/scanner.c"
	"${LIBYAML_DIR}/src/parser.c"
	"${LIBYAML_DIR}/src/loader.c"
	"${LIBYAML_DIR}/src/writer.c"
	"${LIBYAML_DIR}/src/emitter.c"
	"${LIBYAML_DIR}/src/dumper.c"
)

echo "=== BLESS OSS-Fuzz build ==="
echo "CC=$CC"
echo "CXX=$CXX"
echo "CFLAGS=$CFLAGS"
echo "OUT=$OUT"

# Compile all C sources with $CC, then perform the final engine link with
# $CXX.  OSS-Fuzz fuzzing engines may depend on the C++ runtime even when
# the target and production sources are pure C.
for target in $FUZZ_TARGETS; do
	echo "--- Building $target ---"

	case "$target" in
	fuzz_yaml)
		$CC $CFLAGS $SAN_FLAGS "${LIBYAML_CPPFLAGS[@]}" \
			-I include -c fuzz/fuzz_yaml.c \
			-o "${OBJ_DIR}/${target}.o"
		$CC $CFLAGS $SAN_FLAGS "${LIBYAML_CPPFLAGS[@]}" \
			-I include -c src/config_yaml.c \
			-o "${OBJ_DIR}/${target}_config_yaml.o"

		yaml_objects=()
		for source in "${LIBYAML_SOURCES[@]}"; do
			object="${OBJ_DIR}/${target}_libyaml_$(basename "${source%.c}").o"
			$CC $CFLAGS $SAN_FLAGS "${LIBYAML_CPPFLAGS[@]}" \
				-c "$source" -o "$object"
			yaml_objects+=("$object")
		done

		$CXX $CXXFLAGS $SAN_FLAGS \
			-o "${OUT}/${target}" \
			"${OBJ_DIR}/${target}.o" \
			"${OBJ_DIR}/${target}_config_yaml.o" \
			"${yaml_objects[@]}" \
			$LIB_FUZZING_ENGINE
		;;

	fuzz_dist)
		$CC $CFLAGS $SAN_FLAGS \
			-I src -I include -c fuzz/fuzz_dist.c \
			-o "${OBJ_DIR}/${target}.o"
		$CC $CFLAGS $SAN_FLAGS \
			-I src -I include -c src/dist.c \
			-o "${OBJ_DIR}/${target}_dist.o"
		$CXX $CXXFLAGS $SAN_FLAGS \
			-o "${OUT}/${target}" \
			"${OBJ_DIR}/${target}.o" \
			"${OBJ_DIR}/${target}_dist.o" \
			$LIB_FUZZING_ENGINE
		;;

	fuzz_ip_range)
		$CC $CFLAGS $SAN_FLAGS \
			-I include -c fuzz/fuzz_ip_range.c \
			-o "${OBJ_DIR}/${target}.o"
		$CC $CFLAGS $SAN_FLAGS \
			-I include -c src/parse.c \
			-o "${OBJ_DIR}/${target}_parse.o"
		$CXX $CXXFLAGS $SAN_FLAGS \
			-o "${OUT}/${target}" \
			"${OBJ_DIR}/${target}.o" \
			"${OBJ_DIR}/${target}_parse.o" \
			$LIB_FUZZING_ENGINE
		;;

	fuzz_http_payload)
		$CC $CFLAGS $SAN_FLAGS \
			-c fuzz/fuzz_http_payload.c \
			-o "${OBJ_DIR}/${target}.o"
		$CXX $CXXFLAGS $SAN_FLAGS \
			-o "${OUT}/${target}" \
			"${OBJ_DIR}/${target}.o" \
			$LIB_FUZZING_ENGINE
		;;

	fuzz_dns_encode)
		$CC $CFLAGS $SAN_FLAGS \
			-c fuzz/fuzz_dns_encode.c \
			-o "${OBJ_DIR}/${target}.o"
		$CXX $CXXFLAGS $SAN_FLAGS \
			-o "${OUT}/${target}" \
			"${OBJ_DIR}/${target}.o" \
			$LIB_FUZZING_ENGINE
		;;

	fuzz_bless)
		$CC $CFLAGS $SAN_FLAGS \
			-I src -I include -c fuzz/fuzz_bless.c \
			-o "${OBJ_DIR}/${target}.o"
		$CC $CFLAGS $SAN_FLAGS \
			-I src -I include -c src/dist.c \
			-o "${OBJ_DIR}/${target}_dist.o"
		$CXX $CXXFLAGS $SAN_FLAGS \
			-o "${OUT}/${target}" \
			"${OBJ_DIR}/${target}.o" \
			"${OBJ_DIR}/${target}_dist.o" \
			$LIB_FUZZING_ENGINE
		;;

	fuzz_ws_frame)
		$CC $CFLAGS $SAN_FLAGS \
			-I include -c fuzz/fuzz_ws_frame.c \
			-o "${OBJ_DIR}/${target}.o"
		$CC $CFLAGS $SAN_FLAGS \
			-I include -c src/ws_frame.c \
			-o "${OBJ_DIR}/${target}_ws_frame.o"
		$CXX $CXXFLAGS $SAN_FLAGS \
			-o "${OUT}/${target}" \
			"${OBJ_DIR}/${target}.o" \
			"${OBJ_DIR}/${target}_ws_frame.o" \
			$LIB_FUZZING_ENGINE
		;;
	esac

	echo "  -> ${OUT}/${target}"
done

# Build per-target seed corpus zips (OSS-Fuzz convention:
# <target>_seed_corpus.zip alongside the binary in $OUT).
# Generating from source seeds avoids drift between the raw
# corpus/ directory and a manually-maintained zip.
if [ -d fuzz/corpus/ws_frame ]; then
	"${ZIP_TOOL}" -j "${OUT}/fuzz_ws_frame_seed_corpus.zip" \
		fuzz/corpus/ws_frame/*
fi

echo "=== Done: $(ls ${OUT}/fuzz_*) ==="
