// tutorial04.c
// A pedagogical video player that will stream through every video frame as fast as it can,
// and play audio (out of sync).
//
// This tutorial was written by Stephen Dranger (dranger@gmail.com).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
//
// Use the Makefile to build all the samples.
//
// Run using
// tutorial04 myvideofile.mpg
//
// to play the video stream on your screen.



#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libswresample/swresample.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#include <SDL.h>
#include <SDL_thread.h>

#undef main /* Prevents SDL from overriding main() */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

#define FRAME_QUEUE_SIZE 16

#define TRUE 1
#define FALSE 0

typedef struct PacketQueue {
	AVPacketList* first_pkt;
	AVPacketList* last_pkt;
	int nb_packets;
	int size;
	SDL_mutex* mtx;
	SDL_cond* cond;
}PacketQueue;

typedef struct FrameQueue {
	AVFrame* queue[FRAME_QUEUE_SIZE];

	size_t read_idx;
	size_t write_idx;
	size_t size;

	SDL_mutex* mtx;
	SDL_cond* cond;
}FrameQueue;

typedef struct VideoState {
	AVFormatContext* avfctx;

	// audio stuff
	int astream_idx;
	AVStream* astream;
	PacketQueue apkt_queue;
	Uint8 audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	size_t audio_buf_size;
	size_t audio_buf_idx;
	AVCodecContext* actx;

	// video stuff
	int vstream_idx;
	AVStream* vstream;
	PacketQueue vpkt_queue;
	FrameQueue frame_queue;
	AVCodecContext* vctx;
	AVFrame render_pict;
	SDL_Rect rect;

	SDL_Thread* demux_thread;
	SDL_Thread* video_thread;

	char filename[1024];
	int quit;

	struct SwsContext* sws_ctx;
	SwrContext* swr_ctx;
}VideoState;

// global variable
SDL_Window* window;
SDL_Renderer* renderer;
SDL_Texture* texture;

void packet_queue_init(PacketQueue* q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mtx = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue* q, AVPacket* pkt) {
	AVPacketList* insertpkt = av_malloc(sizeof(AVPacketList));
	if (!insertpkt && av_packet_make_refcounted(pkt) < 0) {
		return -1;
	}

	av_packet_move_ref(&insertpkt->pkt, pkt);
	insertpkt->next = NULL;

	SDL_LockMutex(q->mtx);

	if (!q->first_pkt) {
		q->first_pkt = insertpkt;
	}
	else {
		q->last_pkt->next = insertpkt;
	}

	q->last_pkt = insertpkt;

	q->size += insertpkt->pkt.size;
	++q->nb_packets;

	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mtx);
	return 0;
}

int packet_queue_get(PacketQueue* q, AVPacket* pkt) {
	SDL_LockMutex(q->mtx);
	AVPacketList* hold;
	for (;;) {
		hold = q->first_pkt;
		if (hold) {
			q->first_pkt = hold->next;

			// check if the queue is empty.
			if (!q->first_pkt) {
				q->last_pkt = NULL;
			}

			--q->nb_packets;
			q->size -= hold->pkt.size;
			*pkt = hold->pkt;

			av_free(hold);

			break;
		}
		else {
			SDL_CondWait(q->cond, q->mtx);
		}
	}
	SDL_UnlockMutex(q->mtx);
	return 0;
}

void frame_queue_init(FrameQueue* q) {
	for (size_t i = 0; i < FRAME_QUEUE_SIZE; ++i) {
		q->queue[i] = av_frame_alloc();
	}
}

int frame_queue_enqueue(FrameQueue* q, AVFrame* f) {
	SDL_LockMutex(q->mtx);

	for (;;) {
		if (q->size < FRAME_QUEUE_SIZE) {
			av_frame_move_ref(q->queue[q->write_idx], f);

			++q->write_idx;
			if (q->write_idx == FRAME_QUEUE_SIZE) {
				q->write_idx = 0;
			}

			++q->size;

			break;
		}
		else {
			SDL_CondWait(q->cond, q->mtx);
		}
	}
	
	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mtx);
	return 0;
}

int frame_queue_dequeue(FrameQueue* q, AVFrame* f) {
	SDL_LockMutex(q->mtx);

	for (;;) {
		if (q->size > 0) {
			av_frame_move_ref(f, q->queue[q->read_idx]);

			++q->read_idx;
			if (q->read_idx == FRAME_QUEUE_SIZE) {
				q->read_idx = 0;
			}

			--q->size;

			break;
		}
		else {
			SDL_CondWait(q->cond, q->mtx);
		}
	}

	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mtx);
	return 0;
}

int audio_decode_frame(VideoState* is) {
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();

	int len1 = 0;
	int data_size = 0;

	int ret = packet_queue_get(&is->apkt_queue, pkt);
	if (ret < 0) {
		av_packet_free(&pkt);
		av_frame_free(&frame);
		return -1;
	}

	AVCodecContext* avctx = is->actx;
	ret = avcodec_send_packet(avctx, pkt);
	Uint8* buf = is->audio_buf;

	while (ret >= 0) {
		ret = avcodec_receive_frame(avctx, frame);
		if (ret == AVERROR(EAGAIN)) {
			av_packet_free(&pkt);
			av_frame_free(&frame);
			return data_size;
		}
		else if (ret < 0) {
			break;
		}

		int dst_nb_samples = av_rescale_rnd(swr_get_delay(is->swr_ctx, frame->sample_rate) + frame->nb_samples
			, frame->sample_rate, frame->sample_rate, AV_ROUND_INF);

		int nb = swr_convert(is->swr_ctx, &buf, dst_nb_samples, (const Uint8**)frame->data, frame->nb_samples);
		if (nb < 0) {
			break;
		}

		data_size += frame->channels * nb * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
		buf += data_size;
	}

	av_packet_free(&pkt);
	av_frame_free(&frame);
	return -1;
}

void audio_callback(void* userdata, Uint8* stream, int len) {
	VideoState* is = userdata;
	int len1 = 0;
	int ret_size = 0;

	while (len > 0) {
		if (is->audio_buf_idx >= is->audio_buf_size) {
			ret_size = audio_decode_frame(is);
			if (ret_size < 0) {
				is->audio_buf_size = 1024;
				memset(is->audio_buf, 0, is->audio_buf_size);
			}
			else {
				is->audio_buf_size = ret_size;
			}

			is->audio_buf_idx = 0;
		}

		len1 = is->audio_buf_size - is->audio_buf_idx;
		if (len1 > len) {
			len1 = len;
		}

		memcpy(stream, (Uint8*)is->audio_buf + is->audio_buf_idx, len1);
		len -= len1;
		is->audio_buf_idx += len1;
		stream += len1;

		if (is->quit) {
			break;
		}
	}
}

Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaque) {
	SDL_Event m_event;
	m_event.type = FF_REFRESH_EVENT;
	m_event.user.data1 = opaque;
	SDL_PushEvent(&m_event);

	// stop timer
	return 0;
}

void schedule_refresh(VideoState* is, int delay) {
	SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState* is) {
	AVFrame* frame = av_frame_alloc();
	frame_queue_dequeue(&is->frame_queue, frame);
	AVFrame* pict = &is->render_pict;

	sws_scale(
		is->sws_ctx,
		(Uint8 const* const*)frame->data,
		frame->linesize,
		0,
		is->vctx->height,
		pict->data,
		pict->linesize
	);

	SDL_UpdateYUVTexture(
		texture,
		NULL,
		pict->data[0],
		pict->linesize[0],
		pict->data[1],
		pict->linesize[1],
		pict->data[2],
		pict->linesize[2]
	);

	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, &is->rect);
	SDL_RenderPresent(renderer);
}

void video_refresh_timer(void* userdata) {
	VideoState* is = userdata;

	if (is->vstream) {
		if (is->frame_queue.size == 0) {
			schedule_refresh(is, 1);
		}
		else {
			// Guess a delay like what we do in tutorial3
			static int init = FALSE;
			static double frame_rate;
			static Uint32 approx_delay;

			if (!init) {
				frame_rate = av_q2d(is->vstream[is->vstream_idx].avg_frame_rate);
				approx_delay = (Uint32)(1.0 / frame_rate * 1000);
				init = TRUE;
			}

			schedule_refresh(is, approx_delay);

			video_display(is);
		}
	}
	else {
		schedule_refresh(is, 100);
	}
}

int video_thread(void* arg) {
	VideoState* is = arg;
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	AVCodecContext* avctx = is->vctx;
	FrameQueue* f = &is->frame_queue;

	int ret = 0;

	for (;;) {
		ret = packet_queue_get(&is->vpkt_queue, pkt);
		if (ret < 0) {
			fprintf(stderr, "Could not get packet from video queue\n");
			exit(EXIT_FAILURE);
		}

		ret = avcodec_send_packet(avctx, pkt);
		if (ret < 0) {
			av_packet_unref(pkt);
			continue;
		}

		while (ret >= 0) {
			ret = avcodec_receive_frame(avctx, frame);
			if (ret == AVERROR(EAGAIN)) {
				break;
			}
			else if (ret < 0) {
				fprintf(stderr, "Unknown error occured in video thread\n");
				exit(EXIT_FAILURE);
			}
			
			// enqueue the frame
			ret = frame_queue_enqueue(f, frame);
			if (ret < 0) {
				fprintf(stderr, "Error occured in frame queue\n");
				exit(EXIT_FAILURE);
			}
		}

		av_packet_unref(pkt);
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
}

// init avframe to hold the frame to be render
void init_render_pict(VideoState* is) {
	AVCodecContext* avctx = is->vctx;
	int yPlaneSz = avctx->width * avctx->height;
	int uvPlaneSz = yPlaneSz / 4;

	Uint8* yPlane = (Uint8*)malloc(yPlaneSz);
	Uint8* uPlane = (Uint8*)malloc(uvPlaneSz);
	Uint8* vPlane = (Uint8*)malloc(uvPlaneSz);

	int uvPitch = avctx->width / 2;

	is->render_pict.data[0] = yPlane;
	is->render_pict.data[1] = uPlane;
	is->render_pict.data[2] = vPlane;
	is->render_pict.linesize[0] = avctx->width;
	is->render_pict.linesize[1] = uvPitch;
	is->render_pict.linesize[2] = uvPitch;
}

void init_render_rect(VideoState* is) {
	AVCodecParameters* codecpar = is->vstream->codecpar;
	AVRational sample_aspect_ratio = codecpar->sample_aspect_ratio;
	int width = codecpar->width;
	int height = codecpar->height;

	double aspect_ratio;

	if (sample_aspect_ratio.num == 0) {
		aspect_ratio = 0;
	}
	else {
		aspect_ratio = av_q2d(sample_aspect_ratio) * width / height;
	}
	if (aspect_ratio <= 0.0) {
		aspect_ratio = (double)width / (double)height;
	}

	int ww, wh;
	int w, h, x, y;
	SDL_GetWindowSize(window, &ww, &wh);

	h = wh;
	w = ((int)rint(h * aspect_ratio)) & -3;
	if (w > ww) {
		w = ww;
		h = ((int)rint(w / aspect_ratio)) & -3;
	}
	x = (ww - w) / 2;
	y = (wh - h) / 2;

	is->rect.x = x;
	is->rect.y = y;
	is->rect.w = w;
	is->rect.h = h;
}

int stream_component_open(VideoState* is, int stream_idx) {
	AVFormatContext* fmt_ctx = is->avfctx;
	
	AVCodecContext* avctx = avcodec_alloc_context3(NULL);
	if (!avctx) {
		fprintf(stderr, "Counld not open codec context./n");
		return -1;
	}

	avcodec_parameters_to_context(avctx, fmt_ctx->streams[stream_idx]->codecpar);

	AVCodec* avc = avcodec_find_decoder(avctx->codec_id);
	if (!avc) {
		fprintf(stderr, "Unsupport codec\n");
		return -1;
	}

	int ret = avcodec_open2(avctx, avc, NULL);
	if (ret < 0) {
		fprintf(stderr, "Could not open codec\n");
		return -1;
	}

	SDL_AudioSpec wanted_spec;
	SDL_AudioSpec spec;
	int index;
	int channels;
	Uint64 channel_layout;
	Uint64 dst_layout;

	switch (avctx->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		wanted_spec.freq = avctx->sample_rate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = avctx->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = is;

		ret = SDL_OpenAudio(&wanted_spec, &spec);
		if (ret < 0) {
			return -1;
		}

		is->astream_idx = stream_idx;
		is->astream = fmt_ctx->streams[stream_idx];
		is->actx = avctx;
		is->audio_buf_size = 0;
		is->audio_buf_idx = 0;
		packet_queue_init(&is->apkt_queue);

		index = av_get_channel_layout_channel_index(av_get_default_channel_layout(4), AV_CH_FRONT_CENTER);
		channels = is->astream->codecpar->channels;
		channel_layout = is->astream->codecpar->channel_layout;
		
		if (channels > 0 && channel_layout == 0) {
			channel_layout = av_get_default_channel_layout(channels);
		}
		else if (channels == 0 && channel_layout > 0) {
			channels = av_get_channel_layout_nb_channels(channel_layout);
		}

		dst_layout = av_get_default_channel_layout(channels);
		is->swr_ctx = swr_alloc();
		swr_alloc_set_opts(
			is->swr_ctx,
			dst_layout,
			AV_SAMPLE_FMT_S16,
			avctx->sample_rate,
			channel_layout,
			(enum AVSampleFormat)is->astream->codecpar->format,
			avctx->sample_rate,
			0,
			NULL
		);

		if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
			return -1;
		}

		SDL_PauseAudio(0);

		break;

	case AVMEDIA_TYPE_VIDEO:
		is->vstream_idx = stream_idx;
		is->vstream = fmt_ctx->streams[stream_idx];
		is->vctx = avctx;
		packet_queue_init(&is->vpkt_queue);
		frame_queue_init(&is->frame_queue);
		is->video_thread = SDL_CreateThread(video_thread, "vt", is);

		is->sws_ctx = sws_getContext(
			is->vctx->width,
			is->vctx->height,
			is->vctx->pix_fmt,
			is->vctx->width,
			is->vctx->height,
			AV_PIX_FMT_YUV420P,
			SWS_BILINEAR,
			NULL,
			NULL,
			NULL
		);

		init_render_pict(is);
		init_render_rect(is);

		break;

	default:
		break;
	}

	return 0;
}

int demux_thread(void* arg) {
	VideoState* is = arg;

	int ret = avformat_open_input(&is->avfctx, is->filename, NULL, NULL);
	if (ret < 0) {
		fprintf(stderr, "Could not open file\n");
		exit(EXIT_FAILURE);
	}

	ret = avformat_find_stream_info(is->avfctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "Could not find stream\n");
		exit(EXIT_FAILURE);
	}

	av_dump_format(is->avfctx, 0, is->filename, 0);
	
	AVFormatContext* fmt_ctx = is->avfctx;
	int audio_idx = -1;
	int video_idx = -1;

	for (size_t i = 0; i < is->avfctx->nb_streams; ++i) {
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_idx < 0) {
			audio_idx = i;
			ret = stream_component_open(is, i);
		}

		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_idx < 0) {
			video_idx = i;
			ret = stream_component_open(is, i);
		}
	}

	if (is->vstream_idx < 0 || is->astream_idx < 0 || ret < 0) {
		fprintf(stderr, "Could not find stream.");
		exit(EXIT_FAILURE);
	}

	AVPacket* pkt = av_packet_alloc();

	// demux loop
	for (;;) {
		if (is->quit) {
			break;
		}

		if (is->apkt_queue.size > MAX_AUDIOQ_SIZE || is->vpkt_queue.size > MAX_VIDEOQ_SIZE) {
			SDL_Delay(10);
			continue;
		}

		ret = av_read_frame(fmt_ctx, pkt);
		if (ret < 0) {
			fprintf(stderr, "Packet read error.\n");
			exit(EXIT_FAILURE);
		}

		if (pkt->stream_index == is->vstream_idx) {
			packet_queue_put(&is->vpkt_queue, pkt);
		}
		else if (pkt->stream_index == is->astream_idx) {
			packet_queue_put(&is->apkt_queue, pkt);
		}
		else {
			av_packet_unref(pkt);
		}
	}
}

void clean_up(VideoState* is) {
	avcodec_close(is->vctx);
	avcodec_close(is->actx);
	avformat_close_input(&is->avfctx);
	av_free(is);
}

int main(int argc, char* argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: <media file>\n");
		return -1;
	}
	
	VideoState* is = av_mallocz(sizeof(VideoState));

	av_strlcpy(is->filename, argv[1], 1024);

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Counld not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}

	SDL_Window* window = SDL_CreateWindow(
		"window",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		1280,
		720,
		0
	);

	is->frame_queue.mtx = SDL_CreateMutex();
	is->frame_queue.cond = SDL_CreateCond();

	schedule_refresh(is, 40);
	is->demux_thread = SDL_CreateThread(demux_thread, "dt", is);
	if (!is->demux_thread) {
		return -1;
	}
	
	SDL_Event m_event;
	for (;;) {
		SDL_WaitEvent(&m_event);
		switch (m_event.type)
		{
		case FF_QUIT_EVENT:
		case SDL_QUIT:
			is->quit = 1;
			break;
		default:
			break;
		}

		if (is->quit == 1) {
			break;
		}
	}
	// exit sdl
	SDL_CondSignal(is->apkt_queue.cond);
	SDL_CondSignal(is->vpkt_queue.cond);
	SDL_CondSignal(is->frame_queue.cond);
	SDL_CloseAudio();
	SDL_Quit();

	// clean up is
	clean_up(is);

	return 0;
}
