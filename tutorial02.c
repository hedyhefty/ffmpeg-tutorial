// tutorial02.c
// A pedagogical video player that will stream through every video frame as fast as it can.
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
// tutorial02 myvideofile.mpg
//
// to play the video stream on your screen.


#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL.h>
#include <SDL_thread.h>

// prevent sdl from overriding main function
#undef main

#include <stdio.h>

int main(int argc, char* argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: <media file>\n");
		return -1;
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
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

	int video_stream_index = -1;

	for (int i = 0; i < pFormatCtx->nb_streams; ++i) {
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream_index = i;
			break;
		}
	}

	if (video_stream_index == -1) {
		return -1;
	}

	AVCodecContext* pCodecCtx = avcodec_alloc_context3(NULL);
	if (!pCodecCtx) {
		return -1;
	}

	// init codec context by codec parameters
	avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[video_stream_index]->codecpar);

	AVCodec* pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if (!pCodec) {
		fprintf(stderr, "Unsupport codec");
		return -1;
	}

	int ret = avcodec_open2(pCodecCtx, pCodec, NULL);
	if (ret < 0) {
		return -1;
	}

	AVFrame* pFrame = av_frame_alloc();

	// create window for display
	SDL_Window* window = SDL_CreateWindow(
		"window",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		pCodecCtx->width,
		pCodecCtx->height,
		0
	);

	if (!window) {
		return -1;
	}

	// create sdl renderer
	SDL_Renderer* renderer = SDL_CreateRenderer(
		window,
		-1,
		0
	);

	if (!renderer) {
		return -1;
	}

	// create sdl texture
	SDL_Texture* texture = SDL_CreateTexture(
		renderer,
		SDL_PIXELFORMAT_YV12,
		SDL_TEXTUREACCESS_STREAMING,
		pCodecCtx->width,
		pCodecCtx->height
	);

	// init scale context
	struct SwsContext* swsCtx = sws_getContext(
		pCodecCtx->width,
		pCodecCtx->height,
		pCodecCtx->pix_fmt,
		pCodecCtx->width,
		pCodecCtx->height,
		AV_PIX_FMT_YUV420P,	  // target format
		SWS_BILINEAR,
		NULL,
		NULL,
		NULL
	);

	// init an AVFrame to hold display data
	int yPlaneSz = pCodecCtx->width * pCodecCtx->height;
	int uvPlaneSz = yPlaneSz / 4;

	Uint8* yPlane = (Uint8*)malloc(yPlaneSz);
	Uint8* uPlane = (Uint8*)malloc(uvPlaneSz);
	Uint8* vPlane = (Uint8*)malloc(uvPlaneSz);

	int uvPitch = pCodecCtx->width / 2;

	AVFrame pict = {
		.data[0] = yPlane,
		.data[1] = uPlane,
		.data[2] = vPlane,
		.linesize[0] = pCodecCtx->width,
		.linesize[1] = uvPitch,
		.linesize[2] = uvPitch
	};

	AVPacket* pkt = av_packet_alloc();

	ret = av_read_frame(pFormatCtx, pkt);
	while (ret >= 0) {
		if (pkt->stream_index == video_stream_index) {
			ret = avcodec_send_packet(pCodecCtx, pkt);
			if (ret < 0) {
				continue;
			}

			while (ret >= 0) {
				ret = avcodec_receive_frame(pCodecCtx, pFrame);

				if (ret < 0) {
					break;
				}

				// convert the frame to YUV420P for SDL display
				sws_scale(
					swsCtx,
					(Uint8 const* const*)pFrame->data,
					pFrame->linesize,
					0,
					pCodecCtx->height,
					pict.data,
					pict.linesize
				);

				// play
				SDL_UpdateYUVTexture(
					texture,
					NULL,
					yPlane,
					pCodecCtx->width,
					uPlane,
					uvPitch,
					vPlane,
					uvPitch
				);

				SDL_RenderClear(renderer);
				SDL_RenderCopy(renderer, texture, NULL, NULL);
				SDL_RenderPresent(renderer);

				av_frame_unref(pFrame);
			}
		}
		
		//av_packet_unref(pkt);
		ret = av_read_frame(pFormatCtx, pkt);
	}

	// clean up
	av_frame_free(&pFrame);
	avcodec_close(pCodecCtx);
	avformat_close_input(&pFormatCtx);

	return 0;
}