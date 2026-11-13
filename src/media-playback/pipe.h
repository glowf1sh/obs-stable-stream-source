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

/* video later than this is dropped, not shown */
#define MP_PIPE_STALE_NS 500000000LL

/* extra hardware surfaces, frames wait between decode and copy thread */
#define MP_PIPE_EXTRA_HW_FRAMES 6

struct mp_pipe_frame {
	AVFrame *frame;
	int64_t pts;
	int64_t next_pts;
};

/* one video decoder with its timestamp state */
struct mp_pipe_vdec {
	AVCodecContext *c;
	bool sw;
	int64_t frame_pts;
	int64_t next_pts;
	int64_t last_duration;
	bool skipping;
	uint64_t skip_start_ns;
	uint64_t skip_packets;
};

/* software fallback, see pipe.c */
enum mp_pipe_fallback {
	MP_PIPE_FB_NONE,     /* hardware decoding, no trouble */
	MP_PIPE_FB_WAIT_KEY, /* trouble: software decoder starts at the next keyframe */
	MP_PIPE_FB_RUNNING,  /* software decoder runs in parallel */
	MP_PIPE_FB_SWITCHED, /* software decoder delivers, hardware idle */
	MP_PIPE_FB_GAVE_UP,  /* software decoder could not catch up */
};

struct mp_pipe_stats {
	uint64_t decode_errors;
	uint64_t copy_errors;
	uint64_t dropped_packets;
	uint64_t skips;
	bool software;
	int64_t video_after_audio; /* ns, latest video arrival relative to the audio */
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
	pthread_t sw_thread;
	bool read_valid;
	bool decode_valid;
	bool copy_valid;
	bool sw_valid;

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

	/* last frame handed to the output queue */
	bool out_set;
	int64_t out_pts;
	int64_t out_next_pts;

	/* software fallback */
	enum mp_pipe_fallback fb;
	bool hw_unused; /* hardware device open, but FFmpeg decodes in software */

	/* arrival time minus timestamp, per stats interval */
	int64_t skew_a_min;
	int64_t skew_v_max;
	bool skew_a_set;
	bool skew_v_set;
	uint64_t fb_start_ns;
	uint64_t trouble_since;
	int trouble;
	struct deque sw_packets;  /* AVPacket *, reader -> sw thread */
	struct deque gop_packets; /* AVPacket *, since the last keyframe */
	bool gop_valid;
	struct deque sw_frames; /* struct mp_pipe_frame, decoded ahead */
	size_t sw_count;

	/* owned by the decode thread (hardware) and the sw thread */
	struct mp_pipe_vdec hwd;
	struct mp_pipe_vdec swd;

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
extern void mp_pipe_note_empty(struct mp_pipe *p);
extern void mp_pipe_set_clock(struct mp_pipe *p, int64_t clock_pts);
extern bool mp_pipe_peek_video_pts(struct mp_pipe *p, int64_t *pts);
extern size_t mp_pipe_video_buffered(struct mp_pipe *p);
extern void mp_pipe_wait(struct mp_pipe *p, uint32_t ms);
extern bool mp_pipe_read_done(struct mp_pipe *p, int *ret);
extern bool mp_pipe_video_done(struct mp_pipe *p);

#ifdef __cplusplus
}
#endif
