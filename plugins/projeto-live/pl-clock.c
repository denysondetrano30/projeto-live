/*
 * Projeto Live: relógio em comum entre o OBS e um celular (veja pl-clock.h).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pl-clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#define SAMPLES 16
/* Resposta que demorou mais que isso não serve para medir (a rede estava engasgada). */
#define MAX_ROUND_TRIP_NS (500LL * 1000000LL)

struct sample {
	int64_t offset;
	int64_t round_trip;
};

struct pl_clock {
	pthread_mutex_t mutex;
	struct sample samples[SAMPLES];
	int count;
	int next;
};

struct pl_clock *pl_clock_create(void)
{
	struct pl_clock *clock = bzalloc(sizeof(*clock));
	pthread_mutex_init(&clock->mutex, NULL);
	return clock;
}

void pl_clock_destroy(struct pl_clock *clock)
{
	if (!clock) {
		return;
	}
	pthread_mutex_destroy(&clock->mutex);
	bfree(clock);
}

void pl_clock_reset(struct pl_clock *clock)
{
	if (!clock) {
		return;
	}
	pthread_mutex_lock(&clock->mutex);
	clock->count = 0;
	clock->next = 0;
	pthread_mutex_unlock(&clock->mutex);
}

void pl_clock_request(char *line, size_t size)
{
	snprintf(line, size, "TIME %llu", (unsigned long long)os_gettime_ns());
}

bool pl_clock_handle_line(struct pl_clock *clock, const char *line)
{
	if (strncmp(line, "TIME ", 5) != 0) {
		return false;
	}
	int64_t received = (int64_t)os_gettime_ns();
	char *end = NULL;
	int64_t sent = (int64_t)strtoull(line + 5, &end, 10);
	if (!end || *end != ' ') {
		return true;
	}
	int64_t phone = strtoll(end + 1, NULL, 10);
	int64_t round_trip = received - sent;
	if (!clock || sent <= 0 || phone <= 0 || round_trip < 0 || round_trip > MAX_ROUND_TRIP_NS) {
		return true;
	}

	/* O celular leu o relógio dele mais ou menos no meio da ida e volta. */
	struct sample sample = {phone - (sent + round_trip / 2), round_trip};
	pthread_mutex_lock(&clock->mutex);
	clock->samples[clock->next] = sample;
	clock->next = (clock->next + 1) % SAMPLES;
	if (clock->count < SAMPLES) {
		clock->count++;
	}
	pthread_mutex_unlock(&clock->mutex);
	return true;
}

bool pl_clock_offset(struct pl_clock *clock, int64_t *offset_ns)
{
	if (!clock) {
		return false;
	}
	pthread_mutex_lock(&clock->mutex);
	int best = -1;
	for (int i = 0; i < clock->count; i++) {
		if (best < 0 || clock->samples[i].round_trip < clock->samples[best].round_trip) {
			best = i;
		}
	}
	if (best >= 0) {
		*offset_ns = clock->samples[best].offset;
	}
	pthread_mutex_unlock(&clock->mutex);
	return best >= 0;
}
