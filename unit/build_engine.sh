#!/bin/bash
# build_engine.sh RHVOICE_SRC [linux|windows]
# Applies Fasih's engine patch and the unit-selection backend to an RHVoice checkout
# (commit in engine/RHVOICE_BASE_COMMIT) and builds it.
#   linux   -> RHVOICE_SRC/b   (label maker + test client used by the training tools)
#   windows -> RHVOICE_SRC/bw  (RHVoice.dll + libRHVoice_core.dll for the add-on; zig 0.13 needed:
#              pip install ziglang==0.13.0, and libboost-dev for the headers)
#   OPUS_SRC: an Opus checkout (https://github.com/xiph/opus, v1.5.2), built here for the target
set -e
RH=$(cd "$1" && pwd); TARGET=${2:-linux}
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/.." && pwd)
cd "$RH"
if [ ! -e src/core/us_core.cpp ]; then
  git apply "$REPO/engine/rhvoice-world-vocoder.patch"
  cp -r "$REPO/engine/world" src/core/world
  cp "$HERE/src/us_core.hpp" "$HERE/src/us_engine_impl.hpp" src/include/core/
  cp "$HERE/src/us_core.cpp" "$HERE/src/us_engine_impl.cpp" src/core/
  sed -i 's|#include "us_core.hpp"|#include "core/us_core.hpp"|' src/core/us_core.cpp
  python3 - <<'E'
p='src/include/core/hts_engine_pool.hpp'; s=open(p).read()
s=s.replace('#include "str_hts_engine_impl.hpp"','#include "str_hts_engine_impl.hpp"\n#include "us_engine_impl.hpp"')
s=s.replace('      prototypes.push_back(hts_engine_impl::pointer(new str_hts_engine_impl(info_)));',
 '      if(is_unit_voice(info_))\n        prototypes.push_back(hts_engine_impl::pointer(new us_engine_impl(info_)));\n      else\n        prototypes.push_back(hts_engine_impl::pointer(new str_hts_engine_impl(info_)));')
open(p,'w').write(s)
p='src/core/CMakeLists.txt'; s=open(p).read()
s=s.replace('add_library(RHVoice_core SHARED "${SRCFILES}")',
 'set(SONIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/libs/sonic")\nadd_library(RHVoice_core SHARED "${SRCFILES}" "${SONIC_DIR}/sonic.c")\ntarget_compile_definitions(RHVoice_core PRIVATE ENABLE_SONIC=1)')
s=s.replace('"${CMAKE_CURRENT_SOURCE_DIR}/world")','"${CMAKE_CURRENT_SOURCE_DIR}/world" "${SONIC_DIR}")')
s=s.replace('target_compile_definitions(RHVoice_core PRIVATE ENABLE_SONIC=1)','target_compile_definitions(RHVoice_core PRIVATE ENABLE_SONIC=1)\nif(FASIH_OPUS_DIR)\n  target_compile_definitions(RHVoice_core PRIVATE FASIH_US_OPUS=1)\n  target_include_directories(RHVoice_core PRIVATE "${FASIH_OPUS_DIR}/include")\n  list(APPEND fasih_extra_libs "${FASIH_OPUS_LIB}")\nendif()')
s=s.replace('target_link_libraries(RHVoice_core "${libs2link}")','target_link_libraries(RHVoice_core "${libs2link}" ${fasih_extra_libs})')
open(p,'w').write(s)
E
fi
OPUS=$(cd "${OPUS_SRC:?set OPUS_SRC to an opus checkout}" && pwd)
if [ "$TARGET" = linux ]; then
  (mkdir -p "$OPUS/bl" && cd "$OPUS/bl" && cmake .. -DCMAKE_BUILD_TYPE=Release -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null && make -j"$(nproc)" opus)
  mkdir -p b && cd b
  cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_SONIC=OFF -DWITH_DATA=OFF -DFASIH_OPUS_DIR="$OPUS" -DFASIH_OPUS_LIB="$OPUS/bl/libopus.a" >/dev/null
  make -j"$(nproc)" RHVoice_core RHVoice-make-hts-labels RHVoice-test
  # the unit tools: test client and Opus encoder
  mkdir -p "$HERE/bin"
  g++ -O2 -std=c++17 -DFASIH_US_OPUS -I"$OPUS/include" -o "$HERE/bin/fasih-us-cli" "$HERE/src/us_cli.cpp" "$HERE/src/us_core.cpp" "$OPUS/bl/libopus.a" -lm
  g++ -O2 -std=c++17 -I"$OPUS/include" -o "$HERE/bin/fasih-us-encode" "$HERE/src/us_encode.cpp" "$OPUS/bl/libopus.a" -lm
  exit 0
fi
Z="$REPO/tools/zig"; mkdir -p "$RH/boostinc"; ln -sfn /usr/include/boost "$RH/boostinc/boost"
mkdir -p bw && cd bw
cat > tc.cmake <<E
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $Z/zcc)
set(CMAKE_CXX_COMPILER $Z/zc++)
set(CMAKE_AR $Z/zar)
set(CMAKE_RANLIB $Z/zranlib)
set(CMAKE_RC_COMPILER $Z/zrc)
set(CMAKE_CXX_FLAGS "-D_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES -D_LIBCPP_ENABLE_CXX20_REMOVED_FEATURES -isystem $RH/boostinc")
E
echo 'set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--export-all-symbols")' >> tc.cmake
(mkdir -p "$OPUS/bwin" && cd "$OPUS/bwin" && cmake .. -DCMAKE_TOOLCHAIN_FILE="$RH/bw/tc.cmake" -DCMAKE_BUILD_TYPE=Release -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF >/dev/null && make -j"$(nproc)" opus)
cmake .. -DCMAKE_TOOLCHAIN_FILE=tc.cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_SONIC=OFF -DWITH_DATA=OFF -DFASIH_OPUS_DIR="$OPUS" -DFASIH_OPUS_LIB="$OPUS/bwin/libopus.a" >/dev/null
make -j"$(nproc)" RHVoice_core || true    # the first link may fail on an outdated export list; it is rebuilt below
# zig's linker ignores --export-all-symbols: export the core's symbols with a .def file
cd src/core
for f in $(find CMakeFiles/RHVoice_core.dir -name "*.obj"); do nm -g --defined-only "$f" 2>/dev/null; done \
  | awk '$2 ~ /^[TWDBR]$/ {print $3}' | grep -v '^\.\|^__\|^\.refptr' | sort -u | sed '1i EXPORTS' > core.def
grep -q core.def CMakeFiles/RHVoice_core.dir/link.txt || sed -i "s|-Wl,--out-implib|$PWD/core.def -Wl,--out-implib|" CMakeFiles/RHVoice_core.dir/link.txt
rm -f libRHVoice_core.dll; cd ../..
make -j"$(nproc)" RHVoice
ls -la src/core/libRHVoice_core.dll src/lib/libRHVoice.dll
