#!/bin/sh
set -e
export PS2DEV=/usr/local/ps2dev
export PS2SDK="$PS2DEV/ps2sdk"
export PATH="$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PS2DEV/bin:$PS2SDK/bin:/c/Program Files/Git/cmd:/bin:/usr/bin:$PATH"

echo "ee-gcc: $(which ee-gcc)"
ee-gcc --version | head -1
echo "PS2SDK=$PS2SDK"

# Makefile uses git describe; MSYS has no git. Fall back if needed.
if command -v git >/dev/null 2>&1; then
  :
else
  export EE_CFLAGS="${EE_CFLAGS} -DGIT_VERSION=local"
fi

cd "$(dirname "$0")"
if [ "$1" = "pcsx2" ]; then
  make -j1 pcsx2
  ls -l TempGBA-pcsx2.elf
elif [ "$1" = "pcsx2-relink" ]; then
  make -j1 PCSX2=1
  ls -l TempGBA-pcsx2.elf
elif [ "$1" = "hardware" ]; then
  make clean
  make -j1
  ls -l TempGBA.elf
else
  make -j1
  ls -l TempGBA.elf
fi
