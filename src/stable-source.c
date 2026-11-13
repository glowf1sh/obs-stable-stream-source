/*
Stable Stream Source for OBS Studio
Copyright (C) 2026 Glowfish GmbH & Co. KG

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * A media source for network streams that keeps audio and video in sync
 * across interruptions.
 *
 * The OBS media source passes stream timestamps to libobs. After a gap or a
 * new sender session the timestamps jump; libobs then re-anchors the audio
 * timing to the current clock but keeps the video timing relative to the
 * old anchor, so audio and video drift apart until the source is restarted.
 *
 * This source maps stream time to wall-clock time with ONE mapping shared by
 * audio and video, and hands every frame to libobs at its mapped time with a
 * timestamp close to the current clock. When the stream timestamps jump, the
 * mapping is re-anchored for both streams at once. libobs never sees a
 * timestamp jump, so it never re-anchors audio on its own.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <plugin-support.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>

#define S_URL "url"
#define S_FORMAT "input_format"
#define S_BUFFER "buffer_ms"
#define S_RECONNECT "reconnect_s"
#define S_CLOSE_INACTIVE "close_when_inactive"
#define S_CLEAR "clear_on_disconnect"
#define S_RANGE "color_range"

#define NS_PER_MS 1000000LL
/* frames later than this or earlier than buffer + this trigger a re-anchor */
#define LATE_LIMIT_NS (1000LL * NS_PER_MS)
#define EARLY_LIMIT_NS (2000LL * NS_PER_MS)
/* audio is handed over slightly ahead of its timestamp so OBS never has to
 * grow its audio buffering */
#define AUDIO_LEAD_NS (100LL * NS_PER_MS)
#define MAX_QUEUE 3000

enum range_mode { RANGE_AUTO = 0, RANGE_PARTIAL = 1, RANGE_FULL = 2 };

struct queued_frame {
	struct queued_frame *next;
	AVFrame *frame;
	bool video;
	int64_t ts;
};

struct stable_source {
	obs_source_t *source;

	/* settings */
	char *url;
	char *format;
	int64_t buffer_ns;
	int reconnect_s;
	bool close_inactive;
	bool clear_on_disconnect;
	enum range_mode range;

	/* threads */
	pthread_t reader_thread;
	pthread_t player_thread;
	bool threads_running;
	volatile long stop;

	/* frame queue, sorted by ts */
	pthread_mutex_t mutex;
	struct queued_frame *head;
	size_t count;

	/* one stream-to-clock mapping for audio and video */
	bool anchored;
	int64_t pts_base;
	int64_t wall_base;
	int64_t last_queued_ts;
};

/* ------------------------------------------------------------------------ */
/* format conversion                                                        */

static enum video_format convert_pixel_format(int f)
{
	switch (f) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUYV422:
		return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_YVYU422:
		return VIDEO_FORMAT_YVYU;
	case AV_PIX_FMT_UYVY422:
		return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_YUVA420P:
		return VIDEO_FORMAT_I40A;
	case AV_PIX_FMT_YUVA422P:
		return VIDEO_FORMAT_I42A;
	case AV_PIX_FMT_YUVA444P:
		return VIDEO_FORMAT_YUVA;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

static enum audio_format convert_sample_format(int f)
{
	switch (f) {
	case AV_SAMPLE_FMT_U8:
		return AUDIO_FORMAT_U8BIT;
	case AV_SAMPLE_FMT_S16:
		return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32:
		return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_FLT:
		return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_U8P:
		return AUDIO_FORMAT_U8BIT_PLANAR;
	case AV_SAMPLE_FMT_S16P:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32P:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLTP:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

static enum speaker_layout convert_speaker_layout(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

static enum video_colorspace convert_colorspace(const AVFrame *f)
{
	switch (f->colorspace) {
	case AVCOL_SPC_BT709:
		return VIDEO_CS_709;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		return f->color_trc == AVCOL_TRC_ARIB_STD_B67 ? VIDEO_CS_2100_HLG : VIDEO_CS_2100_PQ;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		return VIDEO_CS_601;
	default:
		return f->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
	}
}

/* ------------------------------------------------------------------------ */
/* queue and timing                                                         */

static void free_queue(struct stable_source *s)
{
	pthread_mutex_lock(&s->mutex);
	struct queued_frame *q = s->head;
	while (q) {
		struct queued_frame *n = q->next;
		av_frame_free(&q->frame);
		bfree(q);
		q = n;
	}
	s->head = NULL;
	s->count = 0;
	s->anchored = false;
	s->last_queued_ts = 0;
	pthread_mutex_unlock(&s->mutex);
}

/* caller holds the mutex */
static int64_t map_timestamp(struct stable_source *s, int64_t pts_ns)
{
	int64_t now = (int64_t)os_gettime_ns();

	if (s->anchored) {
		int64_t ts = s->wall_base + (pts_ns - s->pts_base);
		if (ts >= now - LATE_LIMIT_NS && ts <= now + s->buffer_ns + EARLY_LIMIT_NS)
			return ts;

		obs_log(LOG_INFO, "[%s] stream time jumped by %lld ms, audio and video re-synchronized",
			obs_source_get_name(s->source), (long long)((ts - (now + s->buffer_ns)) / NS_PER_MS));
	}

	int64_t target = now + s->buffer_ns;
	/* frames of the old mapping that are still queued play first */
	if (s->last_queued_ts && target <= s->last_queued_ts)
		target = s->last_queued_ts + NS_PER_MS;

	s->wall_base = target;
	s->pts_base = pts_ns;
	s->anchored = true;
	return target;
}

static void enqueue_frame(struct stable_source *s, AVFrame *frame, bool video, int64_t pts_ns)
{
	struct queued_frame *q = bzalloc(sizeof(*q));
	q->frame = av_frame_clone(frame);
	q->video = video;

	pthread_mutex_lock(&s->mutex);
	q->ts = map_timestamp(s, pts_ns);
	if (q->ts > s->last_queued_ts)
		s->last_queued_ts = q->ts;

	struct queued_frame **p = &s->head;
	while (*p && (*p)->ts <= q->ts)
		p = &(*p)->next;
	q->next = *p;
	*p = q;
	s->count++;

	/* the player is not keeping up: drop the oldest frame */
	if (s->count > MAX_QUEUE) {
		struct queued_frame *old = s->head;
		s->head = old->next;
		s->count--;
		av_frame_free(&old->frame);
		bfree(old);
	}
	pthread_mutex_unlock(&s->mutex);
}

/* ------------------------------------------------------------------------ */
/* output to libobs                                                         */

static void output_video(struct stable_source *s, const AVFrame *f, int64_t ts)
{
	struct obs_source_frame frame = {0};
	enum video_format format = convert_pixel_format(f->format);

	if (format == VIDEO_FORMAT_NONE) {
		static bool warned = false;
		if (!warned) {
			obs_log(LOG_WARNING, "[%s] unsupported pixel format %d", obs_source_get_name(s->source),
				f->format);
			warned = true;
		}
		return;
	}

	for (size_t i = 0; i < MAX_AV_PLANES && i < AV_NUM_DATA_POINTERS; i++) {
		frame.data[i] = f->data[i];
		frame.linesize[i] = f->linesize[i] < 0 ? 0 : (uint32_t)f->linesize[i];
	}

	bool full;
	if (s->range == RANGE_FULL)
		full = true;
	else if (s->range == RANGE_PARTIAL)
		full = false;
	else
		full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
		       f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P;

	frame.format = format;
	frame.width = (uint32_t)f->width;
	frame.height = (uint32_t)f->height;
	frame.full_range = full;
	frame.timestamp = (uint64_t)ts;
	frame.trc = VIDEO_TRC_DEFAULT;
	video_format_get_parameters_for_format(convert_colorspace(f), full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
					       format, frame.color_matrix, frame.color_range_min,
					       frame.color_range_max);

	obs_source_output_video(s->source, &frame);
}

static void output_audio(struct stable_source *s, const AVFrame *f, int64_t ts)
{
	struct obs_source_audio audio = {0};
	enum audio_format format = convert_sample_format(f->format);

	if (format == AUDIO_FORMAT_UNKNOWN)
		return;

	for (size_t i = 0; i < MAX_AV_PLANES && i < AV_NUM_DATA_POINTERS; i++)
		audio.data[i] = f->data[i];

	audio.frames = (uint32_t)f->nb_samples;
	audio.samples_per_sec = (uint32_t)f->sample_rate;
	audio.speakers = convert_speaker_layout(f->ch_layout.nb_channels);
	audio.format = format;
	audio.timestamp = (uint64_t)ts;

	obs_source_output_audio(s->source, &audio);
}

static void *player_thread(void *data)
{
	struct stable_source *s = data;
	os_set_thread_name("stable-stream-player");

	while (!os_atomic_load_long(&s->stop)) {
		pthread_mutex_lock(&s->mutex);
		struct queued_frame *q = s->head;
		if (!q) {
			pthread_mutex_unlock(&s->mutex);
			os_sleep_ms(2);
			continue;
		}

		int64_t due = q->ts - (q->video ? 0 : AUDIO_LEAD_NS);
		int64_t now = (int64_t)os_gettime_ns();
		if (now < due) {
			pthread_mutex_unlock(&s->mutex);
			int64_t wait_ms = (due - now) / NS_PER_MS;
			os_sleep_ms(wait_ms > 5 ? 5 : (wait_ms < 1 ? 1 : (uint32_t)wait_ms));
			continue;
		}

		s->head = q->next;
		s->count--;
		pthread_mutex_unlock(&s->mutex);

		if (q->video)
			output_video(s, q->frame, q->ts);
		else
			output_audio(s, q->frame, q->ts);

		av_frame_free(&q->frame);
		bfree(q);
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* reading and decoding                                                     */

static int interrupt_cb(void *data)
{
	struct stable_source *s = data;
	return os_atomic_load_long(&s->stop) ? 1 : 0;
}

static AVCodecContext *open_decoder(AVStream *st)
{
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec)
		return NULL;

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return NULL;

	if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
		avcodec_free_context(&ctx);
		return NULL;
	}
	ctx->pkt_timebase = st->time_base;
	ctx->thread_count = 0;
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

	if (avcodec_open2(ctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx);
		return NULL;
	}
	return ctx;
}

static void decode_packet(struct stable_source *s, AVCodecContext *ctx, AVStream *st, AVPacket *pkt, AVFrame *frame,
			  bool video)
{
	if (avcodec_send_packet(ctx, pkt) < 0)
		return;

	while (avcodec_receive_frame(ctx, frame) == 0) {
		int64_t pts = frame->best_effort_timestamp;
		if (pts == AV_NOPTS_VALUE)
			pts = frame->pts;
		if (pts != AV_NOPTS_VALUE) {
			int64_t pts_ns = av_rescale_q(pts, st->time_base, (AVRational){1, 1000000000});
			enqueue_frame(s, frame, video, pts_ns);
		}
		av_frame_unref(frame);
	}
}

static void sleep_interruptible(struct stable_source *s, int seconds)
{
	for (int i = 0; i < seconds * 10 && !os_atomic_load_long(&s->stop); i++)
		os_sleep_ms(100);
}

static void *reader_thread(void *data)
{
	struct stable_source *s = data;
	os_set_thread_name("stable-stream-reader");

	while (!os_atomic_load_long(&s->stop)) {
		if (!s->url || !*s->url) {
			sleep_interruptible(s, 1);
			continue;
		}

		AVFormatContext *fmt = avformat_alloc_context();
		fmt->interrupt_callback.callback = interrupt_cb;
		fmt->interrupt_callback.opaque = s;

		AVDictionary *opts = NULL;
		av_dict_set(&opts, "rw_timeout", "5000000", 0);
		const AVInputFormat *ifmt = (s->format && *s->format) ? av_find_input_format(s->format) : NULL;

		AVCodecContext *vctx = NULL, *actx = NULL;
		AVPacket *pkt = NULL;
		AVFrame *frame = NULL;
		int vidx = -1, aidx = -1;

		int ret = avformat_open_input(&fmt, s->url, ifmt, &opts);
		av_dict_free(&opts);
		if (ret < 0) {
			fmt = NULL;
			goto disconnected;
		}
		if (avformat_find_stream_info(fmt, NULL) < 0)
			goto disconnected;

		vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
		aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
		if (vidx >= 0)
			vctx = open_decoder(fmt->streams[vidx]);
		if (aidx >= 0)
			actx = open_decoder(fmt->streams[aidx]);
		if (!vctx && !actx)
			goto disconnected;

		obs_log(LOG_INFO, "[%s] connected (%s%s%s)", obs_source_get_name(s->source),
			vctx ? avcodec_get_name(vctx->codec_id) : "", vctx && actx ? " + " : "",
			actx ? avcodec_get_name(actx->codec_id) : "");

		pkt = av_packet_alloc();
		frame = av_frame_alloc();
		while (!os_atomic_load_long(&s->stop) && av_read_frame(fmt, pkt) >= 0) {
			if (pkt->stream_index == vidx && vctx)
				decode_packet(s, vctx, fmt->streams[vidx], pkt, frame, true);
			else if (pkt->stream_index == aidx && actx)
				decode_packet(s, actx, fmt->streams[aidx], pkt, frame, false);
			av_packet_unref(pkt);
		}

	disconnected:
		av_packet_free(&pkt);
		av_frame_free(&frame);
		avcodec_free_context(&vctx);
		avcodec_free_context(&actx);
		if (fmt)
			avformat_close_input(&fmt);

		if (os_atomic_load_long(&s->stop))
			break;

		obs_log(LOG_INFO, "[%s] disconnected, reconnecting in %d s", obs_source_get_name(s->source),
			s->reconnect_s);
		if (s->clear_on_disconnect)
			obs_source_output_video(s->source, NULL);
		sleep_interruptible(s, s->reconnect_s);
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* source callbacks                                                         */

static void start_threads(struct stable_source *s)
{
	if (s->threads_running)
		return;
	os_atomic_set_long(&s->stop, 0);
	if (pthread_create(&s->player_thread, NULL, player_thread, s) != 0)
		return;
	if (pthread_create(&s->reader_thread, NULL, reader_thread, s) != 0) {
		os_atomic_set_long(&s->stop, 1);
		pthread_join(s->player_thread, NULL);
		return;
	}
	s->threads_running = true;
}

static void stop_threads(struct stable_source *s)
{
	if (!s->threads_running)
		return;
	os_atomic_set_long(&s->stop, 1);
	pthread_join(s->reader_thread, NULL);
	pthread_join(s->player_thread, NULL);
	s->threads_running = false;
	free_queue(s);
}

static const char *stable_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("StableStreamSource");
}

static void stable_update(void *data, obs_data_t *settings)
{
	struct stable_source *s = data;
	bool was_running = s->threads_running;

	stop_threads(s);

	bfree(s->url);
	bfree(s->format);
	s->url = bstrdup(obs_data_get_string(settings, S_URL));
	s->format = bstrdup(obs_data_get_string(settings, S_FORMAT));
	s->buffer_ns = obs_data_get_int(settings, S_BUFFER) * NS_PER_MS;
	s->reconnect_s = (int)obs_data_get_int(settings, S_RECONNECT);
	s->close_inactive = obs_data_get_bool(settings, S_CLOSE_INACTIVE);
	s->clear_on_disconnect = obs_data_get_bool(settings, S_CLEAR);
	s->range = (enum range_mode)obs_data_get_int(settings, S_RANGE);

	if (!s->close_inactive || obs_source_active(s->source) || was_running)
		start_threads(s);
}

static void *stable_create(obs_data_t *settings, obs_source_t *source)
{
	struct stable_source *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init(&s->mutex, NULL);
	/* frames are paced by this source, OBS shows them on arrival */
	obs_source_set_async_unbuffered(source, true);
	stable_update(s, settings);
	return s;
}

static void stable_destroy(void *data)
{
	struct stable_source *s = data;
	stop_threads(s);
	pthread_mutex_destroy(&s->mutex);
	bfree(s->url);
	bfree(s->format);
	bfree(s);
}

static void stable_activate(void *data)
{
	struct stable_source *s = data;
	if (s->close_inactive)
		start_threads(s);
}

static void stable_deactivate(void *data)
{
	struct stable_source *s = data;
	if (s->close_inactive) {
		stop_threads(s);
		obs_source_output_video(s->source, NULL);
	}
}

static void stable_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_BUFFER, 250);
	obs_data_set_default_int(settings, S_RECONNECT, 2);
	obs_data_set_default_bool(settings, S_CLOSE_INACTIVE, false);
	obs_data_set_default_bool(settings, S_CLEAR, false);
	obs_data_set_default_int(settings, S_RANGE, RANGE_AUTO);
}

static obs_properties_t *stable_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_text(props, S_URL, obs_module_text("Url"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p, obs_module_text("Url.Hint"));
	obs_properties_add_text(props, S_FORMAT, obs_module_text("InputFormat"), OBS_TEXT_DEFAULT);
	obs_properties_add_int_slider(props, S_BUFFER, obs_module_text("Buffer"), 0, 5000, 50);
	obs_properties_add_int(props, S_RECONNECT, obs_module_text("ReconnectDelay"), 1, 60, 1);
	obs_properties_add_bool(props, S_CLOSE_INACTIVE, obs_module_text("CloseWhenInactive"));
	obs_properties_add_bool(props, S_CLEAR, obs_module_text("ClearOnDisconnect"));

	p = obs_properties_add_list(props, S_RANGE, obs_module_text("ColorRange"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("ColorRange.Auto"), RANGE_AUTO);
	obs_property_list_add_int(p, obs_module_text("ColorRange.Partial"), RANGE_PARTIAL);
	obs_property_list_add_int(p, obs_module_text("ColorRange.Full"), RANGE_FULL);

	return props;
}

struct obs_source_info stable_stream_source_info = {
	.id = "stable_stream_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = stable_get_name,
	.create = stable_create,
	.destroy = stable_destroy,
	.update = stable_update,
	.activate = stable_activate,
	.deactivate = stable_deactivate,
	.get_defaults = stable_defaults,
	.get_properties = stable_properties,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};
