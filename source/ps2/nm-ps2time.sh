#!/bin/sh
export PS2DEV=/usr/local/ps2dev
export PATH="$PS2DEV/ee/bin:$PS2DEV/bin:$PATH"
ee-nm "$PS2DEV/ps2sdk/ee/lib/libps2time.a"
