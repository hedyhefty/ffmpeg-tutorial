// tutorial03.c
// A pedagogical video player that will stream through every video frame as fast as it can
// and play audio (out of sync).
//
// This tutorial was written by Stephen Dranger (dranger@gmail.com).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
//
// Use the Makefile to build all examples.
//
// Run using
// tutorial03 myvideofile.mpg
//
// to play the stream on your screen.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#include <SDL.h>
#include <SDL_thread.h>

// prevent sdl from overriding main function
#undef main

#include <stdio.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue {
	AVPacketList* first_pkt;
	AVPacketList* last_pkt;
	int nb_packets;
	int size;
	SDL_mutex* mtx;
	SDL_cond* cond;
}PacketQueue;

// global var
int quit = 0;
PacketQueue audioq;

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

// func use to initialize swrcontext, which is used to rescale the planar audio to non-planar
// audio, for SDL2.
int init_swrctx(SwrContext* swrCtx, AVCodecContext* avctx, AVFrame* frame) {
	
	int index = av_get_channel_layout_channel_index(av_get_default_channel_layout(4), AV_CH_FRONT_CENTER);
	int channels = avctx->channels;
	Uint64 channel_layout = avctx->channel_layout;

	if (channels > 0 && channel_layout == 0) {
		channel_layout = av_get_default_channel_layout(channels);
	}
	else if (channels == 0 && channel_layout > 0) {
		channels = av_get_channel_layout_nb_channels(channel_layout);
	}

	Uint64 dst_layout = av_get_default_channel_layout(channels);

	swr_alloc_set_opts(
		swrCtx,
		dst_layout,
		AV_SAMPLE_FMT_S16,
		avctx->sample_rate,
		channel_layout,
		frame->format,
		avctx->sample_rate,
		0,
		NULL
	);

	if (!swrCtx || swr_init(swrCtx) < 0) {
		return -1;
	}

	return 0;
}

// delete the third parameter "buf_size" as it was useless.
// we cover old data every time this function is called so we don't
// need to know the buffer size(or current index).
// return the size(byte) of the audio_buf(contain the decoded frame). 
// return -1 while error.
int audio_decode_frame(AVCodecContext* avctx, uint8_t* audio_buf) {
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();

	int len1 = 0;
	int data_size = 0;

	// data in frame have 2 plane but SDL2 can only read data in single plane,
	// so we have to scale the data to one plane by swrcontext.
	static SwrContext* swrCtx;
	
	static int swr_is_init = 0;

	int ret = packet_queue_get(&audioq, pkt);
	if (ret < 0) {
		av_packet_free(&pkt);
		av_frame_free(&frame);
		return -1;
	}

	ret = avcodec_send_packet(avctx, pkt);

	int index = 0;

	if (ret >= 0) {
		ret = avcodec_receive_frame(avctx, frame);
		if (ret < 0) {
			av_packet_free(&pkt);
			av_frame_free(&frame);
			return -1;
		}

		// init swrcontext in the first call.
		if (!swrCtx || !swr_is_initialized(swrCtx)) {
			swrCtx = swr_alloc();
			ret = init_swrctx(swrCtx, avctx, frame);
			if (ret < 0) {
				av_packet_free(&pkt);
				av_frame_free(&frame);
				return -1;
			}
			swr_is_init = 1;
		}

		int dst_nb_samples = av_rescale_rnd(swr_get_delay(swrCtx, frame->sample_rate) + frame->nb_samples
			, frame->sample_rate, frame->sample_rate, AV_ROUND_INF);

		int nb = swr_convert(swrCtx, &audio_buf, dst_nb_samples, (const Uint8**)frame->data, frame->nb_samples);
		if (nb < 0) {
			av_packet_free(&pkt);
			av_frame_free(&frame);
			return -1;
		}

		data_size += frame->channels * nb * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
		index += data_size;

		av_packet_free(&pkt);
		av_frame_free(&frame);
		return data_size;
	}

	av_packet_free(&pkt);
	av_frame_free(frame);
	return -1;
}

void audio_callback(void* userdata, Uint8* stream, int len) {
	AVCodecContext* avctx = userdata;
	int len1 = 0;
	int ret_size = 0;

	static Uint8 audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	while (len > 0) {
		// decoded data all read, need new data.
		// infact, as len1 <= audio_buf_size - audio_buf_index,
		// means audio_buf_index + len1 <= audio_buf_size,
		// so audio_buf_index cannot bigger than audio_buf_size
		// but at most equal.
		if (audio_buf_index >= audio_buf_size) {
			ret_size = audio_decode_frame(avctx, audio_buf);
			if (ret_size < 0) {
				audio_buf_size = 1024;
				memset(audio_buf, 0, audio_buf_size);
			}
			else {
				audio_buf_size = ret_size;
			}

			// as all old data read, so the new decoded data
			// cover the old data so index start by zero.
			audio_buf_index = 0;
		}

		// len1 is the data length to be copy.
		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len) {
			len1 = len;
		}

		// if the audio_buf is not empty but no sound output,
		// you should check if you have to specify a driver
		// for SDL2. In my case(win10), add environment variable
		// SDL_AUDIODRIVER and set the value as directsound.
		memcpy(stream, (Uint8*)audio_buf + audio_buf_index, len1);
		len -= len1;
		audio_buf_index += len1;
		stream += len1;

		if (quit) {
			break;
		}
	}
}

int main(int argc, char* argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: <media file>\n");
		return -1;
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Counld not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}

	AVFormatContext* pFormatCtx = NULL;

	if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) {
		return -1;
	}

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
		return -1;
	}

	av_dump_format(pFormatCtx, 0, argv[1], 0);

	int vst_idx = -1;
	int ast_idx = -1;

	for (int i = 0; i < pFormatCtx->nb_streams; ++i) {
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ast_idx < 0) {
			ast_idx = i;
		}

		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vst_idx < 0) {
			vst_idx = i;
		}
	}

	if (vst_idx < 0 || ast_idx < 0) {
		fprintf(stderr, "Could not find stream\n");
		return -1;
	}

	AVCodecContext* aCodecCtx = avcodec_alloc_context3(NULL);
	if (!aCodecCtx) {
		return -1;
	}

	avcodec_parameters_to_context(aCodecCtx, pFormatCtx->streams[ast_idx]->codecpar);

	AVCodec* aCodec = avcodec_find_decoder(aCodecCtx->codec_id);
	if (!aCodec) {
		fprintf(stderr, "Unsupport codec\n");
		return -1;
	}

	SDL_AudioSpec spec;
	SDL_AudioSpec wanted_spec;

	wanted_spec.freq = aCodecCtx->sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = aCodecCtx->channels;
	wanted_spec.silence = 0;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = aCodecCtx;

	if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
		fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
		return -1;
	}

	if (avcodec_open2(aCodecCtx, aCodec, 0) < 0) {
		return -1;
	}

	packet_queue_init(&audioq);

	// start audio
	SDL_PauseAudio(0);

	AVCodecContext* vCodecCtx = avcodec_alloc_context3(NULL);
	if (!vCodecCtx) {
		return -1;
	}

	avcodec_parameters_to_context(vCodecCtx, pFormatCtx->streams[vst_idx]->codecpar);

	AVCodec* vCodec = avcodec_find_decoder(vCodecCtx->codec_id);
	if (!vCodec) {
		fprintf(stderr, "Unsupport codec\n");
		return -1;
	}

	if (avcodec_open2(vCodecCtx, vCodec, NULL) < 0) {
		return -1;
	}

	AVFrame* pFrame = av_frame_alloc();

	SDL_Window* window = SDL_CreateWindow(
		"window",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		vCodecCtx->width,
		vCodecCtx->height,
		0
	);

	if (!window) {
		return -1;
	}

	SDL_Renderer* renderer = SDL_CreateRenderer(
		window,
		-1,
		0
	);

	if (!renderer) {
		return -1;
	}

	SDL_Texture* texture = SDL_CreateTexture(
		renderer,
		SDL_PIXELFORMAT_YV12,
		SDL_TEXTUREACCESS_STREAMING,
		vCodecCtx->width,
		vCodecCtx->height
	);

	struct SwsContext* swsCtx = sws_getContext(
		vCodecCtx->width,
		vCodecCtx->height,
		vCodecCtx->pix_fmt,
		vCodecCtx->width,
		vCodecCtx->height,
		AV_PIX_FMT_YUV420P,
		SWS_BILINEAR,
		NULL,
		NULL,
		NULL
	);

	int yPlaneSz = vCodecCtx->width * vCodecCtx->height;
	int uvPlaneSz = yPlaneSz / 4;

	Uint8* yPlane = (Uint8*)malloc(yPlaneSz);
	Uint8* uPlane = (Uint8*)malloc(uvPlaneSz);
	Uint8* vPlane = (Uint8*)malloc(uvPlaneSz);

	int uvPitch = vCodecCtx->width / 2;

	AVFrame pict = {
		.data[0] = yPlane,
		.data[1] = uPlane,
		.data[2] = vPlane,
		.linesize[0] = vCodecCtx->width,
		.linesize[1] = uvPitch,
		.linesize[2] = uvPitch
	};

	double frame_rate = av_q2d(pFormatCtx->streams[vst_idx]->avg_frame_rate);
	double approx_delay = 1.0 / frame_rate * 1000;
	printf("approx delay: %f", approx_delay);

	AVPacket* pkt = av_packet_alloc();
	SDL_Event sdl_event;

	int ret = av_read_frame(pFormatCtx, pkt);
	while (ret >= 0) {
		if (pkt->stream_index == vst_idx) {
			ret = avcodec_send_packet(vCodecCtx, pkt);
			if (ret < 0) {
				av_packet_unref(pkt);
				ret = av_read_frame(pFormatCtx, pkt);
				continue;
			}

			while (ret >= 0) {
				ret = avcodec_receive_frame(vCodecCtx, pFrame);

				if (ret < 0) {
					break;
				}

				sws_scale(
					swsCtx,
					(Uint8 const* const*)pFrame->data,
					pFrame->linesize,
					0,
					vCodecCtx->height,
					pict.data,
					pict.linesize
				);

				SDL_UpdateYUVTexture(
					texture,
					NULL,
					yPlane,
					vCodecCtx->width,
					uPlane,
					uvPitch,
					vPlane,
					uvPitch
				);

				SDL_RenderClear(renderer);
				SDL_RenderCopy(renderer, texture, NULL, NULL);
				SDL_RenderPresent(renderer);

				// or the video will end immediately and you can hardly heard the sound.
				SDL_Delay(approx_delay);

				av_packet_unref(pkt);
				av_frame_unref(pFrame);
			}
		}
		else if (pkt->stream_index == ast_idx) {
			packet_queue_put(&audioq, pkt);
		}

		SDL_PollEvent(&sdl_event);
		switch (sdl_event.type)
		{
		case SDL_QUIT:
			quit = 1;
		default:
			break;
		}
		//av_packet_unref(pkt);
		if (quit) {
			break;
		}
		ret = av_read_frame(pFormatCtx, pkt);
	}

	// all packets sent
	quit = 1;
	SDL_CondSignal(audioq.cond);
	SDL_CloseAudio(0);
	//SDL_Quit();

	// clean up
	av_frame_free(&pFrame);
	av_packet_free(&pkt);
	avcodec_close(vCodecCtx);
	avcodec_close(aCodecCtx);
	avformat_close_input(&pFormatCtx);

	return 0;
}