#!/bin/bash
# build_engine.sh [RHVOICE_SRC] [linux|windows]
# Builds Fasih's RHVoice engine: Fasih's engine patches and the unit-selection backend applied to RHVoice
# at the commit in engine/RHVOICE_BASE_COMMIT.
#   bash unit/build_engine.sh                 everything is fetched into build/ (RHVoice, its Boost and
#                                             Sonic submodules, Opus 1.5.2) and built for Linux
#   bash unit/build_engine.sh "" windows      the Windows DLLs for the add-on (zig 0.13 needed:
#                                             pip install ziglang==0.13.0)
#   linux   -> RHVOICE_SRC/b   (label maker, test client and library used by the training tools; the
#                               training scripts look for it in build/rhvoice/b unless RHV_BUILD is set)
#   windows -> RHVOICE_SRC/bw  (RHVoice.dll + libRHVoice_core.dll)
#   OPUS_SRC: an Opus checkout (default build/opus, fetched at v1.5.2)
set -e
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/.." && pwd)
TARGET=${2:-linux}
RHDIR=${1:-$REPO/build/rhvoice}
if [ ! -d "$RHDIR/src/core" ]; then
  echo "fetching RHVoice into $RHDIR"
  git clone -q https://github.com/RHVoice/RHVoice "$RHDIR"
  git -C "$RHDIR" checkout -q "$(cat "$REPO/engine/RHVOICE_BASE_COMMIT")"
fi
RH=$(cd "$RHDIR" && pwd)
if [ ! -e "$RH/external/libs/sonic/sonic.c" ] || [ ! -d "$RH/external/libs/boost/libs/config/include" ] || [ ! -e "$RH/cmake/thirdParty/sanitizers/cmake/FindSanitizers.cmake" ]; then
  echo "fetching RHVoice's Sonic and Boost submodules"
  subs=$(git -C "$RH" config -f .gitmodules --get-regexp 'submodule\..*\.path' | awk '{print $2}' | grep -E '^(external/libs/(sonic|boost/)|cmake/thirdParty/|src/third-party/cldr)')
  git -C "$RH" submodule update -q --init --depth 1 $subs
fi
OPUS_SRC=${OPUS_SRC:-$REPO/build/opus}
if [ ! -d "$OPUS_SRC/include" ]; then
  echo "fetching Opus 1.5.2 into $OPUS_SRC"
  git clone -q --depth 1 -b v1.5.2 https://github.com/xiph/opus "$OPUS_SRC"
fi
# Boost headers: the system's if installed, otherwise RHVoice's submodules
BOOST_FLAGS=""
if [ ! -f /usr/include/boost/version.hpp ]; then
  for d in "$RH"/external/libs/boost/libs/*/include; do BOOST_FLAGS="$BOOST_FLAGS -I$d"; done
fi
cd "$RH"
if [ ! -e src/core/us_core.cpp ]; then
  git apply "$REPO/engine/rhvoice-world-vocoder.patch"
  cp -r "$REPO/engine/world" src/core/world
  git apply "$REPO/engine/rhvoice-skip-bad-words.patch"
  cp "$HERE/src/us_core.hpp" "$HERE/src/us_engine_impl.hpp" src/include/core/
  cp "$HERE/src/us_vocoder.hpp" src/include/core/
  cp "$HERE/src/us_core.cpp" "$HERE/src/us_engine_impl.cpp" "$HERE/src/us_vocoder.cpp" src/core/
  mkdir -p src/core/ort && cp "$HERE"/src/ort/*.h src/core/ort/
  mkdir -p src/core/miniz && cp "$HERE"/src/miniz/miniz.[ch] src/core/miniz/
  sed -i 's|#include "us_core.hpp"|#include "core/us_core.hpp"|' src/core/us_core.cpp
  sed -i 's|#include "us_vocoder.hpp"|#include "core/us_vocoder.hpp"|' src/core/us_vocoder.cpp
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
s=s.replace('target_link_libraries(RHVoice_core "${libs2link}")','target_link_libraries(RHVoice_core "${libs2link}" ${fasih_extra_libs} ${CMAKE_DL_LIBS})')
open(p,'w').write(s)
E
fi
OPUS=$(cd "$OPUS_SRC" && pwd)
if [ "$TARGET" = linux ]; then
  (mkdir -p "$OPUS/bl" && cd "$OPUS/bl" && cmake .. -DCMAKE_BUILD_TYPE=Release -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null && make -j"$(nproc)" opus)
  mkdir -p b && cd b
  cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_SONIC=OFF -DWITH_DATA=OFF -DCMAKE_CXX_FLAGS="-std=gnu++17$BOOST_FLAGS" -DFASIH_OPUS_DIR="$OPUS" -DFASIH_OPUS_LIB="$OPUS/bl/libopus.a" >/dev/null
  make -j"$(nproc)" RHVoice_core RHVoice-make-hts-labels RHVoice-test
  # the unit tools: test client and Opus encoder
  mkdir -p "$HERE/bin"
  gcc -O2 -c -o "$HERE/bin/miniz.o" "$HERE/src/miniz/miniz.c"
  g++ -O2 -std=c++17 -DFASIH_US_OPUS -I"$HERE/src" -I"$OPUS/include" -o "$HERE/bin/fasih-us-cli" "$HERE/src/us_cli.cpp" "$HERE/src/us_core.cpp" "$HERE/src/us_vocoder.cpp" "$HERE/bin/miniz.o" "$OPUS/bl/libopus.a" -lm -ldl -lpthread
  g++ -O2 -std=c++17 -I"$OPUS/include" -o "$HERE/bin/fasih-us-encode" "$HERE/src/us_encode.cpp" "$OPUS/bl/libopus.a" -lm
  g++ -O2 -std=c++17 -I"$OPUS/include" -o "$HERE/bin/fasih-us-decode" "$HERE/src/us_decode.cpp" "$OPUS/bl/libopus.a" -lm
  g++ -O2 -std=c++17 -DFASIH_US_OPUS -I"$HERE/src" -I"$OPUS/include" -o "$HERE/bin/fasih-us-usage" "$HERE/src/us_usage.cpp" "$HERE/src/us_core.cpp" "$HERE/src/us_vocoder.cpp" "$HERE/bin/miniz.o" "$OPUS/bl/libopus.a" -lm -ldl -lpthread
  echo "engine built: $RH/b (the training scripts find it there)"
  exit 0
fi
Z="$REPO/tools/zig"
if [ -f /usr/include/boost/version.hpp ]; then   # zig does not search /usr/include
  mkdir -p "$RH/boostinc"; ln -sfn /usr/include/boost "$RH/boostinc/boost"; BOOST_FLAGS="-isystem $RH/boostinc"
fi
mkdir -p bw && cd bw
cat > tc.cmake <<E
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $Z/zcc)
set(CMAKE_CXX_COMPILER $Z/zc++)
set(CMAKE_AR $Z/zar)
set(CMAKE_RANLIB $Z/zranlib)
set(CMAKE_RC_COMPILER $Z/zrc)
set(CMAKE_CXX_FLAGS "-D_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES -D_LIBCPP_ENABLE_CXX20_REMOVED_FEATURES -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION -D_LIBCPP_ENABLE_CXX20_REMOVED_NEGATORS -D_LIBCPP_ENABLE_CXX17_REMOVED_BINDERS -D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR $BOOST_FLAGS")
E
echo 'set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--export-all-symbols")' >> tc.cmake
(mkdir -p "$OPUS/bwin" && cd "$OPUS/bwin" && cmake .. -DCMAKE_TOOLCHAIN_FILE="$RH/bw/tc.cmake" -DCMAKE_BUILD_TYPE=Release -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF >/dev/null && make -j"$(nproc)" opus)
cmake .. -DCMAKE_TOOLCHAIN_FILE=tc.cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_SONIC=OFF -DWITH_DATA=OFF -DFASIH_OPUS_DIR="$OPUS" -DFASIH_OPUS_LIB="$OPUS/bwin/libopus.a" >/dev/null
make -j"$(nproc)" RHVoice_core || true    # the first link may fail on an outdated export list; it is rebuilt below
# zig's linker ignores --export-all-symbols: export the core's symbols with a .def file
cd src/core
for f in $(find CMakeFiles/RHVoice_core.dir -name "*.obj"); do nm -g --defined-only "$f" 2>/dev/null; done \
  | awk '$2 ~ /^[TW]$/ {print $3} $2 ~ /^[DBR]$/ {print $3" DATA"}' | grep -v '^\.\|^__\|^\.refptr' | sort -u -k1,1 | sed '1i EXPORTS' > core.def   # data (vtables, statics) as DATA, not as functions
grep -q core.def CMakeFiles/RHVoice_core.dir/link.txt || sed -i "s|-Wl,--out-implib|$PWD/core.def -Wl,--out-implib|" CMakeFiles/RHVoice_core.dir/link.txt
rm -f libRHVoice_core.dll; cd ../..
make -j"$(nproc)" RHVoice
ls -la src/core/libRHVoice_core.dll src/lib/libRHVoice.dll
