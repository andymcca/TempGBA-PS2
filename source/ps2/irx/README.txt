Vendored IOP modules, not in the 2018 PS2SDK:

- sio2man.irx — ps2sdk sio2man library 2.7 (ps2dev/ps2dev image, 2026-09-22).
  mmceman only locks the controller bus on sio2man 1.2 or 2.7. rom0:SIO2MAN
  is older, so a directory listing steals the pad. Loaded at boot in place
  of rom0:SIO2MAN. ROM MCMAN and PADMAN stay.
- mmceman.irx — https://github.com/ps2-mmce/mmceman/releases/tag/latest
- mx4sio_bd.irx — https://github.com/saildot4k/wLaunchELF_R3Z (iop/__precompiled)

MMCE and MX4SIO both hook SIO2 and cannot be used at the same time.
