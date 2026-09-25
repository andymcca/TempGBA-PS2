# PSP-mod → PS2 port log

Working tree: `tempgba-ps2/` (TempGBA-PS2).  
Source of ports: TempGBA4PSP-mod `master` (`3153984`) and the earlier commits that were later re-baselined on PSP.

Do **not** cherry-pick. Shared filenames hide a layout split (`source/src/` + Allegrex vs `source/` + EE `stub.S`).

---

## Already on PS2 (before this pass)

| Item | Notes |
| --- | --- |
| HOST / PCSX2 as compile flag | `make PCSX2=1` → `-DHOST`; hardware ELF stays separate |
| Field-clock audio | Restore of `3769386`; do not go back to one-GBA-frame-per-emu-frame on PAL |
| Load-new-game hang | `CHANGED_PC_STATUS` must stay set after `load_gamepak` (`311cfca`) |
| Sticky ROM cache | PS2 already has LRU + prefill + prefetch |
| 4+2 MiB JIT | Already in `port.h` |

---

## Isolation (2026-09-22)

Hang after “Loading per-game settings” (leftover progress; first `execute` never returns). User confirmed working again after the **last** revert batch.

| Revert | Result |
| --- | --- |
| BIOS SWI pin | still hung |
| Div/DivArm HLE, Thumb PC-pool, IWRAM STM, OAM/affine | still hung |
| **ADCS/SBCS/RSCS emit + sound I/O masks** | **works again** |

Hang was therefore **ADCS/SBCS/RSCS** (`emit.h`, PSP `ee9e12c` / libretro `88454e9`) and/or the sound write masks (`sound.h`, `63d2782`). ADCS is the execute-path one; do not re-apply that sequence as-is on EE. Sound masks are unused-bit stores and are the weaker suspect.

All six PSP-alignment ports are currently **reverted**. Working tree matches `_TempGBA-PS2-git` source (pre-port).

---

## Ports (applied then reverted this pass)

Leave `expand_blend_mips` until last (see below). Re-apply one at a time after a working baseline; skip stock ADCS.

### 1. Sound I/O write masks — reverted (weaker hang suspect)

PSP: `63d2782`. PS2 still uses `GBC_SOUND_*` macros, not `write_io_register16`.

- tone high: `0x47FF`
- sweep: `value &= 0x007F`
- wave: `0x00E0`
- noise: `0x40FF`
- SOUNDCNT_L: `0xFF77`
- SOUNDCNT_H: `0x770F`

### 2. ADCS / SBCS / RSCS flag codegen — reverted (**hang**)

PSP: `ee9e12c` (libretro `88454e9`). EE has `movz`, but the ported flag sequence stalled first execute. Do not drop in again without an EE-specific rewrite / single-op test.

### 3. IWRAM stack STM tag invalidation — reverted (not the hang)

PSP: `bd15015`. Safe to retry alone once baseline is confirmed.

### 4. Old-renderer OAM + affine — reverted (not the hang)

PSP: `7ae8f25`, `53d92ea`. Safe to retry alone.

### 5. Thumb ROM PC-pool loads — reverted (not the hang)

PSP: `0d058b3`. Safe to retry alone.

### 6. BIOS SWI pin + Div/DivArm HLE — reverted (not the hang)

PSP: `737ef19`, `5aadbd9`. Pin was already ruled out; Div HLE was in the “still hung” batch. Retry Div only with a zero-divisor guard; skip the pin.

---

## Done this batch

### `expand_blend_mips` (old renderer alpha)

PSP `source/src/video_blend_mips.S` (`4d1aa19`) Allegrex loop ported to EE as `source/video_blend_mips.S`:

- `ins` dilation → `sll`/`or`/`and` (`(c|(c<<16)) & 0x03E07C1F`)
- Allegrex 2-op `maddu` → two `multu` + `addu` (LO-only; weights fit)
- `video.c`: `expand_blend` → asm; `expand_blend_c` kept; `#define USE_EXPAND_BLEND_MIPS 0` forces C
- `ps2/Makefile`: `../video_blend_mips.o` in `COMMON_OBJS`

Validate on hardware/PCSX2 with alpha-blended OBJ titles (saturate and non-saturate BLDALPHA).

---

## Still to do

### Later extras (not in this batch)

| Item | Why later |
| --- | --- |
| ZIP / `file_length` (`09e6563`) | Portable `zip.c` only; ignore PSP gui/exception |
| IRQ delay accounting (`01c3798`) | Re-apply in PS2 vcount loop, not PSP `main.c` |
| `game_config` extras | SMC/M4A gates, idle-loop cap, `filename_match`; PS2 file is still Kai-era |
| Newer-SDK `audsrv` available/queued | Tried: vendored current IRX + EE wrappers + hardware worker. **Crashed as soon as audio started.** Reverted. Leave alone unless we have a new plan that does not swap the IRX. |
| Mirrored ROM load stubs `0x08–0x0B` | Large `stub.S` rewrite; only if paging still hurts |

### Skip (PSP-only)

Dual renderer / OAM hijack, sleep/resume, GBK/CJK, XMB/EBOOT, boxart, carousel, Recent ROMs, single-game launcher, overclock.

---

## Build / test notes

- Hardware ELF and PCSX2 HOST ELF stay separate. `make clean` between `make` and `make pcsx2`.
- Build via MSYS 2018 toolchain (`source/ps2/build-msys.sh`), not host `psp-gcc` / Docker.
- HOST ELF: `source/ps2/TempGBA-pcsx2.elf` (PSP ports reverted; confirmed working).
- Hardware ELF: `source/ps2/TempGBA.elf`. Main-thread play; honor `play_audio` return (v5). `make WATCHDOG=1` → `TempGBA57.elf` (v5+v7). Option branches: `audio-v2-field-clock`, `audio-v3-backpressure`, `audio-v7-watchdog`. HOST never starts a worker.
- **Hang after “Loading per-game settings”:** leftover progress; first `execute` never returns.
- **All PSP-alignment ports reverted (2026-09-22):** sound masks, ADCS/SBCS/RSCS, IWRAM STM, OAM/affine, Thumb PC-pool, SWI pin, Div HLE. Working tree now matches `_TempGBA-PS2-git` source (pre-port).
- Smoke-test: load-game, audio, a BIOS-heavy title, and a blended-sprite title.
- Do not commit until asked.
