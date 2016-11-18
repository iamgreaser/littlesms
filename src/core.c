#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "littleemu.h"

// TODO: make cores more separable
struct EmuGlobal *lemu_core_global_new(const char *fname, const void *data, size_t len);
void lemu_core_global_free(struct EmuGlobal *G);
void lemu_core_state_init(struct EmuGlobal *G, void *state);
void lemu_core_run_frame(struct EmuGlobal *G, void *sms, bool no_draw);

uint64_t time_now(void)
{
	struct timeval ts;
	gettimeofday(&ts, NULL);

	uint64_t sec = ts.tv_sec;
	uint64_t usec = ts.tv_usec;
	sec *= 1000000ULL;
	usec += sec;
	return usec;
}

struct EmuGlobal *lemu_global_new(const char *fname, const void *data, size_t len)
{
	return lemu_core_global_new(fname, data, len);
}

void lemu_global_free(struct EmuGlobal *G)
{
	lemu_core_global_free(G);
}

void lemu_state_init(struct EmuGlobal *G, void *state)
{
	lemu_core_state_init(G, state);
}

void lemu_run_frame(struct EmuGlobal *G, void *sms, bool no_draw)
{
	lemu_core_run_frame(G, sms, no_draw);
}

void lemu_copy(struct EmuGlobal *G, void *dest_state, void *src_state)
{
	memcpy(dest_state, src_state, G->state_len);
}

