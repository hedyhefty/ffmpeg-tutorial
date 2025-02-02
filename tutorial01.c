// tutorial01.c
//
// This tutorial was written by Stephen Dranger (dranger@gmail.com).
// Update by st wang with FFmpeg 4 & SDL 2.
// Replace all attribute_deprecated functions.
//
// Code based on a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1

// A small sample program that shows how to use libavformat and libavcodec to
// read video from a file.
//
// Use the Makefile to build all examples.
//
// Run using
//
// tutorial01 myvideofile.mpg
//
// to write the first five frames from "myvideofile.mpg" to disk in PPM
// format.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif // _MSC_VER

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <stdio.h>

void SaveFrame(AVFrame* pFrame, int width, int height, int iFrame) {
	FILE* pFile;
	char szFilename[32];
	int  y;

	// Open file
	sprintf(szFilename, "frame%d.ppm", iFrame);
	pFile = fopen(szFilename, "wb");
	if (pFile == NULL)
		return;

	// Write header
	fprintf(pFile, "P6\n%d %d\n255\n", width, height);

	// Write pixel data
	for (y = 0; y < height; y++)
		fwrite(pFrame->data[0] + y * pFrame->linesize[0], 1, width * 3, pFile);

	// Close file
	fclose(pFile);
}

int main(int argc, char* argv[]) {
	AVFormatContext* pFormatCtx = NULL;
	int             i, videoStream;
	AVCodecContext* pCodecCtx = NULL;
	AVCodec* pCodec = NULL;
	AVFrame* pFrame = NULL;
	AVFrame* pFrameRGB = NULL;
	AVPacket        packet;
	int             frameFinished;
	int             numBytes;
	uint8_t* buffer = NULL;

	AVDictionary* optionsDict = NULL;
	struct SwsContext* sws_ctx = NULL;

	if (argc < 2) {
		printf("Please provide a movie file\n");
		return -1;
	}

	// No longer need register

	// Open video file
	if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
		return -1; // Couldn't open file

	  // Retrieve stream information
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
		return -1; // Couldn't find stream information

	  // Dump information about file onto standard error
	av_dump_format(pFormatCtx, 0, argv[1], 0);

	// Find the first video stream
	videoStream = -1;
	for (i = 0; i < pFormatCtx->nb_streams; i++)
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoStream = i;
			break;
		}
	if (videoStream == -1)
		return -1; // Didn't find a video stream

	pCodecCtx = avcodec_alloc_context3(NULL);
	if (!pCodecCtx) {
		fprintf(stderr, "Couldn't allocate codec");
		return -1;
	}

	// Get decode context from format context
	avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[i]->codecpar);

	// Find the decoder for the video stream
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if (pCodec == NULL) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1; // Codec not found
	}
	// Open codec
	if (avcodec_open2(pCodecCtx, pCodec, &optionsDict) < 0)
		return -1; // Could not open codec

	  // Allocate video frame
	pFrame = av_frame_alloc();

	// Allocate an AVFrame structure
	pFrameRGB = av_frame_alloc();
	if (pFrameRGB == NULL)
		return -1;

	// Determine required buffer size and allocate buffer
	numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 32);

	buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));

	sws_ctx = sws_getContext(
		pCodecCtx->width,
		pCodecCtx->height,
		pCodecCtx->pix_fmt,
		pCodecCtx->width,
		pCodecCtx->height,
		AV_PIX_FMT_RGB24,
		SWS_BILINEAR,
		NULL,
		NULL,
		NULL
	);

	av_image_fill_arrays(pFrameRGB, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 32);
	// Read frames and save first five frames to disk
	i = 0;
	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		// Is this a packet from the video stream?
		if (packet.stream_index == videoStream) {
			// Decode video frame
			frameFinished = avcodec_send_packet(pCodecCtx, &packet);
			if (frameFinished < 0) {
				fprintf(stderr, "Error sending a packet for decoding\n");
				return -1;
			}

			
			while (frameFinished >= 0) {
				frameFinished = avcodec_receive_frame(pCodecCtx, pFrame);
				// Did we get a video frame?
				if (frameFinished == AVERROR(EAGAIN) || frameFinished == AVERROR_EOF) {
					break;
				}
				else if (frameFinished < 0) {
					fprintf(stderr, "Error during decoding\n");
					return -1;
				}

				// Convert the image from its native format to RGB
				sws_scale(
					sws_ctx,
					(uint8_t const* const*)pFrame->data,
					pFrame->linesize,
					0,
					pCodecCtx->height,
					pFrameRGB->data,
					pFrameRGB->linesize
				);

				// Save the frame to disk
				if (++i <= 5)
					SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
			}
		}

		// Free the packet that was allocated by av_read_frame
		av_packet_unref(&packet);
	}

	// Free the RGB image
	av_free(buffer);
	av_free(pFrameRGB);

	// Free the YUV frame
	av_frame_free(&pFrame);

	// Close the codec
	avcodec_close(pCodecCtx);

	// Close the video file
	avformat_close_input(&pFormatCtx);

	return 0;
}
