#include "system/sms/all.h"

#define VDP_ADD_CYCLES(vdp, v) (vdp)->H.timestamp += ((v)*2)

#include "video/tms9918/core.c"

