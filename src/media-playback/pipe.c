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
 */

#include "media-playback.h"
#include "media.h"
#include "pipe.h"

#include <inttypes.h>
#include <util/platform.h>

#define MP_PIPE_HW_MAX 3
#define MP_PIPE_OUT_MAX 8

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

static void drop_front_packet(struct mp_pipe *p, struct deque *q)
{
	AVPacket *pkt;
	deque_pop_front(q, &pkt, sizeof(pkt));
	mp_media_free_packet(p->m, pkt);
	p->dropped_packets++;
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

		struct deque *q = NULL;
		if (pkt->size && pkt->stream_index == audio_index)
			q = &p->audio_packets;
		else if (pkt->size && pkt->stream_index == video_index)
			q = &p->video_packets;

		if (!q) {
			mp_media_free_packet(m, pkt);
			continue;
		}

		pthread_mutex_lock(&p->mutex);
		if (pipe_stopped(p)) {
			pthread_mutex_unlock(&p->mutex);
			mp_media_free_packet(m, pkt);
			break;
		}
		size_t max = q == &p->audio_packets ? MP_PIPE_AUDIO_PACKETS_MAX : MP_PIPE_VIDEO_PACKETS_MAX;
		while (q->size / sizeof(AVPacket *) >= max)
			drop_front_packet(p, q);
		deque_push_back(q, &pkt, sizeof(pkt));
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);

		if (q == &p->audio_packets)
			wake_media(p);
	}

	return NULL;
}

/* same timestamp rules as mp_decode_next() for video */
static void set_frame_pts(struct mp_pipe *p, struct mp_pipe_frame *item)
{
	struct mp_decode *d = &p->m->v;
	AVFrame *f = item->frame;
	int64_t last_pts = p->frame_pts;

	if (f->best_effort_timestamp == AV_NOPTS_VALUE)
		p->frame_pts = p->next_pts;
	else
		p->frame_pts =
			av_rescale_q(f->best_effort_timestamp, d->stream->time_base, (AVRational){1, 1000000000});

	int64_t duration = f->duration;
	if (duration) {
		duration = av_rescale_q(duration, d->stream->time_base, (AVRational){1, 1000000000});
	} else if (last_pts) {
		duration = p->frame_pts - last_pts;
	} else if (p->last_duration) {
		duration = p->last_duration;
	} else {
		duration = av_rescale_q(d->decoder->time_base.num, d->decoder->time_base, (AVRational){1, 1000000000});
	}

	p->last_duration = duration;
	p->next_pts = p->frame_pts + duration;
	item->pts = p->frame_pts;
	item->next_pts = p->next_pts;
}

/* returns false when the pipe stops */
static bool receive_frames(struct mp_pipe *p)
{
	AVCodecContext *c = p->m->v.decoder;

	for (;;) {
		struct mp_pipe_frame item = {0};
		item.frame = av_frame_alloc();
		if (!item.frame)
			return true;

		int ret = avcodec_receive_frame(c, item.frame);
		if (ret < 0) {
			av_frame_free(&item.frame);
			return true;
		}

		set_frame_pts(p, &item);

		pthread_mutex_lock(&p->mutex);
		while (!pipe_stopped(p) && p->hw_count >= MP_PIPE_HW_MAX)
			pthread_cond_wait(&p->cond, &p->mutex);
		if (pipe_stopped(p)) {
			pthread_mutex_unlock(&p->mutex);
			av_frame_free(&item.frame);
			return false;
		}
		deque_push_back(&p->hw_frames, &item, sizeof(item));
		p->hw_count++;
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);
	}
}

/* pkt NULL drains the decoder; returns false when the pipe stops */
static bool decode_packet(struct mp_pipe *p, AVPacket *pkt)
{
	AVCodecContext *c = p->m->v.decoder;

	for (int tries = 0; tries < 64; tries++) {
		int ret = avcodec_send_packet(c, pkt);
		bool again = ret == AVERROR(EAGAIN);
		if (ret < 0 && !again) {
			if (ret != AVERROR_EOF) {
				pthread_mutex_lock(&p->mutex);
				p->decode_errors++;
				pthread_mutex_unlock(&p->mutex);
			}
			return receive_frames(p);
		}

		if (!receive_frames(p))
			return false;
		if (!again)
			return true;
	}

	return true;
}

/* returns true if the packet is skipped */
static bool skip_late_packet(struct mp_pipe *p, AVPacket *pkt, int64_t clock_pts)
{
	int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
	if (!clock_pts || ts == AV_NOPTS_VALUE)
		return false;

	int64_t ns = av_rescale_q(ts, p->m->v.stream->time_base, (AVRational){1, 1000000000});
	int64_t behind = clock_pts - ns;
	bool jump = behind > MP_PIPE_JUMP_NS || behind < -MP_PIPE_JUMP_NS;

	if (!p->skipping) {
		if (behind <= MP_PIPE_SKIP_NS || jump)
			return false;
		p->skipping = true;
		p->skip_start_ns = os_gettime_ns();
		p->skip_packets = 0;
		blog(LOG_INFO,
		     "[stable-stream-source] video decoding %" PRId64 " ms behind, skipping to the next keyframe",
		     behind / 1000000);
	}

	if ((pkt->flags & AV_PKT_FLAG_KEY) && (behind <= 0 || jump)) {
		avcodec_flush_buffers(p->m->v.decoder);
		p->skipping = false;
		pthread_mutex_lock(&p->mutex);
		p->skips++;
		pthread_mutex_unlock(&p->mutex);
		blog(LOG_INFO,
		     "[stable-stream-source] video resumes at a keyframe after %" PRIu64 " ms, skipped %" PRIu64
		     " packets",
		     (os_gettime_ns() - p->skip_start_ns) / 1000000, p->skip_packets);
		return false;
	}

	p->skip_packets++;
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
		pthread_mutex_unlock(&p->mutex);

		if (pkt && skip_late_packet(p, pkt, clock_pts)) {
			mp_media_free_packet(p->m, pkt);
			continue;
		}

		bool running = decode_packet(p, pkt);
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
			p->copy_done = true;
			pthread_mutex_unlock(&p->mutex);
			wake_media(p);
			break;
		}
		deque_pop_front(&p->hw_frames, &item, sizeof(item));
		p->hw_count--;
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);

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
		while (!pipe_stopped(p) && p->out_count >= MP_PIPE_OUT_MAX)
			pthread_cond_wait(&p->cond, &p->mutex);
		if (pipe_stopped(p)) {
			pthread_mutex_unlock(&p->mutex);
			av_frame_free(&item.frame);
			break;
		}
		deque_push_back(&p->out_frames, &item, sizeof(item));
		p->out_count++;
		pthread_mutex_unlock(&p->mutex);
		wake_media(p);
	}

	return NULL;
}

bool mp_pipe_start(struct mp_pipe *p, struct mp_media *m)
{
	memset(p, 0, sizeof(*p));
	p->m = m;

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
	if (pthread_create(&p->read_thread, NULL, read_thread, p) != 0)
		return false;
	p->read_valid = true;

	p->active = true;
	return true;
}

static void free_frames(struct deque *q)
{
	while (q->size) {
		struct mp_pipe_frame item;
		deque_pop_front(q, &item, sizeof(item));
		av_frame_free(&item.frame);
	}
	deque_free(q);
}

static void free_packets(struct mp_pipe *p, struct deque *q)
{
	while (q->size) {
		AVPacket *pkt;
		deque_pop_front(q, &pkt, sizeof(pkt));
		mp_media_free_packet(p->m, pkt);
	}
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

	free_packets(p, &p->audio_packets);
	free_packets(p, &p->video_packets);
	free_frames(&p->hw_frames);
	free_frames(&p->out_frames);

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

void mp_pipe_set_clock(struct mp_pipe *p, int64_t clock_pts)
{
	pthread_mutex_lock(&p->mutex);
	p->clock_pts = clock_pts;
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
	pthread_mutex_unlock(&p->mutex);
}
