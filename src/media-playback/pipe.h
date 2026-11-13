/*
 * Stable Stream Source: decoupled video decoding for network streams.
 * Copyright (C) 2026 Glowfish GmbH & Co. KG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <util/deque.h>
#include <util/threading.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4204)
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

struct mp_media;

/* Presentation runs this far behind the arrival of the data, so the video
 * thread can decode ahead and short GPU stalls are absorbed. */
#define MP_PIPE_DELAY_NS 100000000ULL

/* extra hardware surfaces, frames wait between decode and copy thread */
#define MP_PIPE_EXTRA_HW_FRAMES 6

struct mp_pipe_frame {
	AVFrame *frame;
	int64_t pts;
	int64_t next_pts;
};

struct mp_pipe_stats {
	uint64_t decode_errors;
	uint64_t copy_errors;
	uint64_t dropped_packets;
	uint64_t skips;
};

struct mp_pipe {
	struct mp_media *m;
	bool active;
	volatile bool stop;
	os_event_t *wake; /* wakes the media thread */

	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool mutex_valid;
	bool cond_valid;

	pthread_t read_thread;
	pthread_t decode_thread;
	pthread_t copy_thread;
	bool read_valid;
	bool decode_valid;
	bool copy_valid;

	/* all fields below are protected by mutex */
	bool read_done; /* reader ended, read_ret holds the reason */
	int read_ret;
	bool decode_done; /* decoder drained after read_done */
	bool copy_done;   /* all frames handed to the output queue */

	struct deque audio_packets; /* AVPacket *, reader -> media thread */
	struct deque video_packets; /* AVPacket *, reader -> decode thread */
	struct deque hw_frames;     /* struct mp_pipe_frame, decode -> copy */
	struct deque out_frames;    /* struct mp_pipe_frame, copy -> media thread */
	size_t hw_count;
	size_t out_count;
	int64_t clock_pts; /* media thread clock (next_pts_ns), 0 = not running */

	/* decode thread only */
	int64_t frame_pts;
	int64_t next_pts;
	int64_t last_duration;
	bool skipping;
	uint64_t skip_start_ns;
	uint64_t skip_packets;

	/* statistics, protected by mutex */
	uint64_t decode_errors;
	uint64_t copy_errors;
	uint64_t dropped_packets;
	uint64_t skips;
};

extern bool mp_pipe_start(struct mp_pipe *p, struct mp_media *m);
extern void mp_pipe_free(struct mp_pipe *p);
extern bool mp_pipe_stopping(struct mp_pipe *p);
extern void mp_pipe_get_stats(struct mp_pipe *p, struct mp_pipe_stats *s);

/* media thread side */
extern size_t mp_pipe_take_audio(struct mp_pipe *p, struct deque *dst);
extern bool mp_pipe_pop_video(struct mp_pipe *p, struct mp_pipe_frame *out);
extern void mp_pipe_set_clock(struct mp_pipe *p, int64_t clock_pts);
extern bool mp_pipe_peek_video_pts(struct mp_pipe *p, int64_t *pts);
extern size_t mp_pipe_video_buffered(struct mp_pipe *p);
extern void mp_pipe_wait(struct mp_pipe *p, uint32_t ms);
extern bool mp_pipe_read_done(struct mp_pipe *p, int *ret);
extern bool mp_pipe_video_done(struct mp_pipe *p);

#ifdef __cplusplus
}
#endif
