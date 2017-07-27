/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2015, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Seven Du <dujinfang@gmail.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Seven Du <dujinfang@gmail.com>
 * Anthony Minessale <anthm@freeswitch.org>
 *
 * mod_avformat -- File Formats with libav.org
 *
 */

#include <switch.h>
GCC_DIAG_OFF(deprecated-declarations)
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavresample/avresample.h>
#include <libswscale/swscale.h>
GCC_DIAG_ON(deprecated-declarations)
#define SCALE_FLAGS SWS_BICUBIC
#define DFT_RECORD_OFFSET 0


#ifndef AVUTIL_TIMESTAMP_H
#define AVUTIL_TIMESTAMP_H

#define AV_TS_MAX_STRING_SIZE 32


/* App interface */

// a wrapper around a single output AVStream
typedef struct MediaStream {
	AVStream *st;
	AVFrame *frame;
	AVFrame *tmp_frame;

	// audio
	int channels;
	int sample_rate;
	struct AVAudioResampleContext *resample_ctx;

	//video
	int width;
	int height;
	struct SwsContext *sws_ctx;
	int64_t next_pts;

} MediaStream;

typedef struct record_helper_s {
	switch_mutex_t *mutex;
	AVFormatContext *fc;
	MediaStream *video_st;
	switch_timer_t *video_timer;
	int in_callback;
	switch_queue_t *video_queue;
	switch_thread_t *video_thread;
	switch_mm_t *mm;
	int finalize;
	switch_file_handle_t *fh;
	switch_time_t record_timer_paused;
	uint64_t last_ts;
} record_helper_t;



/* file interface */

struct av_file_context {
	switch_memory_pool_t *pool;
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	switch_buffer_t *buf;
	switch_buffer_t *audio_buffer;
	switch_timer_t video_timer;
	switch_timer_t audio_timer;
	int offset;
	int audio_start;
	int aud_ready;
	int vid_ready;
	int audio_ready;
	int closed;

	MediaStream video_st;
	MediaStream audio_st;
	AVFormatContext *fc;
	AVCodec *audio_codec;
	AVCodec *video_codec;

	int has_audio;
	int has_video;

	record_helper_t eh;
	switch_thread_t *file_read_thread;
	int file_read_thread_running;
	int file_read_thread_started;
	switch_time_t video_start_time;
	switch_image_t *last_img;
	int read_fps;
	switch_time_t last_vid_push;
	int64_t seek_ts;
	switch_bool_t read_paused;
	int errs;
	switch_file_handle_t *handle;
};

typedef struct av_file_context av_file_context_t;



//FUCK


/**
 * Fill the provided buffer with a string containing a timestamp
 * representation.
 *
 * @param buf a buffer with size in bytes of at least AV_TS_MAX_STRING_SIZE
 * @param ts the timestamp to represent
 * @return the buffer in input
 */
static inline char *av_ts_make_string(char *buf, int64_t ts)
{
    if (ts == AV_NOPTS_VALUE) snprintf(buf, AV_TS_MAX_STRING_SIZE, "NOPTS");
    else                      snprintf(buf, AV_TS_MAX_STRING_SIZE, "%"PRId64"", ts);
    return buf;
}

/**
 * Convenience macro, the return value should be used only directly in
 * function arguments but never stand-alone.
 */
#define av_ts2str(ts) av_ts_make_string((char[AV_TS_MAX_STRING_SIZE]){0}, ts)

/**
 * Fill the provided buffer with a string containing a timestamp time
 * representation.
 *
 * @param buf a buffer with size in bytes of at least AV_TS_MAX_STRING_SIZE
 * @param ts the timestamp to represent
 * @param tb the timebase of the timestamp
 * @return the buffer in input
 */
static inline char *av_ts_make_time_string(char *buf, int64_t ts, AVRational *tb)
{
    if (ts == AV_NOPTS_VALUE) snprintf(buf, AV_TS_MAX_STRING_SIZE, "NOPTS");
    else                      snprintf(buf, AV_TS_MAX_STRING_SIZE, "%.6g", av_q2d(*tb) * ts);
    return buf;
}

/**
 * Convenience macro, the return value should be used only directly in
 * function arguments but never stand-alone.
 */
#define av_ts2timestr(ts, tb) av_ts_make_time_string((char[AV_TS_MAX_STRING_SIZE]){0}, ts, tb)

#endif /* AVUTIL_TIMESTAMP_H */


static switch_status_t av_file_close(switch_file_handle_t *handle);
SWITCH_MODULE_LOAD_FUNCTION(mod_avformat_load);

static char *const get_error_text(const int error)
{
	static char error_buffer[255];
	av_strerror(error, error_buffer, sizeof(error_buffer));
	return error_buffer;
}

static void av_unused fill_avframe(AVFrame *pict, switch_image_t *img)
{
	int i;
	uint8_t *y = img->planes[0];
	uint8_t *u = img->planes[1];
	uint8_t *v = img->planes[2];

	/* Y */
	for (i = 0; i < pict->height; i++) {
		memcpy(&pict->data[0][i * pict->linesize[0]], y + i * img->stride[0], pict->width);
	}

	/* U/V */
	for(i = 0; i < pict->height / 2; i++) {
		memcpy(&pict->data[1][i * pict->linesize[1]], u + i * img->stride[1], pict->width / 2);
		memcpy(&pict->data[2][i * pict->linesize[2]], v + i * img->stride[2], pict->width / 2);
	}
}

static void av_unused avframe2img(AVFrame *pict, switch_image_t *img)
{
	int i, j;

	if (img->fmt == SWITCH_IMG_FMT_I420) {
		if (pict->format == AV_PIX_FMT_YUV420P) {
			switch_I420_copy2(pict->data, pict->linesize, img->planes, img->stride, img->d_w, img->d_h);
		} else if (pict->format == AV_PIX_FMT_YUVA420P) {
			int linesize[3];
			linesize[0] = pict->linesize[0];
			linesize[1] = pict->linesize[1];
			linesize[2] = pict->linesize[2] + pict->linesize[0];

			switch_I420_copy2(pict->data, linesize, img->planes, img->stride, img->d_w, img->d_h);
		}

	} else if (img->fmt == SWITCH_IMG_FMT_ARGB) {
		if (pict->format == AV_PIX_FMT_YUV420P) {
			switch_rgb_color_t *color = (switch_rgb_color_t *)img->planes[SWITCH_PLANE_PACKED];
			uint8_t *alpha = pict->data[3];

			/*!\brief I420 to ARGB Convertion*/
			switch_I420ToARGB(pict->data[0], pict->linesize[0],
				pict->data[1], pict->linesize[1],
				pict->data[2], pict->linesize[2],
				img->planes[SWITCH_PLANE_PACKED], img->stride[SWITCH_PLANE_PACKED],
				img->d_w, img->d_h);


			for (j = 0; j < img->d_h; j++) {
				for (i = 0; i < img->d_w; i++) {
					color->a = *alpha++;
					color++;
				}
				color = (switch_rgb_color_t *)(img->planes[SWITCH_PLANE_PACKED] + img->stride[SWITCH_PLANE_PACKED] * j);
			}
		} else if (pict->format == AV_PIX_FMT_RGBA) {
#if SWITCH_BYTE_ORDER == __BIG_ENDIAN
			switch_RGBAToARGB(pict->data[0], pict->linesize[0],
				mg->planes[SWITCH_PLANE_PACKED], img->stride[SWITCH_PLANE_PACKED],
				img->d_w, img->d_h);
#else
			switch_ABGRToARGB(pict->data[0], pict->linesize[0],
				img->planes[SWITCH_PLANE_PACKED], img->stride[SWITCH_PLANE_PACKED],
				img->d_w, img->d_h);
#endif
		} else if (pict->format == AV_PIX_FMT_BGRA) {
#if SWITCH_BYTE_ORDER == __BIG_ENDIAN
			switch_BGRAToARGB(pict->data[0], pict->linesize[0],
			, img->planes[SWITCH_PLANE_PACKED], img->stride[SWITCH_PLANE_PACKED],
			, img->d_w, img->d_h);
#else
			switch_ARGBToARGB(pict->data[0], pict->linesize[0],
			img->planes[SWITCH_PLANE_PACKED], img->stride[SWITCH_PLANE_PACKED],
			img->d_w, img->d_h);
#endif
		}
	}
}

static void av_unused avframe2fd(AVFrame *pict, int fd)
{
	int i;
	uint8_t *y = pict->data[0];
	uint8_t *u = pict->data[1];
	uint8_t *v = pict->data[2];

	/* Y */
	for (i = 0; i < pict->height; i++) {
		write(fd, y + i * pict->linesize[0], pict->width);
	}

	/* U/V */
	for(i = 0; i < pict->height / 2; i++) {
		write(fd, u + i * pict->linesize[1], pict->width / 2);
	}

	for(i = 0; i < pict->height / 2; i++) {
		write(fd, v + i * pict->linesize[2], pict->width / 2);
	}
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
	AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

	printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
		   av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
		   av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
		   av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
		   pkt->stream_index);
}

static int mod_avformat_alloc_output_context2(AVFormatContext **avctx, AVOutputFormat *oformat,
								   const char *format, const char *filename)
{
	AVFormatContext *s = avformat_alloc_context();
	int ret = 0;

	*avctx = NULL;
	if (!s)
		goto nomem;

	if (!oformat) {
		if (format) {
			oformat = av_guess_format(format, NULL, NULL);
			if (!oformat) {
				av_log(s, AV_LOG_ERROR, "Requested output format '%s' is not a suitable output format\n", format);
				ret = AVERROR(EINVAL);
				goto error;
			}
		} else {
			oformat = av_guess_format(NULL, filename, NULL);
			if (!oformat) {
				ret = AVERROR(EINVAL);
				av_log(s, AV_LOG_ERROR, "Unable to find a suitable output format for '%s'\n",
					   filename);
				goto error;
			}
		}
	}

	s->oformat = oformat;
	if (s->oformat->priv_data_size > 0) {
		s->priv_data = av_mallocz(s->oformat->priv_data_size);
		if (!s->priv_data)
			goto nomem;
		if (s->oformat->priv_class) {
			*(const AVClass**)s->priv_data= s->oformat->priv_class;
			av_opt_set_defaults(s->priv_data);
		}
	} else
		s->priv_data = NULL;

	if (filename)
		av_strlcpy(s->filename, filename, sizeof(s->filename));
	*avctx = s;
	return 0;
nomem:
	av_log(s, AV_LOG_ERROR, "Out of memory\n");
	ret = AVERROR(ENOMEM);
error:
	avformat_free_context(s);
	return ret;
}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
	/* rescale output packet timestamp values from codec to stream timebase */
	av_packet_rescale_ts(pkt, *time_base, st->time_base);
	pkt->stream_index = st->index;

	/* Write the compressed frame to the media file. */
	if (0) log_packet(fmt_ctx, pkt);
	return av_interleaved_write_frame(fmt_ctx, pkt);
}

/* Add an output stream. */
static switch_status_t add_stream(MediaStream *mst, AVFormatContext *fc, AVCodec **codec, enum AVCodecID codec_id, switch_mm_t *mm)
{
	AVCodecContext *c;
	switch_status_t status = SWITCH_STATUS_FALSE;
	int threads = switch_core_cpu_count();
	int buffer_bytes = 2097152; /* 2 mb */
	int fps = 15;

	if (mm->try_hardware_encoder && codec_id == AV_CODEC_ID_H264) {
		*codec = avcodec_find_encoder_by_name("nvenc_h264");
	}

	if (!*codec) {
		/* find the encoder */
		*codec = avcodec_find_encoder(codec_id);
	}

	if (!(*codec)) {
		 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not find encoder\n");
		return status;
	}

	mst->st = avformat_new_stream(fc, *codec);
	if (!mst->st) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not allocate stream\n");
		return status;
	}
	mst->st->id = fc->nb_streams - 1;
GCC_DIAG_OFF(deprecated-declarations)
	c = mst->st->codec;
GCC_DIAG_ON(deprecated-declarations)
	//switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "id:%d den:%d num:%d\n", mst->st->id, mst->st->time_base.den, mst->st->time_base.num);

	if (threads > 4) {
		threads = 4;
	}

	switch ((*codec)->type) {
	case AVMEDIA_TYPE_AUDIO:
		c->sample_fmt  = (*codec)->sample_fmts ? (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
		c->bit_rate    = 128000;
		c->sample_rate = mst->sample_rate = 44100;
		c->channels    = mst->channels;
		c->channel_layout = av_get_default_channel_layout(c->channels);

		if (mm) {
			if (mm->ab) {
				c->bit_rate = mm->ab * 1024;
			}
			if (mm->samplerate) {
				c->sample_rate = mst->sample_rate = mm->samplerate;
			}
		}
		//FUCKER
		mst->st->time_base.den = c->sample_rate;
		mst->st->time_base.num = 1;
		c->time_base.den = c->sample_rate;
		c->time_base.num = 1;

		break;

	case AVMEDIA_TYPE_VIDEO:

		if (mm) {
			if (mm->vbuf) {
				buffer_bytes = mm->vbuf;
			}
			if (mm->fps) {
				fps = mm->fps;
			} else {
				mm->fps = fps;
			}

			if (mm->vw && mm->vh) {
				mst->width = mm->vw;
				mst->height = mm->vh;
			}

		}

		c->codec_id = codec_id;

		/* Resolution must be a multiple of two. */
		c->width    = mst->width;
		c->height   = mst->height;
		c->bit_rate = mm->vb;
		mst->st->time_base.den = 90000;
		mst->st->time_base.num = 1;
		c->time_base.den = 90000;
		c->time_base.num = 1;
		c->gop_size      = 25; /* emit one intra frame every x frames at mmst */
		c->pix_fmt       = AV_PIX_FMT_YUV420P;
		c->thread_count  = threads;
		c->rc_initial_buffer_occupancy = buffer_bytes * 8;

		if (codec_id == AV_CODEC_ID_H264) {
			c->ticks_per_frame = 2;


			c->flags|=CODEC_FLAG_LOOP_FILTER;   // flags=+loop
			c->me_cmp|= 1;  // cmp=+chroma, where CHROMA = 1
			c->me_range = 16;   // me_range=16
			c->max_b_frames = 3;    // bf=3

			av_opt_set_int(c->priv_data, "b_strategy", 1, 0);
			av_opt_set_int(c->priv_data, "motion_est", ME_HEX, 0);
			av_opt_set_int(c->priv_data, "coder", 1, 0);

			switch (mm->vprofile) {
			case SWITCH_VIDEO_PROFILE_BASELINE:
				av_opt_set(c->priv_data, "profile", "baseline", 0);
				c->level = 41;
				break;
			case SWITCH_VIDEO_PROFILE_MAIN:
				av_opt_set(c->priv_data, "profile", "main", 0);
				av_opt_set(c->priv_data, "level", "5", 0);
				c->level = 5;
				break;
			case SWITCH_VIDEO_PROFILE_HIGH:
				av_opt_set(c->priv_data, "profile", "high", 0);
				av_opt_set(c->priv_data, "level", "52", 0);
				c->level = 52;
				break;
			}

			switch (mm->vencspd) {
			case SWITCH_VIDEO_ENCODE_SPEED_SLOW:
				av_opt_set(c->priv_data, "preset", "veryslow", 0);
				break;
			case SWITCH_VIDEO_ENCODE_SPEED_MEDIUM:
				av_opt_set(c->priv_data, "preset", "medium", 0);
				break;
			case SWITCH_VIDEO_ENCODE_SPEED_FAST:
				//av_opt_set(c->priv_data, "tune", "zerolatency", 0);
				av_opt_set(c->priv_data, "preset", "veryfast", 0);
				break;
			default:
				break;
			}
		}


		if (mm->cbr) {
			c->rc_min_rate = c->bit_rate;
			c->rc_max_rate = c->bit_rate;
			c->rc_buffer_size = c->bit_rate;
			c->qcompress = 0;
		} else {
			c->gop_size = 250;  // g=250
			c->keyint_min = 25; // keyint_min=25
			c->i_quant_factor = 0.71; // i_qfactor=0.71
			c->qcompress = 0.6; // qcomp=0.6
			c->qmin = 10;   // qmin=10
			c->qmax = 31;   // qmax=31
			c->max_qdiff = 4;   // qdiff=4
			av_opt_set_int(c->priv_data, "crf", 18, 0);
		}

		if (codec_id == AV_CODEC_ID_VP8) {
			av_set_options_string(c, "quality=realtime", "=", ":");
		}

		c->colorspace = AVCOL_SPC_RGB;
		c->color_range = AVCOL_RANGE_JPEG;

		if (mm) {
			if (mm->vb) {
				c->bit_rate = mm->vb * 1024;
			}
			if (mm->keyint) {
				c->gop_size = mm->keyint;
			}
		}

		break;
	default:
		break;
	}

	/* Some formats want stream headers to be separate. */
	if (fc->oformat->flags & AVFMT_GLOBALHEADER) {
		c->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}

	return SWITCH_STATUS_SUCCESS;
}

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;
	int ret;

	picture = av_frame_alloc();
	if (!picture) return NULL;

	picture->format = pix_fmt;
	picture->width  = width;
	picture->height = height;

	/* allocate the buffers for the frame data */
	ret = av_frame_get_buffer(picture, 32);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not allocate frame data.\n");
		return NULL;
	}

	return picture;
}

static switch_status_t open_video(AVFormatContext *fc, AVCodec *codec, MediaStream *mst)
{
	int ret;
GCC_DIAG_OFF(deprecated-declarations)
	AVCodecContext *c = mst->st->codec;
GCC_DIAG_ON(deprecated-declarations)
	switch_status_t status = SWITCH_STATUS_FALSE;

	/* open the codec */
	ret = avcodec_open2(c, codec, NULL);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open video codec: %s\n", get_error_text(ret));
		return status;
	}

	/* allocate and init a re-usable frame */
	mst->frame = alloc_picture(c->pix_fmt, c->width, c->height);
	switch_assert(mst->frame);
	mst->frame->pts = 0;

	// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "pix_fmt: %d\n", c->pix_fmt);
	switch_assert(c->pix_fmt == AV_PIX_FMT_YUV420P); // always I420 for NOW

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t open_audio(AVFormatContext *fc, AVCodec *codec, MediaStream *mst)
{
	AVCodecContext *c;
	int ret;
	switch_status_t status = SWITCH_STATUS_FALSE;
GCC_DIAG_OFF(deprecated-declarations)
	c = mst->st->codec;
GCC_DIAG_ON(deprecated-declarations)
	ret = avcodec_open2(c, codec, NULL);

	if (ret == AVERROR_EXPERIMENTAL) {
		const AVCodecDescriptor *desc = avcodec_descriptor_get(c->codec_id);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Codec [%s] is experimental feature in libavcodec, never mind\n", desc->name);
		c->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
		ret = avcodec_open2(c, codec, NULL);
	}

	if (ret < 0) {
		const AVCodecDescriptor *desc = avcodec_descriptor_get(c->codec_id);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open audio codec [%s], error: %s\n", desc->name, get_error_text(ret));
		return status;
	}

	mst->frame = av_frame_alloc();
	switch_assert(mst->frame);

	mst->frame->sample_rate    = c->sample_rate;
	mst->frame->format         = AV_SAMPLE_FMT_S16;
	mst->frame->channel_layout = c->channel_layout;

	if (c->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE) {
		mst->frame->nb_samples = 10000;
	} else {
		mst->frame->nb_samples = c->frame_size;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "sample_rate: %d nb_samples: %d\n", mst->frame->sample_rate, mst->frame->nb_samples);


	if (c->sample_fmt != AV_SAMPLE_FMT_S16) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "sample_fmt %d != AV_SAMPLE_FMT_S16, start resampler\n", c->sample_fmt);

		mst->resample_ctx = avresample_alloc_context();

		if (!mst->resample_ctx) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not allocate resampler context\n");
			return status;
		}

		/* set options */
		av_opt_set_int(mst->resample_ctx, "in_channel_count",   c->channels,       0);
		av_opt_set_int(mst->resample_ctx, "in_sample_rate",     c->sample_rate,    0);
		av_opt_set_int(mst->resample_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);
		av_opt_set_int(mst->resample_ctx, "in_channel_layout",  c->channel_layout, 0);
		av_opt_set_int(mst->resample_ctx, "out_channel_count",  c->channels,       0);
		av_opt_set_int(mst->resample_ctx, "out_sample_rate",    c->sample_rate,    0);
		av_opt_set_int(mst->resample_ctx, "out_sample_fmt",     c->sample_fmt,     0);
		av_opt_set_int(mst->resample_ctx, "out_channel_layout", c->channel_layout, 0);

		if ((ret = avresample_open(mst->resample_ctx)) < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to initialize the resampling context\n");
			av_free(mst->resample_ctx);
			mst->resample_ctx = NULL;
			return status;
		}
	}

	ret = av_frame_get_buffer(mst->frame, 0);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not allocate audio frame.\n");
		return status;
	}

	if (mst->resample_ctx) {
		mst->tmp_frame = av_frame_alloc();
		switch_assert(mst->tmp_frame);

		mst->tmp_frame->sample_rate    = c->sample_rate;
		mst->tmp_frame->format         = c->sample_fmt;
		mst->tmp_frame->channel_layout = c->channel_layout;
		mst->tmp_frame->nb_samples     = mst->frame->nb_samples;

		ret = av_frame_get_buffer(mst->tmp_frame, 0);
		if (ret < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not allocate audio frame.\n");
			return status;
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

static int flush_video_queue(switch_queue_t *q, int min)
{
	void *pop;

	if (switch_queue_size(q) > min) {
		while (switch_queue_trypop(q, &pop) == SWITCH_STATUS_SUCCESS) {
			switch_image_t *img = (switch_image_t *) pop;
			switch_img_free(&img);
			if (min && switch_queue_size(q) <= min) {
				break;
			}
		}
	}

	return switch_queue_size(q);
}

static void *SWITCH_THREAD_FUNC video_thread_run(switch_thread_t *thread, void *obj)
{
	av_file_context_t *context = (av_file_context_t *) obj;
	void *pop = NULL;
	switch_image_t *img = NULL;
	int d_w = context->eh.video_st->width, d_h = context->eh.video_st->height;
	int size = 0, skip = 0, skip_freq = 0, skip_count = 0, skip_total = 0, skip_total_count = 0;
	uint64_t delta_avg = 0, delta_sum = 0, delta_i = 0, delta = 0;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "video thread start\n");
		switch_assert(context->eh.video_queue);
	for(;;) {
		AVPacket pkt = { 0 };
		int got_packet;
		int ret = -1;

	top:

		switch_assert(context->eh.video_queue);

		if (switch_queue_pop(context->eh.video_queue, &pop) == SWITCH_STATUS_SUCCESS) {
            switch_img_free(&img);

			if (!pop) {
				goto endfor;
			}

			img = (switch_image_t *) pop;
			
			if (!d_w) d_w = img->d_w;
			if (!d_h) d_h = img->d_h;
			
			//if (d_w && d_h && (d_w != img->d_w || d_h != img->d_h)) {
				/* scale to match established stream */
			//	switch_img_fit(&img, d_w, d_h, SWITCH_FIT_SIZE);
			//}
		} else {
			continue;
		}

		if (skip) {
			if ((skip_total_count > 0 && !--skip_total_count) || ++skip_count >= skip_freq) {
				skip_total_count = skip_total;
				skip_count = 0;
				skip--;

				goto top;
			}
		} else {

			size = switch_queue_size(context->eh.video_queue);

			if (size > 5 && !context->eh.finalize) {
				skip = size;

				if (size > 10) {
					skip_freq = 3;
					skip_total = 1;
				} else {
					skip_freq = 2;
					skip_total = 1;
				}
			}
		}

		//switch_mutex_lock(context->eh.mutex);

		context->eh.in_callback = 1;

		av_init_packet(&pkt);

		if (context->eh.video_st->frame) {
			ret = av_frame_make_writable(context->eh.video_st->frame);
		}

		if (ret < 0) {
			continue;
		}

		if (context->eh.record_timer_paused) {
			context->eh.last_ts = 0;
			continue;
		}		

		fill_avframe(context->eh.video_st->frame, img);

		if (context->eh.finalize) {
			if (delta_i && !delta_avg) {
				delta_avg = (int)(double)(delta_sum / delta_i);
				delta_i = 1;
				delta_sum = delta_avg;
			}

			if (delta_avg) {
				delta = delta_avg;
			} else if (context->eh.mm->fps) {
				delta = 1000 / context->eh.mm->fps;
			} else {
				delta = 33;
			}

			context->eh.video_st->frame->pts += delta;
		} else {
			uint64_t delta_tmp;

			switch_core_timer_sync(context->eh.video_timer);
			delta_tmp = context->eh.video_timer->samplecount - context->eh.last_ts;
			
			if (delta_tmp != 0) {
				delta_sum += delta_tmp;
				delta_i++;
				
				if (delta_i == UINT64_MAX) {
					delta_i = 1;
					delta_sum = delta_avg;
				}
				
				
				if ((delta_i % 10) == 0) {
					delta_avg = (int)(double)(delta_sum / delta_i);
				}
				
				context->eh.video_st->frame->pts = context->eh.video_timer->samplecount;
			} else {
				context->eh.video_st->frame->pts = (context->eh.video_timer->samplecount) + 1;
			}
		}

		context->eh.last_ts = context->eh.video_st->frame->pts;

		//switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "pts: %ld\n", context->eh.video_st->frame->pts);

		/* encode the image */
GCC_DIAG_OFF(deprecated-declarations)
		ret = avcodec_encode_video2(context->eh.video_st->st->codec, &pkt, context->eh.video_st->frame, &got_packet);
GCC_DIAG_ON(deprecated-declarations)
 
		if (ret < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Encoding Error %d\n", ret);
			continue;
		}

		if (got_packet) {
			switch_mutex_lock(context->eh.mutex);
GCC_DIAG_OFF(deprecated-declarations)
			ret = write_frame(context->eh.fc, &context->eh.video_st->st->codec->time_base, context->eh.video_st->st, &pkt);
GCC_DIAG_ON(deprecated-declarations) 
			switch_mutex_unlock(context->eh.mutex);
			av_packet_unref(&pkt);
		}

		context->eh.in_callback = 0;
		//switch_mutex_unlock(context->eh.mutex);
	}

 endfor:

	for(;;) {
		AVPacket pkt = { 0 };
		int got_packet = 0;
		int ret = 0;

		av_init_packet(&pkt);

GCC_DIAG_OFF(deprecated-declarations)		
		ret = avcodec_encode_video2(context->eh.video_st->st->codec, &pkt, NULL, &got_packet);
GCC_DIAG_ON(deprecated-declarations)

		if (ret < 0) {
			break;
		} else if (got_packet) {
			switch_mutex_lock(context->eh.mutex);
GCC_DIAG_OFF(deprecated-declarations)
			ret = write_frame(context->eh.fc, &context->eh.video_st->st->codec->time_base, context->eh.video_st->st, &pkt);
GCC_DIAG_ON(deprecated-declarations)
			switch_mutex_unlock(context->eh.mutex);
			av_packet_unref(&pkt);
			if (ret < 0) break;
		} else {
			break;
		}
	}

	while(switch_queue_trypop(context->eh.video_queue, &pop) == SWITCH_STATUS_SUCCESS) {
		if (!pop) break;
		img = (switch_image_t *) pop;
		switch_img_free(&img);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "video thread done\n");

	return NULL;
}


static switch_status_t video_read_callback(switch_core_session_t *session, switch_frame_t *frame, void *user_data)
{
	record_helper_t *eh = user_data;
	switch_image_t *img = NULL;

	if (frame->img) {
		switch_img_copy(frame->img, &img);
		switch_queue_push(eh->video_queue, img);
	}

	return SWITCH_STATUS_SUCCESS;
}

static void close_stream(AVFormatContext *fc, MediaStream *mst)
{
	if (mst->resample_ctx) avresample_free(&mst->resample_ctx);
	if (mst->sws_ctx) sws_freeContext(mst->sws_ctx);
	if (mst->frame) av_frame_free(&mst->frame);
	if (mst->tmp_frame) av_frame_free(&mst->tmp_frame);

GCC_DIAG_OFF(deprecated-declarations)
	if (mst->st && mst->st->codec) {
		avcodec_close(mst->st->codec);
	}
GCC_DIAG_ON(deprecated-declarations)
}

SWITCH_STANDARD_APP(record_av_function)
{
	switch_status_t status;
	switch_frame_t *read_frame;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_timer_t timer = { 0 };
	switch_mutex_t *mutex = NULL;
	switch_codec_t codec;//, *vid_codec;
	switch_codec_implementation_t read_impl = { 0 };
	switch_dtmf_t dtmf = { 0 };
	switch_buffer_t *buffer = NULL;
	int inuse = 0, bytes = 0;
	switch_vid_params_t vid_params = { 0 };
	int force_sample_rate;

	MediaStream video_st = { 0 }, audio_st = { 0 };
	AVOutputFormat *fmt = NULL;
	const char *format = NULL;
	AVFormatContext *fc = NULL;
	AVCodec *audio_codec, *video_codec;
	int has_audio = 0, has_video = 0;
	int ret;
	av_file_context_t context = { 0 };

	switch_channel_answer(channel);
	switch_core_session_get_read_impl(session, &read_impl);
	switch_core_session_request_video_refresh(session);

	switch_channel_set_variable(channel, SWITCH_PLAYBACK_TERMINATOR_USED, "");

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "%s not ready.\n", switch_channel_get_name(channel));
		switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "Channel not ready");
		goto done;
	}

	switch_channel_set_flag_recursive(channel, CF_VIDEO_DECODED_READ);
	switch_core_media_get_vid_params(session, &vid_params);
	switch_channel_set_flag(channel, CF_VIDEO_ECHO);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "video size: %dx%d\n", vid_params.width, vid_params.height);

	force_sample_rate = read_impl.samples_per_second;

	if (!strncmp(data, "rtmp://", 7)) {
		format = "flv";
		force_sample_rate = 44100;
	}

	if (switch_core_codec_init(&codec,
							   "L16",
							   NULL,
							   NULL,
							   force_sample_rate,
							   read_impl.microseconds_per_packet / 1000,
							   read_impl.number_of_channels, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Audio Codec Activation Success\n");
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Audio Codec Activation Fail\n");
		switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "Audio codec activation failed");
		goto end;
	}

	switch_buffer_create_dynamic(&buffer, 8192, 65536, 0);

	if (!buffer) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not allocate buffer for %s\n", data);
		goto end;
	}

	av_register_all();
	mod_avformat_alloc_output_context2(&fc, NULL, format, data);

	if (!fc) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not deduce output format from file extension\n");
		goto end;
	}

	fmt = fc->oformat;

	/* open the output file, if needed */
	if (!(fmt->flags & AVFMT_NOFILE)) {
		ret = avio_open(&fc->pb, data, AVIO_FLAG_WRITE);
		if (ret < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open '%s': %s\n", data, get_error_text(ret));
			goto end;
		}
	} else {
		avformat_network_init();
	}

	if (fmt->video_codec != AV_CODEC_ID_NONE &&
		switch_channel_test_flag(channel, CF_VIDEO) &&
		vid_params.width > 0 && vid_params.height > 0) {

		char codec_str[256];
		const AVCodecDescriptor *desc;

		if (!strncmp(data, "rtmp://", 7) || !strncmp(data, "rtsp://", 7)) {
			fmt->video_codec = AV_CODEC_ID_H264;
			fmt->audio_codec = AV_CODEC_ID_AAC;
		}

		desc = avcodec_descriptor_get(fmt->video_codec);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "use video codec: [%d] %s (%s)\n", fmt->video_codec, desc->name, desc->long_name);

		video_st.width = vid_params.width;
		video_st.height = vid_params.height;
		video_st.next_pts = switch_time_now() / 1000;
		if (add_stream(&video_st, fc, &video_codec, fmt->video_codec, NULL) == SWITCH_STATUS_SUCCESS &&
			open_video(fc, video_codec, &video_st) == SWITCH_STATUS_SUCCESS) {
GCC_DIAG_OFF(deprecated-declarations)
			avcodec_string(codec_str, sizeof(codec_str), video_st.st->codec, 1);
GCC_DIAG_ON(deprecated-declarations)
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "use video codec implementation %s\n", codec_str);
			has_video = 1;
		}
	}

	if (fmt->audio_codec != AV_CODEC_ID_NONE) {
		audio_st.channels = read_impl.number_of_channels;
		audio_st.sample_rate = force_sample_rate;

		add_stream(&audio_st, fc, &audio_codec, fmt->audio_codec, NULL);
		if (open_audio(fc, audio_codec, &audio_st) != SWITCH_STATUS_SUCCESS) {
			goto end;
		}

		has_audio = 1;
	}

	av_dump_format(fc, 0, data, 1);

	/* Write the stream header, if any. */
	ret = avformat_write_header(fc, NULL);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error occurred when opening output file: %s\n", get_error_text(ret));
		goto end;
	}

	if (has_video) {
		switch_threadattr_t *thd_attr = NULL;
		switch_mutex_init(&mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
		context.eh.mutex = mutex;
		context.eh.video_st = &video_st;
		context.eh.fc = fc;
		if (switch_core_timer_init(&timer, "soft", 1, 90, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Timer Activation Fail\n");
			goto end;
		}
		context.eh.video_timer = &timer;
		switch_queue_create(&context.eh.video_queue, SWITCH_CORE_QUEUE_LEN, switch_core_session_get_pool(session));
		switch_core_session_set_video_read_callback(session, video_read_callback, (void *)&context.eh);

		switch_threadattr_create(&thd_attr, switch_core_session_get_pool(session));
		//switch_threadattr_priority_set(thd_attr, SWITCH_PRI_REALTIME);
		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
		switch_thread_create(&context.eh.video_thread, thd_attr, video_thread_run, &context, switch_core_session_get_pool(session));
	}

	switch_core_session_set_read_codec(session, &codec);

	{
		char buf[SWITCH_RECOMMENDED_BUFFER_SIZE] = {0};
		switch_size_t datalen = codec.implementation->decoded_bytes_per_packet;
		switch_size_t samples = datalen / 2 / codec.implementation->number_of_channels;
		int offset = DFT_RECORD_OFFSET;
		int fps = codec.implementation->actual_samples_per_second / samples;
		int lead_frames = (offset * fps) / 1000;

		for (int x = 0; x < lead_frames; x++) {
			switch_buffer_write(buffer, buf, datalen);
		}
	}

	while (switch_channel_ready(channel)) {

		status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_SINGLE_READ, 0);

		if (switch_channel_test_flag(channel, CF_BREAK)) {
			switch_channel_clear_flag(channel, CF_BREAK);
			break;
		}

		switch_ivr_parse_all_events(session);

		//check for dtmf interrupts
		if (switch_channel_has_dtmf(channel)) {
			const char * terminators = switch_channel_get_variable(channel, SWITCH_PLAYBACK_TERMINATORS_VARIABLE);
			switch_channel_dequeue_dtmf(channel, &dtmf);

			if (terminators && !strcasecmp(terminators, "none"))
			{
				terminators = NULL;
			}

			if (terminators && strchr(terminators, dtmf.digit)) {

				char sbuf[2] = {dtmf.digit, '\0'};
				switch_channel_set_variable(channel, SWITCH_PLAYBACK_TERMINATOR_USED, sbuf);
				break;
			}
		}

		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		if (switch_test_flag(read_frame, SFF_CNG)) {
			continue;
		}

		if (mutex) switch_mutex_lock(mutex);

		switch_buffer_write(buffer, read_frame->data, read_frame->datalen);
GCC_DIAG_OFF(deprecated-declarations)
		bytes = audio_st.frame->nb_samples * 2 * audio_st.st->codec->channels;
GCC_DIAG_ON(deprecated-declarations)
		inuse = switch_buffer_inuse(buffer);

		while (inuse >= bytes) {
			AVPacket pkt = { 0 };
			int got_packet = 0;

			av_init_packet(&pkt);

			// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "inuse: %d samples: %d bytes: %d\n", inuse, audio_st.frame->nb_samples, bytes);

			if (audio_st.resample_ctx) { // need resample
				int out_samples = avresample_get_out_samples(audio_st.resample_ctx, audio_st.frame->nb_samples);

				av_frame_make_writable(audio_st.frame);
				av_frame_make_writable(audio_st.tmp_frame);
				switch_buffer_read(buffer, audio_st.frame->data[0], bytes);
				/* convert to destination format */
				ret = avresample_convert(audio_st.resample_ctx,
						audio_st.tmp_frame->data, 0, out_samples,
						(uint8_t **)audio_st.frame->data, 0, audio_st.frame->nb_samples);

				if (ret < 0) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error while converting %d samples, error text: %s\n",
						audio_st.frame->nb_samples, get_error_text(ret));
					inuse = switch_buffer_inuse(buffer);
					continue;
				}

				audio_st.tmp_frame->pts = audio_st.next_pts;
				audio_st.next_pts += audio_st.frame->nb_samples;

GCC_DIAG_OFF(deprecated-declarations)
				ret = avcodec_encode_audio2(audio_st.st->codec, &pkt, audio_st.tmp_frame, &got_packet);
GCC_DIAG_ON(deprecated-declarations)
			} else {
				av_frame_make_writable(audio_st.frame);
				switch_buffer_read(buffer, audio_st.frame->data[0], bytes);
				audio_st.frame->pts = audio_st.next_pts;
				audio_st.next_pts  += audio_st.frame->nb_samples;

GCC_DIAG_OFF(deprecated-declarations)
				ret = avcodec_encode_audio2(audio_st.st->codec, &pkt, audio_st.frame, &got_packet);
GCC_DIAG_ON(deprecated-declarations)
			}

			if (ret < 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error encoding audio frame: %d\n", ret);
				inuse = switch_buffer_inuse(buffer);
				continue;
			}

			if (got_packet) {
				// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "got pkt: %d\n", pkt.size);
GCC_DIAG_OFF(deprecated-declarations)
				ret = write_frame(fc, &audio_st.st->codec->time_base, audio_st.st, &pkt);
GCC_DIAG_ON(deprecated-declarations)
				if (ret < 0) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error while writing audio frame: %s\n", get_error_text(ret));
					goto end;
				}
			}

			inuse = switch_buffer_inuse(buffer);
		}

		if (mutex) switch_mutex_unlock(mutex);

		//switch_core_session_write_frame(session, read_frame, SWITCH_IO_FLAG_NONE, 0);
	}

	switch_core_session_set_video_read_callback(session, NULL, NULL);

	if (has_video) {
		AVPacket pkt = { 0 };
		int got_packet = 0;
		int ret = 0;
		switch_status_t st;

		switch_queue_push(context.eh.video_queue, NULL);
		switch_thread_join(&st, context.eh.video_thread);

	again:
		av_init_packet(&pkt);
GCC_DIAG_OFF(deprecated-declarations)
		ret = avcodec_encode_video2(video_st.st->codec, &pkt, NULL, &got_packet);
GCC_DIAG_ON(deprecated-declarations)

		if (ret < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Encoding Error %d\n", ret);
			goto end;
		}

		if (got_packet) {
GCC_DIAG_OFF(deprecated-declarations)
			ret = write_frame(fc, &video_st.st->codec->time_base, video_st.st, &pkt);
GCC_DIAG_ON(deprecated-declarations)
			av_packet_unref(&pkt);
			goto again;
		}
	}

	av_write_trailer(fc);
	switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "OK");

  end:

	switch_channel_clear_flag_recursive(channel, CF_VIDEO_DECODED_READ);

	if (fc) {
		if (has_video) close_stream(fc, &video_st);
		if (has_audio) close_stream(fc, &audio_st);

		if (fmt) {
			if (!(fmt->flags & AVFMT_NOFILE)) {
				avformat_close_input(&fc);
			} else {
				avformat_network_deinit();
				avformat_free_context(fc);
			}
		} else {
			avformat_free_context(fc);
		}
	}

	if (timer.interval) {
		switch_core_timer_destroy(&timer);
	}

	switch_core_media_end_video_function(session);
	switch_core_session_set_read_codec(session, NULL);
	switch_core_codec_destroy(&codec);

	if (buffer) switch_buffer_destroy(&buffer);

 done:
	switch_core_session_video_reset(session);
}
/* end of App interface */


/* API interface */

static int is_device(const AVClass *avclass)
{
#if defined (AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT)
	if (!avclass) return 0;


	return  avclass->category == AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT ||
			avclass->category == AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT ||
			avclass->category == AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT ||
			avclass->category == AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT ||
			avclass->category == AV_CLASS_CATEGORY_DEVICE_OUTPUT ||
			avclass->category == AV_CLASS_CATEGORY_DEVICE_INPUT;
#endif

	return 0;

}

void show_formats(switch_stream_handle_t *stream) {
	AVInputFormat *ifmt  = NULL;
	AVOutputFormat *ofmt = NULL;
	const char *last_name;
	// int is_dev;

	stream->write_function(stream, "============= File Formats ==============================:\n"
		   " D. = Demuxing supported\n"
		   " .M = Muxing supported\n"
		   "----------------------\n");

	last_name = "000";

	for (;;) {
		int decode = 0;
		int encode = 0;
		int is_dev = 0;
		const char *name      = NULL;
		const char *long_name = NULL;

		while ((ofmt = av_oformat_next(ofmt))) {
			is_dev = is_device(ofmt->priv_class);

			if ((name == NULL || strcmp(ofmt->name, name) < 0) &&
				strcmp(ofmt->name, last_name) > 0) {
				name      = ofmt->name;
				long_name = ofmt->long_name;
				encode    = 1;
			}
		}

		while ((ifmt = av_iformat_next(ifmt))) {
			is_dev = is_device(ifmt->priv_class);

			if ((name == NULL || strcmp(ifmt->name, name) < 0) &&
				strcmp(ifmt->name, last_name) > 0) {
				name      = ifmt->name;
				long_name = ifmt->long_name;
				encode    = 0;
			}

			if (name && strcmp(ifmt->name, name) == 0) decode = 1;
		}

		if (name == NULL) break;

		last_name = name;

		stream->write_function(stream, "%s%s%s %-15s %s\n",
			is_dev ? "*" : " ",
			decode ? "D" : " ",
			encode ? "M" : " ",
			name, long_name ? long_name:" ");
	}

}

void show_codecs(switch_stream_handle_t *stream);

SWITCH_STANDARD_API(av_format_api_function)
{
	if (zstr(cmd)) {
		show_codecs(stream);
		stream->write_function(stream, "\n");
		show_formats(stream);
	} else {
		if (!strcmp(cmd, "show formats")) {
			show_formats(stream);
		} else if (!strcmp(cmd, "show codecs")) {
			show_codecs(stream);
		} else {
			stream->write_function(stream, "Usage: ffmpeg show <formats|codecs>");
		}
	}

	return SWITCH_STATUS_SUCCESS;
}


static void mod_avformat_destroy_output_context(av_file_context_t *context)
{

	close_stream(context->fc, &context->video_st);
	close_stream(context->fc, &context->audio_st);

	if (context->audio_st.resample_ctx) {
		avresample_free(&context->audio_st.resample_ctx);
	}

	avformat_close_input(&context->fc);

	context->fc = NULL;
	context->audio_st.st = NULL;
	context->video_st.st = NULL;
	
}

static switch_status_t open_input_file(av_file_context_t *context, switch_file_handle_t *handle, const char *filename)
{
	AVCodec *audio_codec = NULL;
	AVCodec *video_codec = NULL;
	AVDictionary *opts = NULL;
	int error;
	int i;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	// av_dict_set(&opts, "c:v", "libvpx", 0);

	/** Open the input file to read from it. */
	if ((error = avformat_open_input(&context->fc, filename, NULL, NULL)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open input file '%s' (error '%s')\n", filename, get_error_text(error));
		switch_goto_status(SWITCH_STATUS_FALSE, err);
	}

	handle->seekable = context->fc->iformat->read_seek2 ? 1 : (context->fc->iformat->read_seek ? 1 : 0);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "file %s is %sseekable\n", filename, handle->seekable ? "" : "not ");

	/** Get information on the input file (number of streams etc.). */
	if ((error = avformat_find_stream_info(context->fc, opts ? &opts : NULL)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open find stream info (error '%s')\n", get_error_text(error));
		if (opts) av_dict_free(&opts);
		switch_goto_status(SWITCH_STATUS_FALSE, err);
	}

	if (opts) av_dict_free(&opts);

	av_dump_format(context->fc, 0, filename, 0);

	for (i = 0; i< context->fc->nb_streams; i++) {
GCC_DIAG_OFF(deprecated-declarations)
		if (context->fc->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && !context->has_audio) {
			context->audio_st.st = context->fc->streams[i];
			context->has_audio = 1;
		} else if (context->fc->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && !context->has_video) {
GCC_DIAG_ON(deprecated-declarations)
			context->video_st.st = context->fc->streams[i];
			if (switch_test_flag(handle, SWITCH_FILE_FLAG_VIDEO)) {
				context->has_video = 1;
				handle->duration = av_rescale_q(context->video_st.st->duration, context->video_st.st->time_base, AV_TIME_BASE_Q);
			}
			if (context->video_st.st->avg_frame_rate.num) {
				handle->mm.source_fps = ceil(av_q2d(context->video_st.st->avg_frame_rate));
			} else {
				handle->mm.source_fps = 25;
			}

			context->read_fps = (int)handle->mm.source_fps;
		}
	}

	/** Find a decoder for the audio stream. */
GCC_DIAG_OFF(deprecated-declarations)
	if (context->has_audio && !(audio_codec = avcodec_find_decoder(context->audio_st.st->codec->codec_id))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not find input codec %d\n", context->audio_st.st->codec->codec_id);
		context->has_audio = 0;
	}

	if (context->has_video && !(video_codec = avcodec_find_decoder(context->video_st.st->codec->codec_id))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not find input codec %d\n", context->video_st.st->codec->codec_id);
		context->has_video = 0;
	}

	if (context->has_audio && (error = avcodec_open2(context->audio_st.st->codec, audio_codec, NULL)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open input codec (error '%s')\n", get_error_text(error));
		context->has_audio = 0;
	}

	if (context->has_video && (error = avcodec_open2(context->video_st.st->codec, video_codec, NULL)) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open input codec (error '%s')\n", get_error_text(error));
		context->has_video = 0;
	}
GCC_DIAG_ON(deprecated-declarations)

	// printf("has audio:%d has_video:%d\n", context->has_audio, context->has_video);

	if ((!context->has_audio) && (!context->has_video)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Neither audio nor video stream found in file %s\n", filename);
		switch_goto_status(SWITCH_STATUS_FALSE, err);
	}

	if (context->has_audio) {
GCC_DIAG_OFF(deprecated-declarations)
		AVCodecContext *c = context->audio_st.st->codec;
GCC_DIAG_ON(deprecated-declarations)

		context->audio_st.frame = av_frame_alloc();
		switch_assert(context->audio_st.frame);

		handle->channels = c->channels > 2 ? 2 : c->channels;
		context->audio_st.channels = handle->channels;
		context->audio_st.sample_rate = handle->samplerate;

GCC_DIAG_OFF(deprecated-declarations)
		if (context->audio_st.st->codec->sample_fmt != AV_SAMPLE_FMT_S16) {
GCC_DIAG_ON(deprecated-declarations)
			AVAudioResampleContext *resample_ctx = avresample_alloc_context();

			if (resample_ctx) {
				int ret;

				av_opt_set_int(resample_ctx, "in_channel_count",   c->channels,       0);
				av_opt_set_int(resample_ctx, "in_sample_rate",     c->sample_rate,    0);
				av_opt_set_int(resample_ctx, "in_sample_fmt",      c->sample_fmt,     0);
				av_opt_set_int(resample_ctx, "in_channel_layout",
					(c->channel_layout == 0 && c->channels == 2) ? AV_CH_LAYOUT_STEREO : c->channel_layout, 0);
				av_opt_set_int(resample_ctx, "out_channel_count",  handle->channels,  0);
				av_opt_set_int(resample_ctx, "out_sample_rate",    handle->samplerate,0);
				av_opt_set_int(resample_ctx, "out_sample_fmt",     AV_SAMPLE_FMT_S16, 0);
				av_opt_set_int(resample_ctx, "out_channel_layout", handle->channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO, 0);

				if ((ret = avresample_open(resample_ctx)) < 0) {
					char errbuf[1024];
					av_strerror(ret, errbuf, 1024);

					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to initialize the resampling context, ret=%d: %s\n", ret, errbuf);
					av_free(resample_ctx);
					switch_goto_status(SWITCH_STATUS_FALSE, err);
				}

				context->audio_st.resample_ctx = resample_ctx;
			}
		}
	}

	if (!context->has_video) {
		switch_clear_flag(handle, SWITCH_FILE_FLAG_VIDEO);
	} else {
GCC_DIAG_OFF(deprecated-declarations)
		switch (context->video_st.st->codec->pix_fmt) {
		case AV_PIX_FMT_YUVA420P:
		case AV_PIX_FMT_RGBA:
		case AV_PIX_FMT_ARGB:
		case AV_PIX_FMT_BGRA:
			context->handle->mm.fmt = SWITCH_IMG_FMT_ARGB;
			break;
		default:
			context->handle->mm.fmt = SWITCH_IMG_FMT_I420;
			break;
		}
GCC_DIAG_ON(deprecated-declarations)
		
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
						  "Opening file in mode: %s\n", context->handle->mm.fmt == SWITCH_IMG_FMT_ARGB ? "ARGB" : "I420");
	}

	return status;

err:
	/*
	if (context->has_video) close_stream(context->fc, &context->video_st);
	if (context->has_audio) close_stream(context->fc, &context->audio_st);

	if (context->fc) avformat_close_input(&context->fc);
	*/
	return status;
}

//#define ALT_WAY
#define AUDIO_BUF_SEC 5

static void *SWITCH_THREAD_FUNC file_read_thread_run(switch_thread_t *thread, void *obj)
{
	av_file_context_t *context = (av_file_context_t *) obj;
	AVPacket pkt = { 0 };
	int got_data = 0;
	int error;
	int sync  = 0;
	int eof = 0;

	switch_mutex_lock(context->mutex);
	context->file_read_thread_started = 1;
	context->file_read_thread_running = 1;
	switch_thread_cond_signal(context->cond);
	switch_mutex_unlock(context->mutex);

	while (context->file_read_thread_running && !context->closed) {
		int vid_frames = 0;

		if (context->seek_ts >= 0) {
			int stream_id = -1;

			switch_mutex_lock(context->mutex);
			switch_buffer_zero(context->audio_buffer);
			switch_mutex_unlock(context->mutex);


			
			// if (context->has_audio) stream_id = context->audio_st.st->index;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "seeking to %" SWITCH_INT64_T_FMT "\n", context->seek_ts);
			avformat_seek_file(context->fc, stream_id, 0, context->seek_ts, INT64_MAX, 0);
			context->seek_ts = -2;

			if (context->has_video) {
				void *pop;
				
				context->video_st.next_pts = 0;
				context->video_start_time = 0;
GCC_DIAG_OFF(deprecated-declarations)
				avcodec_flush_buffers(context->video_st.st->codec);
GCC_DIAG_ON(deprecated-declarations)
				
				while(switch_queue_trypop(context->eh.video_queue, &pop) == SWITCH_STATUS_SUCCESS) {
					switch_image_t *img;
					if (!pop) break;
					img = (switch_image_t *) pop;
					switch_img_free(&img);
				}
			}
		}

		if (context->has_video) {
			vid_frames = switch_queue_size(context->eh.video_queue);
		}
		
		if (switch_buffer_inuse(context->audio_buffer) > AUDIO_BUF_SEC * context->audio_st.sample_rate * context->audio_st.channels * 2 &&
			(!context->has_video || vid_frames > 5)) {
			switch_yield(context->has_video ? 1000 : 10000);
			continue;
		}



		av_init_packet(&pkt);
		pkt.data = NULL;
		pkt.size = 0;

		if ((error = av_read_frame(context->fc, &pkt)) < 0) {
			if (error == AVERROR_EOF) {
				if (!context->has_video) break;

				eof = 1;
				/* just make sure*/
				pkt.data = NULL;
				pkt.size = 0;
				pkt.stream_index = context->video_st.st->index;
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not read frame (error '%s')\n", get_error_text(error));
				break;
			}
		}

		// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "stream: %d, pkt size %d\n", pkt.stream_index, pkt.size);
		if (context->has_video && pkt.stream_index == context->video_st.st->index) {
			AVFrame *vframe;
			switch_image_t *img;
			if (!sync) {
				switch_buffer_zero(context->audio_buffer);
				sync = 1;
			}

again:
			vframe = av_frame_alloc();
			switch_assert(vframe);

GCC_DIAG_OFF(deprecated-declarations)
			if ((error = avcodec_decode_video2(context->video_st.st->codec, vframe, &got_data, &pkt)) < 0) {
GCC_DIAG_ON(deprecated-declarations)
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not decode frame (error '%s')\n", get_error_text(error));
				av_packet_unref(&pkt);
				av_frame_free(&vframe);
				break;
			}

			// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "pkt: %d, pts: %lld dts: %lld\n", pkt.size, pkt.pts, pkt.dts);
			av_packet_unref(&pkt);

			//if (switch_queue_size(context->eh.video_queue) > 300) {
			//	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Dropping frames\n");
			//	av_frame_free(&vframe);
			//	continue;
			//}
			// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "got_data=%d, error=%d\n", got_data, error);

			if (got_data && error >= 0) {
				switch_img_fmt_t fmt = SWITCH_IMG_FMT_I420;
				if ((
						vframe->format == AV_PIX_FMT_YUVA420P ||
						vframe->format == AV_PIX_FMT_RGBA ||
						vframe->format == AV_PIX_FMT_ARGB ||
						vframe->format == AV_PIX_FMT_BGRA )) {
					fmt = SWITCH_IMG_FMT_ARGB;
				} else if (vframe->format != AV_PIX_FMT_YUV420P) {
					AVFrame *frm = vframe;
					int ret;

					if (!context->video_st.sws_ctx) {
						context->video_st.sws_ctx =
							sws_getContext(frm->width, frm->height,
											frm->format,
											frm->width, frm->height,
											AV_PIX_FMT_YUV420P,
											SCALE_FLAGS, NULL, NULL, NULL);
						if (!context->video_st.sws_ctx) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Cannot init sws context\n");
							av_frame_free(&frm);
							continue;
						}
					}

					vframe = av_frame_alloc();
					switch_assert(vframe);

					vframe->format = AV_PIX_FMT_YUV420P;
					vframe->width = frm->width;
					vframe->height = frm->height;
					vframe->pts = frm->pts;
GCC_DIAG_OFF(deprecated-declarations)
					vframe->pkt_pts = frm->pkt_pts;
GCC_DIAG_ON(deprecated-declarations)
					vframe->pkt_dts = frm->pkt_dts;
					ret = av_frame_get_buffer(vframe, 32);

					switch_assert(ret >= 0);

					ret = sws_scale(context->video_st.sws_ctx, (const uint8_t *const *)frm->data, frm->linesize,
						  0, frm->height, vframe->data, vframe->linesize);

					av_frame_free(&frm);

					if (ret <= 0 ) {
						av_frame_free(&vframe);
						continue;
					}
				}

				context->handle->mm.fmt = fmt;

				img = switch_img_alloc(NULL, fmt, vframe->width, vframe->height, 1);

				if (img) {
					int64_t *pts = malloc(sizeof(int64_t));

					if (pts) {
#ifdef ALT_WAY
						int diff;
						int sleep = 66000;
#endif
GCC_DIAG_OFF(deprecated-declarations)
						*pts = vframe->pkt_pts;
GCC_DIAG_ON(deprecated-declarations)
						avframe2img(vframe, img);
						img->user_priv = pts;

#ifdef ALT_WAY
						diff = sleep - (switch_time_now() - context->last_vid_push);

						if (diff > 0 && diff <= sleep) {
							switch_core_timer_next(&context->video_timer);
						} else {
							switch_core_timer_sync(&context->video_timer);
						}
#endif

						context->vid_ready = 1;
						switch_queue_push(context->eh.video_queue, img);
						context->last_vid_push = switch_time_now();
						
						

					}
				}
			}

			av_frame_free(&vframe);

			if (eof) {
				if (got_data) {
					goto again; // to get all delayed video frames in decoder
				} else {
					break;
				}
			}
			continue;
		} else if (context->has_audio && pkt.stream_index == context->audio_st.st->index) {
			AVFrame in_frame = { { 0 } };

GCC_DIAG_OFF(deprecated-declarations)
			if ((error = avcodec_decode_audio4(context->audio_st.st->codec, &in_frame, &got_data, &pkt)) < 0) {
GCC_DIAG_ON(deprecated-declarations)
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not decode frame (error '%s')\n", get_error_text(error));
				av_packet_unref(&pkt);
				break;
			}

			// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "pkt: %d, decodedddd: %d pts: %lld dts: %lld\n", pkt.size, error, pkt.pts, pkt.dts);
			av_packet_unref(&pkt);

			if (got_data) {
				// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "got data frm->format: %d samples: %d\n", in_frame.format, in_frame.nb_samples);

				if (context->audio_st.resample_ctx) {
					int out_samples = avresample_get_out_samples(context->audio_st.resample_ctx, in_frame.nb_samples);
					int ret;
					uint8_t *data[2] = { 0 };

					data[0] = malloc(out_samples * context->audio_st.channels * 2);
					switch_assert(data[0]);

					ret = avresample_convert(context->audio_st.resample_ctx, data, 0, out_samples,
						in_frame.data, 0, in_frame.nb_samples);

					// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "out_samples: %d ret: %d delay: %d buffer: %zu\n", out_samples, ret, avresample_get_delay(context->audio_st.resample_ctx), switch_buffer_inuse(context->audio_buffer));

					if (ret) {
						switch_mutex_lock(context->mutex);
						switch_buffer_write(context->audio_buffer, data[0], ret * 2 * context->audio_st.channels);
						switch_mutex_unlock(context->mutex);
					}

					// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "decoded samples: %d\n", ret);

					free(data[0]);

					// if (ret == 0 && avresample_get_delay(context->audio_st.resample_ctx)) {
					// 	frameP = NULL;
					// 	goto again;
					// }

				} else {
					//switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "this block is not tested samples: %d\n", in_frame.nb_samples);
					switch_mutex_lock(context->mutex);
					switch_buffer_write(context->audio_buffer, in_frame.data[0], in_frame.nb_samples * 2 * context->audio_st.channels);
					switch_mutex_unlock(context->mutex);
				}

			}

		}
	}

	if (context->has_video) switch_queue_push(context->eh.video_queue, NULL);

	context->file_read_thread_running = 0;

	return NULL;
}

static switch_status_t av_file_open(switch_file_handle_t *handle, const char *path)
{
	av_file_context_t *context = NULL;
	char *ext;
	const char *tmp = NULL;
	AVOutputFormat *fmt;
	const char *format = NULL;
	int ret;
	char file[1024];
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_set_string(file, path);

	if ((ext = strrchr((char *)path, '.')) == 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid Format\n");
		return SWITCH_STATUS_GENERR;
	} else if (handle->stream_name && (!strcasecmp(handle->stream_name, "rtmp") || !strcasecmp(handle->stream_name, "youtube"))) {
		format = "flv";

		// meh really silly format for the user / pass libav.....
		if (handle->mm.auth_username && handle->mm.auth_password) { 
			switch_snprintf(file, sizeof(file), "rtmp://%s pubUser=%s pubPasswd=%s flashver=FMLE/3.0", path, handle->mm.auth_username, handle->mm.auth_password);
		} else {
			switch_snprintf(file, sizeof(file), "rtmp://%s", path);
		}

	} else if (handle->stream_name && !strcasecmp(handle->stream_name, "rtsp")) {
		format = "rtsp";
		switch_snprintf(file, sizeof(file), "rtsp://%s", path);
	}


	ext++;

	if ((context = (av_file_context_t *)switch_core_alloc(handle->memory_pool, sizeof(av_file_context_t))) == 0) {
		switch_goto_status(SWITCH_STATUS_MEMERR, end);
	}

	memset(context, 0, sizeof(av_file_context_t));
	handle->private_info = context;
	context->pool = handle->memory_pool;
	context->seek_ts = -1;
	context->offset = DFT_RECORD_OFFSET;
	context->handle = handle;

	if (handle->params) {
		if ((tmp = switch_event_get_header(handle->params, "av_video_offset"))) {
			context->offset = atoi(tmp);
		}
	}

	switch_mutex_init(&context->mutex, SWITCH_MUTEX_NESTED, handle->memory_pool);
	switch_thread_cond_create(&context->cond, handle->memory_pool);
	switch_buffer_create_dynamic(&context->audio_buffer, 512, 512, 0);

	if (!context->audio_buffer) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not allocate buffer for %s\n", path);
		switch_goto_status(SWITCH_STATUS_MEMERR, end);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "sample rate: %d, channels: %d\n", handle->samplerate, handle->channels);

	av_register_all();

	if (switch_test_flag(handle, SWITCH_FILE_FLAG_READ)) {
		if (open_input_file(context, handle, path) != SWITCH_STATUS_SUCCESS) {
			//clean up;
			switch_goto_status(SWITCH_STATUS_GENERR, end);
		}

		if (context->has_video) {
			switch_queue_create(&context->eh.video_queue, context->read_fps, handle->memory_pool);
			switch_mutex_init(&context->eh.mutex, SWITCH_MUTEX_NESTED, handle->memory_pool);
			switch_core_timer_init(&context->video_timer, "soft", (int)(1000.0f / context->read_fps), 1, context->pool);
		}

		{
			switch_threadattr_t *thd_attr = NULL;

			switch_threadattr_create(&thd_attr, handle->memory_pool);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
			switch_thread_create(&context->file_read_thread, thd_attr, file_read_thread_run, context, handle->memory_pool);
		}

		return SWITCH_STATUS_SUCCESS;
	}

	mod_avformat_alloc_output_context2(&context->fc, NULL, format, (char *)file);

	if (!context->fc) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Could not deduce output format from file extension\n");
		switch_goto_status(SWITCH_STATUS_GENERR, end);
	}

	fmt = context->fc->oformat;

	if (handle->params && (tmp = switch_event_get_header(handle->params, "av_audio_codec"))) {
		if ((context->audio_codec = avcodec_find_encoder_by_name(tmp))) {
			fmt->audio_codec = context->audio_codec->id;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "specified audio codec %s %s [%s]\n", 
							  tmp, context->audio_codec->name, context->audio_codec->long_name);
		}
	}

	if (!strcasecmp(ext, "wav") || (handle->params && switch_true(switch_event_get_header(handle->params, "av_record_audio_only")))) {
		context->has_video = 0;
		switch_clear_flag(handle, SWITCH_FILE_FLAG_VIDEO);
	}

	/* open the output file, if needed */
	if (!(fmt->flags & AVFMT_NOFILE)) {
		ret = avio_open(&context->fc->pb, file, AVIO_FLAG_WRITE);
		if (ret < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not open '%s': %s\n", file, get_error_text(ret));
			switch_goto_status(SWITCH_STATUS_GENERR, end);
		}
	} else {
		avformat_network_init();
	}

	if (handle->mm.samplerate) {
		handle->mm.samplerate = handle->samplerate;
	}

	if (!handle->mm.ab) {
		handle->mm.ab = 128;
	}

	if (!handle->mm.vb) {
		handle->mm.vb = switch_calc_bitrate(handle->mm.vw, handle->mm.vh, 1, handle->mm.fps);
	}

	if (fmt->video_codec != AV_CODEC_ID_NONE) {
		const AVCodecDescriptor *desc;

		if ((handle->stream_name && (!strcasecmp(handle->stream_name, "rtmp") || !strcasecmp(handle->stream_name, "rtsp") || !strcasecmp(handle->stream_name, "youtube")))) {

			if (fmt->video_codec != AV_CODEC_ID_H264 ) {
				fmt->video_codec = AV_CODEC_ID_H264; // force H264
			}

			fmt->audio_codec = AV_CODEC_ID_AAC;
			handle->samplerate = 44100;
			handle->mm.samplerate = 44100;
			handle->mm.ab = 128;
			//handle->mm.vencspd = SWITCH_VIDEO_ENCODE_SPEED_FAST;
			handle->mm.vprofile = SWITCH_VIDEO_PROFILE_BASELINE;

			if (!handle->mm.vb && handle->mm.vw && handle->mm.vh) {
				switch(handle->mm.vh) {
				case 240:
					handle->mm.vb = 400;
					break;
				case 360:
					handle->mm.vb = 750;
					break;
				case 480:
					handle->mm.vb = 1000;
					break;
				case 720:
					handle->mm.vb = 2500;
					break;
				case 1080:
					handle->mm.vb = 4500;
					break;
				default:
					handle->mm.vb = switch_calc_bitrate(handle->mm.vw, handle->mm.vh, 1, handle->mm.fps);
					break;
				}
			}

			if (handle->stream_name && handle->mm.fps > 0.0f) {
				handle->mm.keyint = (int) 2.0f * handle->mm.fps;
			}
		}

		desc = avcodec_descriptor_get(fmt->video_codec);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "use video codec: [%d] %s (%s)\n", fmt->video_codec, desc->name, desc->long_name);
	}

	if (fmt->audio_codec != AV_CODEC_ID_NONE) {
		context->audio_st.channels = handle->channels;
		context->audio_st.sample_rate = handle->samplerate;

		add_stream(&context->audio_st, context->fc, &context->audio_codec, fmt->audio_codec, &handle->mm);

		if (open_audio(context->fc, context->audio_codec, &context->audio_st) != SWITCH_STATUS_SUCCESS) {
			switch_goto_status(SWITCH_STATUS_GENERR, end);
		}

		context->has_audio = 1;
	}

	av_dump_format(context->fc, 0, file, 1);

	handle->format = 0;
	handle->sections = 0;
	handle->seekable = 0;
	handle->speed = 0;
	handle->pos = 0;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Opening File [%s] %dhz %s\n",
		file, handle->samplerate, switch_test_flag(handle, SWITCH_FILE_FLAG_VIDEO) ? " with VIDEO" : "");

	return SWITCH_STATUS_SUCCESS;

 end:

	if (!context) {
		return status;
	}

	if (context->fc) {
		mod_avformat_destroy_output_context(context);
	}

	if (context->video_timer.interval) {
		switch_core_timer_destroy(&context->video_timer);
	}

	if (context->audio_timer.interval) {
		switch_core_timer_destroy(&context->audio_timer);
	}

	if (context->audio_buffer) {
		switch_buffer_destroy(&context->audio_buffer);
	}

	return status;
}

static switch_status_t av_file_truncate(switch_file_handle_t *handle, int64_t offset)
{
	return SWITCH_STATUS_FALSE;
}


static switch_status_t av_file_write(switch_file_handle_t *handle, void *data, size_t *len)
{

	uint32_t datalen = 0;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	// uint8_t buf[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 }, *bp = buf;
	// uint32_t encoded_rate;
	av_file_context_t *context = (av_file_context_t *)handle->private_info;
	// uint32_t size = 0;
	uint32_t bytes;
	int inuse;

	if (!switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!context->vid_ready) {
		if (switch_test_flag(handle, SWITCH_FILE_FLAG_VIDEO)) {
			switch_buffer_zero(context->audio_buffer);
			return status;
		} else if (!context->aud_ready) { // audio only recording
			int ret = avformat_write_header(context->fc, NULL);
			if (ret < 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error occurred when opening output file: %s\n", get_error_text(ret));
				return SWITCH_STATUS_FALSE;
			}
			context->aud_ready = 1;
		}
	}

	if (data && len) {
		datalen = *len * 2 * handle->channels;

		if (context->offset) {
			char buf[SWITCH_RECOMMENDED_BUFFER_SIZE] = {0};
			switch_size_t samples = *len;
			int fps = handle->samplerate / samples;
			int lead_frames = (context->offset * fps) / 1000;

			for (int x = 0; x < lead_frames; x++) {
				switch_buffer_write(context->audio_buffer, buf, datalen);
			}
			context->offset = 0;
		}

		switch_buffer_write(context->audio_buffer, data, datalen);
	}

GCC_DIAG_OFF(deprecated-declarations)
	bytes = context->audio_st.frame->nb_samples * 2 * context->audio_st.st->codec->channels;
GCC_DIAG_ON(deprecated-declarations)


	//inuse = switch_buffer_inuse(context->audio_buffer);
	//switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "inuse: %d samples: %d bytes: %d\n", inuse, context->audio_st.frame->nb_samples, bytes);

	if (context->closed) {
		inuse = switch_buffer_inuse(context->audio_buffer);
		if (inuse < bytes) {
			char buf[SWITCH_RECOMMENDED_BUFFER_SIZE] = {0};
			switch_buffer_write(context->audio_buffer, buf, bytes - inuse);
		}
	}

	if (!context->audio_timer.interval) {
		switch_core_timer_init(&context->audio_timer, "soft", 1, handle->samplerate / 1000, context->pool);
	}

	while ((inuse = switch_buffer_inuse(context->audio_buffer)) >= bytes) {
		AVPacket pkt = { 0 };
		int got_packet = 0;
		int ret;

		av_init_packet(&pkt);

		if (context->audio_st.resample_ctx) { // need resample
			int out_samples = avresample_get_out_samples(context->audio_st.resample_ctx, context->audio_st.frame->nb_samples);
			av_frame_make_writable(context->audio_st.frame);
			av_frame_make_writable(context->audio_st.tmp_frame);
			switch_buffer_read(context->audio_buffer, context->audio_st.frame->data[0], bytes);
			/* convert to destination format */
			ret = avresample_convert(context->audio_st.resample_ctx,
									 context->audio_st.tmp_frame->data, 0, out_samples,
									 (uint8_t **)context->audio_st.frame->data, 0, context->audio_st.frame->nb_samples);

			if (ret < 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error while converting %d samples, error text: %s\n",
					context->audio_st.frame->nb_samples, get_error_text(ret));
				continue;
			}

			switch_core_timer_sync(&context->audio_timer);
			context->audio_st.tmp_frame->pts = context->audio_timer.samplecount;
			context->audio_st.next_pts = context->audio_st.tmp_frame->pts + context->audio_st.frame->nb_samples;

			//context->audio_st.tmp_frame->pts = context->audio_st.next_pts;
			//context->audio_st.next_pts += context->audio_st.frame->nb_samples;
GCC_DIAG_OFF(deprecated-declarations)
			ret = avcodec_encode_audio2(context->audio_st.st->codec, &pkt, context->audio_st.tmp_frame, &got_packet);
GCC_DIAG_ON(deprecated-declarations)
		} else {
			av_frame_make_writable(context->audio_st.frame);
			switch_buffer_read(context->audio_buffer, context->audio_st.frame->data[0], bytes);
			switch_core_timer_sync(&context->audio_timer);
			context->audio_st.frame->pts = context->audio_timer.samplecount;
			context->audio_st.next_pts = context->audio_st.frame->pts + context->audio_st.frame->nb_samples;
			//context->audio_st.frame->pts = context->audio_st.next_pts;
			//context->audio_st.next_pts  += context->audio_st.frame->nb_samples;

GCC_DIAG_OFF(deprecated-declarations)
			ret = avcodec_encode_audio2(context->audio_st.st->codec, &pkt, context->audio_st.frame, &got_packet);
GCC_DIAG_ON(deprecated-declarations)
		}

		if (ret < 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Error encoding audio frame: %d\n", ret);
			continue;
		}
		
		if (got_packet) {
			if (context->mutex) switch_mutex_lock(context->mutex);
GCC_DIAG_OFF(deprecated-declarations)
			ret = write_frame(context->fc, &context->audio_st.st->codec->time_base, context->audio_st.st, &pkt);
GCC_DIAG_ON(deprecated-declarations)
			if (context->mutex) switch_mutex_unlock(context->mutex);
			if (ret < 0) {
				context->errs++;
				if ((context->errs % 10) == 0) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error while writing audio frame: %s\n", get_error_text(ret));
				}
				//switch_goto_status(SWITCH_STATUS_FALSE, end);
			} else {
				context->errs = 0;
			}

			if (context->errs > 100) {
				status = SWITCH_STATUS_FALSE;
				break;
			}
		}
		if (data) {
			break;
		}
	}

	return status;
}

static switch_status_t av_file_command(switch_file_handle_t *handle, switch_file_command_t command)
{
	av_file_context_t *context = (av_file_context_t *)handle->private_info;
	uint32_t offset = 0;

	switch(command) {
	case SCFC_FLUSH_AUDIO:
		switch_mutex_lock(context->mutex);
		switch_buffer_zero(context->audio_buffer);
		switch_mutex_unlock(context->mutex);
		break;
	case SCFC_PAUSE_READ:
		if (context->read_paused) {
			context->read_paused = SWITCH_FALSE;
			context->video_st.next_pts = 0;
			context->video_start_time = 0;
		} else {
			context->read_paused = SWITCH_TRUE;
		}
		break;
	case SCFC_PAUSE_WRITE:
		context->vid_ready = 0;
		context->eh.record_timer_paused = switch_micro_time_now();
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s pause write\n", handle->file_path);
		break;
	case SCFC_RESUME_WRITE:
		if (context->eh.record_timer_paused) {
			context->eh.last_ts = 0;
			offset = (uint32_t)(switch_micro_time_now() - context->eh.record_timer_paused);
			context->video_timer.start += offset;
			switch_core_timer_sync(&context->video_timer);
			context->audio_timer.start += offset;
			switch_core_timer_sync(&context->audio_timer);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s resume write\n", handle->file_path);
			context->eh.record_timer_paused = 0;
		}
		break;
	default:
		break;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t av_file_close(switch_file_handle_t *handle)
{
	av_file_context_t *context = (av_file_context_t *)handle->private_info;
	switch_status_t status;

	context->closed = 1;
	context->eh.finalize = 1;

	if (context->eh.video_queue) {
		if (!switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) {
			flush_video_queue(context->eh.video_queue, 0);
			switch_queue_push(context->eh.video_queue, NULL);
			switch_queue_term(context->eh.video_queue);
		} else {
			switch_queue_push(context->eh.video_queue, NULL);
		}
	}
	

	if (context->eh.video_thread) {
		switch_thread_join(&status, context->eh.video_thread);
	}

	if (switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) {
		av_file_write(handle, NULL, NULL);
	}

	if (context->file_read_thread_running) {
		context->file_read_thread_running = 0;
	}

	if (context->file_read_thread) {
		switch_thread_join(&status, context->file_read_thread);
		context->file_read_thread = NULL;
	}

	if (context->eh.video_queue) {
		flush_video_queue(context->eh.video_queue, 0);
	}

	if (context->fc) {
		if (context->has_video && switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) {
			av_write_trailer(context->fc);
		}
		
		mod_avformat_destroy_output_context(context);
	}

	if (context->video_timer.interval) {
		switch_core_timer_destroy(&context->video_timer);
	}

	if (context->audio_timer.interval) {
		switch_core_timer_destroy(&context->audio_timer);
	}

	switch_img_free(&context->last_img);

	switch_buffer_destroy(&context->audio_buffer);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t av_file_seek(switch_file_handle_t *handle, unsigned int *cur_sample, int64_t samples, int whence)
{
	av_file_context_t *context = (av_file_context_t *)handle->private_info;

	if (whence == SEEK_SET) {
		handle->pos = handle->offset_pos = samples;
	}

	context->seek_ts = samples / handle->native_rate * AV_TIME_BASE;
	*cur_sample = context->seek_ts;

	context->closed = 0;

	if (!context->file_read_thread_running) {
		switch_threadattr_t *thd_attr = NULL;

		if (context->file_read_thread) {
			switch_status_t status;
			switch_thread_join(&status, context->file_read_thread);
			context->file_read_thread = NULL;
		}

		switch_threadattr_create(&thd_attr, handle->memory_pool);
		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
		switch_thread_create(&context->file_read_thread, thd_attr, file_read_thread_run, context, handle->memory_pool);
	}

	return SWITCH_STATUS_FALSE;
}

static switch_status_t av_file_read(switch_file_handle_t *handle, void *data, size_t *len)
{
	av_file_context_t *context = (av_file_context_t *)handle->private_info;
	int size;
	size_t need = *len * 2 * context->audio_st.channels;

	if (!context->has_audio && context->has_video && context->file_read_thread_running) {
		memset(data, 0, *len * handle->channels * 2);
		return SWITCH_STATUS_SUCCESS;
	}

	switch_mutex_lock(context->mutex);
	while (!context->file_read_thread_started) {
		switch_thread_cond_wait(context->cond, context->mutex);
	}
	switch_mutex_unlock(context->mutex);

	if (context->closed || (!context->file_read_thread_running && switch_buffer_inuse(context->audio_buffer) == 0)) {
		*len = 0;
		return SWITCH_STATUS_FALSE;
	}

	while (context->has_video && !context->vid_ready && !context->closed) {
		switch_yield(1000);
	}

	switch_mutex_lock(context->mutex);
	size = switch_buffer_read(context->audio_buffer, data, need);
	switch_mutex_unlock(context->mutex);

	if (size == 0) {
		size_t blank = (handle->samplerate / 20) * 2 * handle->real_channels;

		if (need > blank) {
			need = blank;
		}
		memset(data, 0, need);
		*len = need / 2 / handle->real_channels;
	} else {
		*len = size / context->audio_st.channels / 2;
	}

	handle->pos += *len;
	handle->sample_count += *len;

	return *len == 0 ? SWITCH_STATUS_FALSE : SWITCH_STATUS_SUCCESS;
}


#ifdef ALT_WAY
static switch_status_t av_file_read_video(switch_file_handle_t *handle, switch_frame_t *frame, switch_video_read_flag_t flags)
{
	void *pop;
	av_file_context_t *context = (av_file_context_t *)handle->private_info;
	switch_status_t status;


	if (!context->has_video || context->closed) return SWITCH_STATUS_FALSE;
	if ((flags & SVR_CHECK)) {
		return SWITCH_STATUS_BREAK;
	}

	if ((flags & SVR_FLUSH)) {
		flush_video_queue(context->eh.video_queue, 1);
	}

	if ((flags & SVR_BLOCK)) {
		status = switch_queue_pop(context->eh.video_queue, &pop);
	} else {
		status = switch_queue_trypop(context->eh.video_queue, &pop);
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		if (!pop) {
			return SWITCH_STATUS_FALSE;
		}

		context->vid_ready = 1;

		frame->img = (switch_image_t *) pop;

		if (frame->img) {
			if (frame->img && context->handle->mm.scale_w && context->handle->mm.scale_h) {
				if (frame->img->d_w != context->handle->mm.scale_w || frame->img->d_h != context->handle->mm.scale_h) {
					switch_img_fit(&frame->img, context->handle->mm.scale_w, context->handle->mm.scale_h, SWITCH_FIT_SIZE);
				}
			}
			context->vid_ready = 1;
		}		

		return SWITCH_STATUS_SUCCESS;
	}

	return (flags & SVR_FLUSH) ? SWITCH_STATUS_BREAK : status;
}
#else

static switch_status_t av_file_read_video(switch_file_handle_t *handle, switch_frame_t *frame, switch_video_read_flag_t flags)
{
	av_file_context_t *context = (av_file_context_t *)handle->private_info;
	void *pop;
	MediaStream *mst = &context->video_st;
	AVStream *st = mst->st;
	int ticks = 0;
	int64_t max_delta = 1 * AV_TIME_BASE; // 1 second
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	double fl_to = 0.02;
	int do_fl = 0;
	int smaller_ts = context->read_fps;

	if (!context->has_video) return SWITCH_STATUS_FALSE;

	if ((flags & SVR_CHECK)) {
		return SWITCH_STATUS_BREAK;
	}

	if (handle->mm.fps > 0 && handle->mm.fps < smaller_ts) {
		smaller_ts = handle->mm.fps;
	}

	fl_to = (1000 / smaller_ts) * 1000;
	//printf("WTF %d (%f)\n",switch_queue_size(context->eh.video_queue), fl_to);
	if (flags & SVR_FLUSH) {
		max_delta = fl_to;
		do_fl = 1;
	}

	if (!context->file_read_thread_running && switch_queue_size(context->eh.video_queue) == 0) {
		return SWITCH_STATUS_FALSE;
	}

	if (context->read_paused || context->seek_ts == -2) {
		int sanity = 10;

		if (context->seek_ts == -2) { // just seeked, try read a new img
		again1:
			status = switch_queue_trypop(context->eh.video_queue, &pop);
			if (pop && status == SWITCH_STATUS_SUCCESS) {
				context->seek_ts = -1;
				switch_img_free(&context->last_img);
				context->last_img = (switch_image_t *)pop;
				switch_img_copy(context->last_img, &frame->img);
				context->vid_ready = 1;
				return SWITCH_STATUS_SUCCESS;
			}

			if (context->last_img) { // repeat the last img
				switch_img_copy(context->last_img, &frame->img);
				context->vid_ready = 1;
				context->seek_ts = -1;
				return SWITCH_STATUS_SUCCESS;
			}

			if ((flags & SVR_BLOCK) && sanity-- > 0) {
				switch_yield(10000);
				goto again1;
			}

			return SWITCH_STATUS_BREAK;
		}

		if (context->last_img) { // repeat the last img
			if ((flags & SVR_BLOCK)) switch_yield(100000);
			switch_img_copy(context->last_img, &frame->img);
			context->vid_ready = 1;
			return SWITCH_STATUS_SUCCESS;
		}

		if ((flags & SVR_BLOCK)) {
			status = switch_queue_pop(context->eh.video_queue, &pop);
		} else {
			status = switch_queue_trypop(context->eh.video_queue, &pop);
		}

		if (pop && status == SWITCH_STATUS_SUCCESS) {
			context->last_img = (switch_image_t *)pop;
			switch_img_copy(context->last_img, &frame->img);
			context->vid_ready = 1;
			return SWITCH_STATUS_SUCCESS;
		}

		return SWITCH_STATUS_BREAK;
	}

#if 0
	if (context->last_img) {
		if (mst->next_pts && (switch_time_now() - mst->next_pts > max_delta)) {
			switch_img_free(&context->last_img); // too late
		} else if (mst->next_pts && (switch_time_now() - mst->next_pts > -10000)) {
			frame->img = context->last_img;
			context->last_img = NULL;
			context->vid_ready = 1;
			return SWITCH_STATUS_SUCCESS;
		}

		if (!(flags & SVR_BLOCK) && !do_fl) {
			if (!mst->next_pts) {
				frame->img = context->last_img;
				context->last_img = NULL;
				context->vid_ready = 1;
				return SWITCH_STATUS_SUCCESS;
			}
			return SWITCH_STATUS_BREAK;
		}
	}
#endif

GCC_DIAG_OFF(deprecated-declarations)
	if (st->codec->time_base.num) {
		ticks = st->parser ? st->parser->repeat_pict + 1 : st->codec->ticks_per_frame;
		// mst->next_pts += ((int64_t)AV_TIME_BASE * st->codec->time_base.num * ticks) / st->codec->time_base.den;
	}

	if (!context->video_start_time) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "start: %" SWITCH_INT64_T_FMT " ticks: %d ticks_per_frame: %d st num:%d st den:%d codec num:%d codec den:%d start: %" SWITCH_TIME_T_FMT ", duration:%" SWITCH_INT64_T_FMT " nb_frames:%" SWITCH_INT64_T_FMT " q2d:%f\n",
			context->video_start_time, ticks, st->codec->ticks_per_frame, st->time_base.num, st->time_base.den, st->codec->time_base.num, st->codec->time_base.den,
			st->start_time, st->duration, st->nb_frames, av_q2d(st->time_base));
	}
GCC_DIAG_ON(deprecated-declarations)

 again:

	if (context->last_img) {
		pop = (void *) context->last_img;
		context->last_img = NULL;
		status = SWITCH_STATUS_SUCCESS;
	} else {
		if ((flags & SVR_BLOCK)) {
			status = switch_queue_pop(context->eh.video_queue, &pop);
		} else {
			status = switch_queue_trypop(context->eh.video_queue, &pop);
		}
	}

	if (pop && status == SWITCH_STATUS_SUCCESS) {
		switch_image_t *img = (switch_image_t *)pop;
		int64_t pts;
		int64_t now = switch_time_now();

		pts = av_rescale_q(*((uint64_t *)img->user_priv), st->time_base, AV_TIME_BASE_Q);
		// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "pkt_pts: %lld pts: %lld queue size: %u\n", *((uint64_t *)img->user_priv), pts, switch_queue_size(context->eh.video_queue));
		handle->vpos = pts;

		if (!context->video_start_time) {
			context->video_start_time = now - pts;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "set start time: %" SWITCH_INT64_T_FMT " now: %" SWITCH_INT64_T_FMT " pts: %" SWITCH_INT64_T_FMT "\n", context->video_start_time, now, pts);
		}

		if (st->time_base.num == 0) {
			mst->next_pts = 0;
		} else {
			// int64_t last_pts = mst->next_pts;
			mst->next_pts = context->video_start_time + pts;
			// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "pts: %" SWITCH_INT64_T_FMT " last_pts: %" SWITCH_INT64_T_FMT " delta: %" SWITCH_INT64_T_FMT " frame_pts: %" SWITCH_INT64_T_FMT " nextpts: %" SWITCH_INT64_T_FMT ", num: %d, den:%d num:%d den:%d sleep: %" SWITCH_INT64_T_FMT "\n",
			// pts, last_pts, mst->next_pts - last_pts, *((uint64_t *)img->user_priv), mst->next_pts, st->time_base.num, st->time_base.den, st->codec->time_base.num, st->codec->time_base.den, mst->next_pts - now);
		}

		if (pts == 0 || context->video_start_time == 0) mst->next_pts = 0;

		if ((mst->next_pts && (now - mst->next_pts) > max_delta)) {
			//switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "picture is too late, off: %" SWITCH_INT64_T_FMT " max delta: %" SWITCH_INT64_T_FMT " queue size:%u fps:%u/%0.2f\n", (int64_t)(now - mst->next_pts), max_delta, switch_queue_size(context->eh.video_queue), context->read_fps, handle->mm.fps);
			switch_img_free(&img);
			//max_delta = AV_TIME_BASE;

			if (switch_queue_size(context->eh.video_queue) > 0) {
				// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "WTF again\n");
				goto again;
			} else if (!(flags & SVR_BLOCK) && !do_fl) {
				mst->next_pts = 0;
				context->video_start_time = 0;
				return SWITCH_STATUS_BREAK;
			}
		}

		if ((flags & SVR_BLOCK)) {
			while (switch_micro_time_now() - mst->next_pts < -10000) {
				// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "yield, delta=%" SWITCH_INT64_T_FMT "\n", switch_micro_time_now() - mst->next_pts);
				switch_yield(1000);
			}
			frame->img = img;
			do_fl = 0;
		} else {
			if (switch_micro_time_now() - mst->next_pts > -10000) {
				frame->img = img;
			} else {
				context->last_img = img;
				return SWITCH_STATUS_BREAK;
			}
		}

	} else {
		return SWITCH_STATUS_BREAK;
	}

	if (frame->img) {
		if (frame->img && context->handle->mm.scale_w && context->handle->mm.scale_h) {
			if (frame->img->d_w != context->handle->mm.scale_w || frame->img->d_h != context->handle->mm.scale_h) {
				switch_img_fit(&frame->img, context->handle->mm.scale_w, context->handle->mm.scale_h, SWITCH_FIT_SCALE);
			}
		}
		context->vid_ready = 1;
	}

	if ((flags & SVR_BLOCK)) {
		if (!frame->img) context->closed = 1;
		return frame->img ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
	} else {
		return frame->img ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_BREAK;
	}
}
#endif

static switch_status_t av_file_write_video(switch_file_handle_t *handle, switch_frame_t *frame)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	av_file_context_t *context = (av_file_context_t *)handle->private_info;

	if (!frame->img) {
		switch_goto_status(SWITCH_STATUS_FALSE, end);
	}

	if (!context->has_video) {
		context->video_st.width = frame->img->d_w;
		context->video_st.height = frame->img->d_h;
		context->video_st.next_pts = switch_time_now() / 1000;
		if (add_stream(&context->video_st, context->fc, &context->video_codec, context->fc->oformat->video_codec, &handle->mm) == SWITCH_STATUS_SUCCESS &&
			open_video(context->fc, context->video_codec, &context->video_st) == SWITCH_STATUS_SUCCESS) {

			char codec_str[256];
			int ret;

GCC_DIAG_OFF(deprecated-declarations)
			avcodec_string(codec_str, sizeof(codec_str), context->video_st.st->codec, 1);
GCC_DIAG_ON(deprecated-declarations)
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "use video codec implementation %s\n", codec_str);
			context->has_video = 1;

			// av_dump_format(context->fc, 0, "/tmp/av.mp4", 1);

			ret = avformat_write_header(context->fc, NULL);
			if (ret < 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error occurred when opening output file: %s\n", get_error_text(ret));
				switch_goto_status(SWITCH_STATUS_FALSE, end);
			}

		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error adding video stream\n");
			switch_goto_status(SWITCH_STATUS_FALSE, end);
		}
	}

	if (context->has_video) {
		switch_image_t *img = NULL;

		if (!context->eh.video_thread) {
			switch_threadattr_t *thd_attr = NULL;
			
			switch_mutex_init(&context->mutex, SWITCH_MUTEX_NESTED, handle->memory_pool);
			context->eh.mutex = context->mutex;
			context->eh.video_st = &context->video_st;
			context->eh.fc = context->fc;
			context->eh.mm = &handle->mm;
			switch_queue_create(&context->eh.video_queue, SWITCH_CORE_QUEUE_LEN, handle->memory_pool);
			switch_threadattr_create(&thd_attr, handle->memory_pool);
			//switch_threadattr_priority_set(thd_attr, SWITCH_PRI_REALTIME);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
			switch_core_timer_init(&context->video_timer, "soft", 1, 90, context->pool);
			context->eh.video_timer = &context->video_timer;
			context->audio_st.frame->pts = 0;
			context->audio_st.next_pts = 0;
			switch_thread_create(&context->eh.video_thread, thd_attr, video_thread_run, context, handle->memory_pool);
		}

		switch_img_copy(frame->img, &img);
		switch_queue_push(context->eh.video_queue, img);
		
		if (!context->vid_ready) {
			switch_mutex_lock(context->mutex);
			switch_buffer_zero(context->audio_buffer);
			switch_mutex_unlock(context->mutex);
			context->vid_ready = 1;
		}

	}

end:
	return status;
}


static switch_status_t av_file_set_string(switch_file_handle_t *handle, switch_audio_col_t col, const char *string)
{
	av_file_context_t *context = (av_file_context_t *)handle->private_info;

	if (context->fc) {
		const char *field = switch_parse_audio_col(col);

		if (field) {
			av_dict_set(&context->fc->metadata, field, string, 0);
			return SWITCH_STATUS_SUCCESS;
		}
	}

	return SWITCH_STATUS_FALSE;
}

static switch_status_t av_file_get_string(switch_file_handle_t *handle, switch_audio_col_t col, const char **string)
{
	av_file_context_t *context = (av_file_context_t *)handle->private_info;

	if (context->fc) {
		AVDictionaryEntry *tag = NULL;
		const char *field = switch_parse_audio_col(col);

		if (field && (tag = av_dict_get(context->fc->metadata, field, tag, 0))) {
			*string = tag->value;
			return SWITCH_STATUS_SUCCESS;
		}
	}

	return SWITCH_STATUS_FALSE;
}

static char *supported_formats[SWITCH_MAX_CODECS] = { 0 };

static const char modname[] = "mod_av";

SWITCH_MODULE_LOAD_FUNCTION(mod_avformat_load)
{
	switch_api_interface_t *api_interface;
	switch_application_interface_t *app_interface;
	switch_file_interface_t *file_interface;
	int i = 0;

	supported_formats[i++] = "av";
	supported_formats[i++] = "rtmp";
	supported_formats[i++] = "rtsp";
	supported_formats[i++] = "mp4";
	supported_formats[i++] = "m4a";
	supported_formats[i++] = "mov";
	supported_formats[i++] = "mkv";
	supported_formats[i++] = "webm";
	//supported_formats[i++] = "wav";

	file_interface = (switch_file_interface_t *)switch_loadable_module_create_interface(*module_interface, SWITCH_FILE_INTERFACE);
	file_interface->interface_name = modname;
	file_interface->extens = supported_formats;
	file_interface->file_open = av_file_open;
	file_interface->file_close = av_file_close;
	file_interface->file_truncate = av_file_truncate;
	file_interface->file_read = av_file_read;
	file_interface->file_write = av_file_write;
	file_interface->file_read_video = av_file_read_video;
	file_interface->file_write_video = av_file_write_video;
	file_interface->file_seek = av_file_seek;
	file_interface->file_set_string = av_file_set_string;
	file_interface->file_get_string = av_file_get_string;
	file_interface->file_command = av_file_command;

	SWITCH_ADD_API(api_interface, "av_format", "av information", av_format_api_function, "");

	SWITCH_ADD_APP(app_interface, "record_av", "record video using libavformat", "record video using libavformat", record_av_function, "<file>", SAF_NONE);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
