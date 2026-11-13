/*
 * Stable Stream Source: decoupled video decoding for network streams.
 * Copyright (C) 2026 Glowfish GmbH & Co. KG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * With hardware decoding, every video frame is copied back from the GPU
 * (av_hwframe_transfer_data), which waits for the GPU synchronously. In the
 * OBS media playback this happens in the same thread that decodes and paces
 * the audio, so a slow GPU delays the sound as well.
 *
 * For network streams with hardware decoding the work is split up:
 *   read thread:   av_read_frame, audio packets to the media thread, video
 *                  packets to the decode thread
 *   decode thread: decodes video packets (hardware frames)
 *   copy thread:   copies hardware frames back to system memory
 *   media thread:  decodes audio and hands audio and video to OBS on one
 *                  clock, as before
 *
 * The media thread runs MP_PIPE_DELAY_NS behind the arrival of the data, so
 * up to MP_PIPE_OUT_MAX decoded frames wait in front of it and short GPU
 * stalls do not reach the picture. Decoding and copying overlap. The audio
 * never waits for the video.
 *
 * Software fallback: if the buffer keeps running empty while undecoded data
 * is waiting (the GPU is too slow), a software decoder starts in parallel
 * (sw thread) at the last keyframe already received (the reader keeps the
 * packets since then), otherwise at the next one. It decodes faster than
 * real time, and as soon as it can deliver the frame after the last one from
 * the GPU, the output switches over, without a gap. If it cannot catch up within MP_PIPE_FB_GIVE_UP_NS, hardware
 * decoding stays.
 */

#include "media-playback.h"
#include "media.h"
#include "pipe.h"

#include <inttypes.h>
#include <util/platform.h>

#define MP_PIPE_HW_MAX 3
#define MP_PIPE_OUT_MAX 8
#define MP_PIPE_SW_MAX 8

/* Upper bound for data waiting in front of the media thread (it does not
 * consume while paused or stalled). Old packets are dropped beyond it. */
#define MP_PIPE_AUDIO_PACKETS_MAX 1000
#define MP_PIPE_VIDEO_PACKETS_MAX 2000

/* If decoding falls this far behind the clock, the decoder skips ahead to
 * the next keyframe that is not late. Otherwise the picture would drift
 * further and further behind the sound. */
#define MP_PIPE_SKIP_NS 200000000LL

/* larger differences are timestamp jumps of the stream, not lateness */
#define MP_PIPE_JUMP_NS 2000000000LL

/* software fallback: MP_PIPE_FB_TROUBLE trouble points within
 * MP_PIPE_FB_WINDOW_NS start it (buffer ran empty with data waiting: 1,
 * skipped to a keyframe: MP_PIPE_FB_TROUBLE) */
#define MP_PIPE_FB_TROUBLE 3
#define MP_PIPE_FB_WINDOW_NS 30000000000ULL
#define MP_PIPE_FB_GIVE_UP_NS 20000000000ULL

/* if the picture stands still meanwhile, switch even with a gap after */
#define MP_PIPE_FB_STANDING_NS 500000000ULL

extern AVPacket *mp_media_get_packet(struct mp_media *m);
extern void mp_media_free_packet(struct mp_media *m, AVPacket *pkt);

static inline void wake_media(struct mp_pipe *p)
{
	os_event_signal(p->wake);
}

static inline bool pipe_stopped(struct mp_pipe *p)
{
	return os_atomic_load_bool(&p->stop);
}

static inline size_t packet_count(struct deque *q)
{
	return q->size / sizeof(AVPacket *);
}

static void drop_front_packet(struct mp_pipe *p, struct deque *q)
{
	AVPacket *pkt;
	deque_pop_front(q, &pkt, sizeof(pkt));
	mp_media_free_packet(p->m, pkt);
	p->dropped_packets++;
}

static void clear_packets(struct mp_pipe *p, struct deque *q)
{
	while (q->size) {
		AVPacket *pkt;
		deque_pop_front(q, &pkt, sizeof(pkt));
		mp_media_free_packet(p->m, pkt);
	}
}

static void clear_frames(struct deque *q)
{
	while (q->size) {
		struct mp_pipe_frame item;
		deque_pop_front(q, &item, sizeof(item));
		av_frame_free(&item.frame);
	}
}

/* mutex held */
static void push_limited(struct mp_pipe *p, struct deque *q, AVPacket *pkt)
{
	while (packet_count(q) >= MP_PIPE_VIDEO_PACKETS_MAX)
		drop_front_packet(p, q);
	deque_push_back(q, &pkt, sizeof(pkt));
}

/* mutex held */
static void fallback_give_up(struct mp_pipe *p, const char *why)
{
	p->fb = MP_PIPE_FB_GAVE_UP;
	clear_packets(p, &p->sw_packets);
	clear_frames(&p->sw_frames);
	p->sw_count = 0;
	pthread_cond_broadcast(&p->cond);
	blog(LOG_INFO, "[stable-stream-source] software decoding %s, staying with hardware decoding", why);
}

/* video packet from the reader, mutex held */
static void route_video_packet(struct mp_pipe *p, AVPacket *pkt)
{
	/* packets since the last keyframe, for a quick start of the fallback */
	if (p->sw_valid && p->fb == MP_PIPE_FB_NONE) {
		if (pkt->flags & AV_PKT_FLAG_KEY) {
			clear_packets(p, &p->gop_packets);
			p->gop_valid = true;
		}
		if (p->gop_valid && packet_count(&p->gop_packets) >= MP_PIPE_VIDEO_PACKETS_MAX) {
			clear_packets(p, &p->gop_packets);
			p->gop_valid = false;
		}
		if (p->gop_valid) {
			AVPacket *copy = av_packet_clone(pkt);
			if (copy)
				deque_push_back(&p->gop_packets, &copy, sizeof(copy));
		}
	}

	if (p->fb == MP_PIPE_FB_WAIT_KEY && (pkt->flags & AV_PKT_FLAG_KEY)) {
		p->fb = MP_PIPE_FB_RUNNING;
		p->fb_start_ns = os_gettime_ns();
		blog(LOG_INFO, "[stable-stream-source] software decoding starts in parallel at a keyframe");
	}

	if (p->fb == MP_PIPE_FB_SWITCHED) {
		push_limited(p, &p->sw_packets, pkt);
		return;
	}

	if (p->fb == MP_PIPE_FB_RUNNING) {
		if (packet_count(&p->sw_packets) >= MP_PIPE_VIDEO_PACKETS_MAX) {
			fallback_give_up(p, "falls too far behind");
		} else {
			AVPacket *copy = av_packet_clone(pkt);
			if (copy)
				deque_push_back(&p->sw_packets, &copy, sizeof(copy));
		}
	}

	push_limited(p, &p->video_packets, pkt);
}

static void *read_thread(void *data)
{
	struct mp_pipe *p = data;
	struct mp_media *m = p->m;
	int audio_index = m->has_audio ? m->a.stream->index : -1;
	int video_index = m->v.stream->index;

	os_set_thread_name("ssrc_read");

	for (;;) {
		AVPacket *pkt = mp_media_get_packet(m);
		int ret = av_read_frame(m->fmt, pkt);
		if (ret < 0) {
			mp_media_free_packet(m, pkt);
			if (ret != AVERROR_EOF && ret != AVERROR_EXIT && !pipe_stopped(p))
				blog(LOG_WARNING, "[stable-stream-source] av_read_frame failed: %s (%d)",
				     av_err2str(ret), ret);

			pthread_mutex_lock(&p->mutex);
			p->read_ret = ret;
			p->read_done = true;
			pthread_cond_broadcast(&p->cond);
			pthread_mutex_unlock(&p->mutex);
			wake_media(p);
			break;
		}

		/* arrival time, to tell a stalled decoder from a timestamp jump */
		pkt->opaque = (void *)(uintptr_t)os_gettime_ns();

		bool audio = pkt->size && pkt->stream_index == audio_index;
		bool video = pkt->size && pkt->stream_index == video_index;
		if (!audio && !video) {
			mp_media_free_packet(m, pkt);
			continue;
		}

		pthread_mutex_lock(&p->mutex);
		if (pipe_stopped(p)) {
			pthread_mutex_unlock(&p->mutex);
			mp_media_free_packet(m, pkt);
			break;
		}
		if (audio) {
			while (packet_count(&p->audio_packets) >= MP_PIPE_AUDIO_PACKETS_MAX)
				drop_front_packet(p, &p->audio_packets);
			deque_push_back(&p->audio_packets, &pkt, sizeof(pkt));
		} else {
			route_video_packet(p, pkt);
		}
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);

		if (audio)
			wake_media(p);
	}

	return NULL;
}

/* same timestamp rules as mp_decode_next() for video */
static void set_frame_pts(struct mp_pipe *p, struct mp_pipe_vdec *dec, struct mp_pipe_frame *item)
{
	AVStream *stream = p->m->v.stream;
	AVFrame *f = item->frame;
	int64_t last_pts = dec->frame_pts;

	if (f->best_effort_timestamp == AV_NOPTS_VALUE)
		dec->frame_pts = dec->next_pts;
	else
		dec->frame_pts = av_rescale_q(f->best_effort_timestamp, stream->time_base, (AVRational){1, 1000000000});

	int64_t duration = f->duration;
	if (duration) {
		duration = av_rescale_q(duration, stream->time_base, (AVRational){1, 1000000000});
	} else if (last_pts) {
		duration = dec->frame_pts - last_pts;
	} else if (dec->last_duration) {
		duration = dec->last_duration;
	} else {
		duration = av_rescale_q(dec->c->time_base.num, dec->c->time_base, (AVRational){1, 1000000000});
	}

	dec->last_duration = duration;
	dec->next_pts = dec->frame_pts + duration;
	item->pts = dec->frame_pts;
	item->next_pts = dec->next_pts;
}

/* mutex held */
static void push_out(struct mp_pipe *p, struct mp_pipe_frame *item)
{
	deque_push_back(&p->out_frames, item, sizeof(*item));
	p->out_count++;
	p->out_set = true;
	p->out_pts = item->pts;
	p->out_next_pts = item->next_pts;
}

/* Switch to the software decoder once it has the frame that follows the
 * last one from the GPU. If the picture already stands still (the clock is
 * past the last frame), switch to whatever it has. Mutex held. */
static void try_switch(struct mp_pipe *p)
{
	if (p->fb != MP_PIPE_FB_RUNNING)
		return;

	while (p->sw_count) {
		struct mp_pipe_frame *head = deque_data(&p->sw_frames, 0);
		if (!p->out_set || head->pts > p->out_pts)
			break;
		struct mp_pipe_frame item;
		deque_pop_front(&p->sw_frames, &item, sizeof(item));
		av_frame_free(&item.frame);
		p->sw_count--;
		pthread_cond_broadcast(&p->cond);
	}
	if (!p->sw_count)
		return;

	struct mp_pipe_frame *head = deque_data(&p->sw_frames, 0);
	int64_t dur = p->out_next_pts - p->out_pts;
	bool seamless = !p->out_set || head->pts <= p->out_next_pts + dur / 2;
	bool standing = p->out_set && p->clock_pts && p->clock_pts >= p->out_next_pts &&
			os_gettime_ns() - p->fb_start_ns >= MP_PIPE_FB_STANDING_NS;
	if (!seamless && !standing)
		return;

	int64_t gap = seamless ? 0 : head->pts - p->out_next_pts;
	while (p->sw_count) {
		struct mp_pipe_frame item;
		deque_pop_front(&p->sw_frames, &item, sizeof(item));
		p->sw_count--;
		push_out(p, &item);
	}

	p->fb = MP_PIPE_FB_SWITCHED;
	clear_packets(p, &p->video_packets);
	pthread_cond_broadcast(&p->cond);
	wake_media(p);

	if (gap > 0)
		blog(LOG_INFO,
		     "[stable-stream-source] switched to software decoding after %" PRIu64 " ms, gap of %" PRId64 " ms",
		     (os_gettime_ns() - p->fb_start_ns) / 1000000, gap / 1000000);
	else
		blog(LOG_INFO, "[stable-stream-source] switched to software decoding after %" PRIu64 " ms, no gap",
		     (os_gettime_ns() - p->fb_start_ns) / 1000000);
}

/* mutex held */
static void note_trouble(struct mp_pipe *p, int points)
{
	if (p->fb != MP_PIPE_FB_NONE || !p->sw_valid)
		return;

	uint64_t now = os_gettime_ns();
	if (!p->trouble_since || now - p->trouble_since > MP_PIPE_FB_WINDOW_NS) {
		p->trouble_since = now;
		p->trouble = 0;
	}
	p->trouble += points;
	if (p->trouble < MP_PIPE_FB_TROUBLE)
		return;

	if (p->gop_valid && p->gop_packets.size) {
		/* hand over the packets since the last keyframe */
		while (p->gop_packets.size) {
			AVPacket *pkt;
			deque_pop_front(&p->gop_packets, &pkt, sizeof(pkt));
			deque_push_back(&p->sw_packets, &pkt, sizeof(pkt));
		}
		p->gop_valid = false;
		p->fb = MP_PIPE_FB_RUNNING;
		p->fb_start_ns = now;
		pthread_cond_broadcast(&p->cond);
		blog(LOG_INFO, "[stable-stream-source] hardware decoding keeps falling behind, "
			       "software decoding starts in parallel at the last keyframe");
		return;
	}

	clear_packets(p, &p->gop_packets);
	p->gop_valid = false;
	p->fb = MP_PIPE_FB_WAIT_KEY;
	blog(LOG_INFO, "[stable-stream-source] hardware decoding keeps falling behind, "
		       "starting software decoding at the next keyframe");
}

/* hands a decoded frame on; returns false when the pipe stops */
static bool push_frame(struct mp_pipe *p, struct mp_pipe_vdec *dec, struct mp_pipe_frame *item)
{
	pthread_mutex_lock(&p->mutex);

	if (!dec->sw) {
		while (!pipe_stopped(p) && p->fb != MP_PIPE_FB_SWITCHED && p->hw_count >= MP_PIPE_HW_MAX)
			pthread_cond_wait(&p->cond, &p->mutex);
		if (pipe_stopped(p) || p->fb == MP_PIPE_FB_SWITCHED) {
			bool stopped = pipe_stopped(p);
			pthread_mutex_unlock(&p->mutex);
			av_frame_free(&item->frame);
			return !stopped;
		}
		deque_push_back(&p->hw_frames, item, sizeof(*item));
		p->hw_count++;
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);
		return true;
	}

	/* software decoder in parallel: wait for the switch */
	while (!pipe_stopped(p) && p->fb == MP_PIPE_FB_RUNNING && p->sw_count >= MP_PIPE_SW_MAX)
		pthread_cond_wait(&p->cond, &p->mutex);

	if (!pipe_stopped(p) && p->fb == MP_PIPE_FB_RUNNING) {
		if (p->out_set && item->pts <= p->out_pts) {
			av_frame_free(&item->frame);
		} else {
			deque_push_back(&p->sw_frames, item, sizeof(*item));
			p->sw_count++;
			try_switch(p);
		}
		pthread_mutex_unlock(&p->mutex);
		return true;
	}

	/* software decoder delivers */
	while (!pipe_stopped(p) && p->fb == MP_PIPE_FB_SWITCHED && p->out_count >= MP_PIPE_OUT_MAX)
		pthread_cond_wait(&p->cond, &p->mutex);
	if (pipe_stopped(p) || p->fb != MP_PIPE_FB_SWITCHED) {
		bool stopped = pipe_stopped(p);
		pthread_mutex_unlock(&p->mutex);
		av_frame_free(&item->frame);
		return !stopped;
	}
	if (p->out_set && item->pts <= p->out_pts && p->out_pts - item->pts < MP_PIPE_JUMP_NS) {
		av_frame_free(&item->frame);
	} else {
		push_out(p, item);
		wake_media(p);
	}
	pthread_mutex_unlock(&p->mutex);
	return true;
}

/* returns false when the pipe stops */
static bool receive_frames(struct mp_pipe *p, struct mp_pipe_vdec *dec)
{
	for (;;) {
		struct mp_pipe_frame item = {0};
		item.frame = av_frame_alloc();
		if (!item.frame)
			return true;

		int ret = avcodec_receive_frame(dec->c, item.frame);
		if (ret < 0) {
			av_frame_free(&item.frame);
			return true;
		}

		set_frame_pts(p, dec, &item);
		if (!push_frame(p, dec, &item))
			return false;
	}
}

/* pkt NULL drains the decoder; returns false when the pipe stops */
static bool decode_packet(struct mp_pipe *p, struct mp_pipe_vdec *dec, AVPacket *pkt)
{
	for (int tries = 0; tries < 64; tries++) {
		int ret = avcodec_send_packet(dec->c, pkt);
		bool again = ret == AVERROR(EAGAIN);
		if (ret < 0 && !again) {
			if (ret != AVERROR_EOF) {
				pthread_mutex_lock(&p->mutex);
				p->decode_errors++;
				pthread_mutex_unlock(&p->mutex);
			}
			return receive_frames(p, dec);
		}

		if (!receive_frames(p, dec))
			return false;
		if (!again)
			return true;
	}

	return true;
}

/* returns true if the packet is skipped */
static bool skip_late_packet(struct mp_pipe *p, struct mp_pipe_vdec *dec, AVPacket *pkt, int64_t clock_pts)
{
	int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
	if (!clock_pts || ts == AV_NOPTS_VALUE)
		return false;

	int64_t ns = av_rescale_q(ts, p->m->v.stream->time_base, (AVRational){1, 1000000000});
	int64_t behind = clock_pts - ns;
	uint64_t now = os_gettime_ns();
	uint64_t arrived = (uint64_t)(uintptr_t)pkt->opaque;
	uint64_t waited = arrived && now > arrived ? now - arrived : 0;
	/* far off the clock: a timestamp jump, unless the packet has been
	 * waiting long (the decoder stalled) */
	bool jump = (behind > MP_PIPE_JUMP_NS || behind < -MP_PIPE_JUMP_NS) && waited < (uint64_t)MP_PIPE_SKIP_NS;

	if (!dec->skipping) {
		if (behind <= MP_PIPE_SKIP_NS || jump)
			return false;
		dec->skipping = true;
		dec->skip_start_ns = os_gettime_ns();
		dec->skip_packets = 0;
		blog(LOG_INFO,
		     "[stable-stream-source] video decoding %" PRId64 " ms behind, skipping to the next keyframe",
		     behind / 1000000);
	}

	if ((pkt->flags & AV_PKT_FLAG_KEY) && (behind <= 0 || jump)) {
		avcodec_flush_buffers(dec->c);
		dec->skipping = false;
		pthread_mutex_lock(&p->mutex);
		p->skips++;
		if (!dec->sw)
			note_trouble(p, MP_PIPE_FB_TROUBLE);
		pthread_mutex_unlock(&p->mutex);
		blog(LOG_INFO,
		     "[stable-stream-source] video resumes at a keyframe after %" PRIu64 " ms, skipped %" PRIu64
		     " packets",
		     (os_gettime_ns() - dec->skip_start_ns) / 1000000, dec->skip_packets);
		return false;
	}

	dec->skip_packets++;
	return true;
}

static void *decode_thread(void *data)
{
	struct mp_pipe *p = data;

	os_set_thread_name("ssrc_vdecode");

	for (;;) {
		AVPacket *pkt = NULL;
		int64_t clock_pts;

		pthread_mutex_lock(&p->mutex);
		while (!pipe_stopped(p) && !p->video_packets.size && !p->read_done)
			pthread_cond_wait(&p->cond, &p->mutex);
		if (pipe_stopped(p)) {
			pthread_mutex_unlock(&p->mutex);
			break;
		}
		if (p->video_packets.size)
			deque_pop_front(&p->video_packets, &pkt, sizeof(pkt));
		clock_pts = p->clock_pts;
		bool idle = p->fb == MP_PIPE_FB_SWITCHED;
		pthread_mutex_unlock(&p->mutex);

		if (pkt && (idle || skip_late_packet(p, &p->hwd, pkt, clock_pts))) {
			mp_media_free_packet(p->m, pkt);
			continue;
		}

		bool running = idle || decode_packet(p, &p->hwd, pkt);
		if (pkt)
			mp_media_free_packet(p->m, pkt);
		if (!running)
			break;

		if (!pkt) {
			pthread_mutex_lock(&p->mutex);
			p->decode_done = true;
			pthread_cond_broadcast(&p->cond);
			pthread_mutex_unlock(&p->mutex);
			wake_media(p);
			break;
		}
	}

	return NULL;
}

static void *copy_thread(void *data)
{
	struct mp_pipe *p = data;
	struct mp_decode *d = &p->m->v;

	os_set_thread_name("ssrc_vcopy");

	for (;;) {
		struct mp_pipe_frame item;

		pthread_mutex_lock(&p->mutex);
		while (!pipe_stopped(p) && !p->hw_count && !p->decode_done)
			pthread_cond_wait(&p->cond, &p->mutex);
		if (pipe_stopped(p)) {
			pthread_mutex_unlock(&p->mutex);
			break;
		}
		if (!p->hw_count) {
			if (p->fb != MP_PIPE_FB_SWITCHED)
				p->copy_done = true;
			pthread_mutex_unlock(&p->mutex);
			wake_media(p);
			break;
		}
		deque_pop_front(&p->hw_frames, &item, sizeof(item));
		p->hw_count--;
		bool idle = p->fb == MP_PIPE_FB_SWITCHED;
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);

		if (idle) {
			av_frame_free(&item.frame);
			continue;
		}

		if (d->hw && item.frame->format == d->hw_format) {
			AVFrame *sw = av_frame_alloc();
			int err = sw ? av_hwframe_transfer_data(sw, item.frame, 0) : AVERROR(ENOMEM);
			if (err == 0)
				err = av_frame_copy_props(sw, item.frame);
			av_frame_free(&item.frame);
			if (err) {
				av_frame_free(&sw);
				pthread_mutex_lock(&p->mutex);
				p->copy_errors++;
				pthread_mutex_unlock(&p->mutex);
				continue;
			}
			item.frame = sw;
		}

		pthread_mutex_lock(&p->mutex);
		while (!pipe_stopped(p) && p->fb != MP_PIPE_FB_SWITCHED && p->out_count >= MP_PIPE_OUT_MAX)
			pthread_cond_wait(&p->cond, &p->mutex);
		if (pipe_stopped(p)) {
			pthread_mutex_unlock(&p->mutex);
			av_frame_free(&item.frame);
			break;
		}
		if (p->fb == MP_PIPE_FB_SWITCHED) {
			pthread_mutex_unlock(&p->mutex);
			av_frame_free(&item.frame);
			continue;
		}
		push_out(p, &item);
		try_switch(p);
		pthread_mutex_unlock(&p->mutex);
		wake_media(p);
	}

	return NULL;
}

static bool open_sw_decoder(struct mp_pipe *p)
{
	struct mp_decode *d = &p->m->v;
	AVCodecContext *c = avcodec_alloc_context3(d->codec);
	if (!c)
		return false;

	c->thread_count = 0;
	if (avcodec_parameters_to_context(c, d->stream->codecpar) < 0 || avcodec_open2(c, d->codec, NULL) < 0) {
		avcodec_free_context(&c);
		return false;
	}

	p->swd.c = c;
	return true;
}

static void *sw_thread(void *data)
{
	struct mp_pipe *p = data;

	os_set_thread_name("ssrc_vsoftware");

	for (;;) {
		AVPacket *pkt = NULL;

		pthread_mutex_lock(&p->mutex);
		while (!pipe_stopped(p) && !p->sw_packets.size && !(p->read_done && p->fb == MP_PIPE_FB_SWITCHED))
			pthread_cond_wait(&p->cond, &p->mutex);
		if (pipe_stopped(p)) {
			pthread_mutex_unlock(&p->mutex);
			break;
		}
		if (p->sw_packets.size)
			deque_pop_front(&p->sw_packets, &pkt, sizeof(pkt));
		bool switched = p->fb == MP_PIPE_FB_SWITCHED;
		int64_t clock_pts = p->clock_pts;
		if (p->fb == MP_PIPE_FB_RUNNING && os_gettime_ns() - p->fb_start_ns > MP_PIPE_FB_GIVE_UP_NS)
			fallback_give_up(p, "could not catch up");
		pthread_mutex_unlock(&p->mutex);

		if (!p->swd.c && pkt && !open_sw_decoder(p)) {
			pthread_mutex_lock(&p->mutex);
			fallback_give_up(p, "could not be started");
			pthread_mutex_unlock(&p->mutex);
		}

		if (!p->swd.c || (pkt && switched && skip_late_packet(p, &p->swd, pkt, clock_pts))) {
			if (pkt)
				mp_media_free_packet(p->m, pkt);
			continue;
		}

		bool running = decode_packet(p, &p->swd, pkt);
		if (pkt)
			mp_media_free_packet(p->m, pkt);
		if (!running)
			break;

		if (!pkt) {
			pthread_mutex_lock(&p->mutex);
			p->copy_done = true;
			pthread_mutex_unlock(&p->mutex);
			wake_media(p);
			break;
		}
	}

	return NULL;
}

bool mp_pipe_start(struct mp_pipe *p, struct mp_media *m)
{
	memset(p, 0, sizeof(*p));
	p->m = m;
	p->hwd.c = m->v.decoder;

	if (pthread_mutex_init(&p->mutex, NULL) != 0)
		return false;
	p->mutex_valid = true;
	if (pthread_cond_init(&p->cond, NULL) != 0)
		return false;
	p->cond_valid = true;
	if (os_event_init(&p->wake, OS_EVENT_TYPE_AUTO) != 0)
		return false;

	if (pthread_create(&p->decode_thread, NULL, decode_thread, p) != 0)
		return false;
	p->decode_valid = true;
	if (pthread_create(&p->copy_thread, NULL, copy_thread, p) != 0)
		return false;
	p->copy_valid = true;

	/* the software fallback only makes sense if the GPU decodes (the test
	 * harness builds with MP_PIPE_TEST_FALLBACK to try it without a GPU) */
#ifdef MP_PIPE_TEST_FALLBACK
	if (true) {
#else
	if (m->v.hw) {
#endif
		p->swd.sw = true;
		if (pthread_create(&p->sw_thread, NULL, sw_thread, p) == 0)
			p->sw_valid = true;
	}

	if (pthread_create(&p->read_thread, NULL, read_thread, p) != 0)
		return false;
	p->read_valid = true;

	p->active = true;
	return true;
}

static void free_frames(struct deque *q)
{
	clear_frames(q);
	deque_free(q);
}

static void free_packets(struct mp_pipe *p, struct deque *q)
{
	clear_packets(p, q);
	deque_free(q);
}

/* the reader must be able to leave av_read_frame: the interrupt callback of
 * the format context checks mp_pipe_stopping() */
void mp_pipe_free(struct mp_pipe *p)
{
	if (!p->m)
		return;

	os_atomic_set_bool(&p->stop, true);
	if (p->mutex_valid) {
		pthread_mutex_lock(&p->mutex);
		if (p->cond_valid)
			pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);
	}

	if (p->read_valid)
		pthread_join(p->read_thread, NULL);
	if (p->decode_valid)
		pthread_join(p->decode_thread, NULL);
	if (p->copy_valid)
		pthread_join(p->copy_thread, NULL);
	if (p->sw_valid)
		pthread_join(p->sw_thread, NULL);

	free_packets(p, &p->audio_packets);
	free_packets(p, &p->video_packets);
	free_packets(p, &p->sw_packets);
	free_packets(p, &p->gop_packets);
	free_frames(&p->hw_frames);
	free_frames(&p->out_frames);
	free_frames(&p->sw_frames);

	if (p->swd.c)
		avcodec_free_context(&p->swd.c);
	if (p->wake)
		os_event_destroy(p->wake);
	if (p->cond_valid)
		pthread_cond_destroy(&p->cond);
	if (p->mutex_valid)
		pthread_mutex_destroy(&p->mutex);

	memset(p, 0, sizeof(*p));
}

bool mp_pipe_stopping(struct mp_pipe *p)
{
	return p->m && pipe_stopped(p);
}

size_t mp_pipe_take_audio(struct mp_pipe *p, struct deque *dst)
{
	size_t n = 0;

	pthread_mutex_lock(&p->mutex);
	while (p->audio_packets.size) {
		AVPacket *pkt;
		deque_pop_front(&p->audio_packets, &pkt, sizeof(pkt));
		deque_push_back(dst, &pkt, sizeof(pkt));
		n++;
	}
	pthread_mutex_unlock(&p->mutex);

	return n;
}

bool mp_pipe_pop_video(struct mp_pipe *p, struct mp_pipe_frame *out)
{
	bool got = false;

	pthread_mutex_lock(&p->mutex);
	if (p->out_count) {
		deque_pop_front(&p->out_frames, out, sizeof(*out));
		p->out_count--;
		pthread_cond_broadcast(&p->cond);
		got = true;
	}
	pthread_mutex_unlock(&p->mutex);

	return got;
}

/* the buffer ran empty; counts as trouble only if undecoded data waits */
void mp_pipe_note_empty(struct mp_pipe *p)
{
	pthread_mutex_lock(&p->mutex);
	if (p->video_packets.size || p->hw_count)
		note_trouble(p, 1);
	pthread_mutex_unlock(&p->mutex);
}

void mp_pipe_set_clock(struct mp_pipe *p, int64_t clock_pts)
{
	pthread_mutex_lock(&p->mutex);
	p->clock_pts = clock_pts;
	/* a standing picture may allow the switch now */
	try_switch(p);
	pthread_mutex_unlock(&p->mutex);
}

bool mp_pipe_peek_video_pts(struct mp_pipe *p, int64_t *pts)
{
	bool got = false;

	pthread_mutex_lock(&p->mutex);
	if (p->out_count) {
		struct mp_pipe_frame *item = deque_data(&p->out_frames, 0);
		*pts = item->pts;
		got = true;
	}
	pthread_mutex_unlock(&p->mutex);

	return got;
}

size_t mp_pipe_video_buffered(struct mp_pipe *p)
{
	pthread_mutex_lock(&p->mutex);
	size_t n = p->out_count;
	pthread_mutex_unlock(&p->mutex);
	return n;
}

void mp_pipe_wait(struct mp_pipe *p, uint32_t ms)
{
	os_event_timedwait(p->wake, ms);
}

bool mp_pipe_read_done(struct mp_pipe *p, int *ret)
{
	pthread_mutex_lock(&p->mutex);
	bool done = p->read_done && !p->audio_packets.size;
	if (ret)
		*ret = p->read_ret;
	pthread_mutex_unlock(&p->mutex);
	return done;
}

bool mp_pipe_video_done(struct mp_pipe *p)
{
	pthread_mutex_lock(&p->mutex);
	bool done = p->copy_done && !p->out_count;
	pthread_mutex_unlock(&p->mutex);
	return done;
}

void mp_pipe_get_stats(struct mp_pipe *p, struct mp_pipe_stats *s)
{
	pthread_mutex_lock(&p->mutex);
	s->decode_errors = p->decode_errors;
	s->copy_errors = p->copy_errors;
	s->dropped_packets = p->dropped_packets;
	s->skips = p->skips;
	s->software = p->fb == MP_PIPE_FB_SWITCHED;
	pthread_mutex_unlock(&p->mutex);
}
