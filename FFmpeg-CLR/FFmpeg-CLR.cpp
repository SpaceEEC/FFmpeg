#include "FFmpeg-CLR.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <winerror.h>

#include <algorithm>

#pragma unmanaged
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
}

void logging(const char *fmt, ...)
{
	va_list args;
	fprintf(stderr, "LOG: ");
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
}
#pragma managed

static inline bool isStreamMp3(const AVStream* val) { return val->codecpar->codec_id == AV_CODEC_ID_MP3; }

static int get_format_from_sample_fmt(const char** fmt, enum AVSampleFormat sample_fmt)
{
	struct sample_fmt_entry {
		enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
	} sample_fmt_entries[] = {
		{ AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
		{ AV_SAMPLE_FMT_S16, "s16be", "s16le" },
		{ AV_SAMPLE_FMT_S32, "s32be", "s32le" },
		{ AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
		{ AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
	};
	*fmt = NULL;

	for (int i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); ++i)
	{
		struct sample_fmt_entry* entry = &sample_fmt_entries[i];
		if (sample_fmt == entry->sample_fmt)
		{
			*fmt = AV_NE(entry->fmt_be, entry->fmt_le);

			return 0;
		}
	}

	logging("ERROR: sample format %s is not supported as output format", av_get_sample_fmt_name(sample_fmt));

	return -1;
}

namespace FFmpeg
{
	FFmpeg::FFmpeg(System::String^ fileIn, System::String^ fileOut)
	{
		this->m_Storage = new Storage();
		this->m_FileIn = static_cast<const char*>(System::Runtime::InteropServices::Marshal::StringToHGlobalAnsi(fileIn).ToPointer());
		this->m_FileOut = static_cast<const char*>(System::Runtime::InteropServices::Marshal::StringToHGlobalAnsi(fileOut).ToPointer());
	}
	FFmpeg::~FFmpeg() { this->!FFmpeg(); }
	FFmpeg::!FFmpeg()
	{
		delete this->m_Storage;
		System::Runtime::InteropServices::Marshal::FreeHGlobal(static_cast<System::IntPtr>(const_cast<char*>(this->m_FileIn)));
		System::Runtime::InteropServices::Marshal::FreeHGlobal(static_cast<System::IntPtr>(const_cast<char*>(this->m_FileOut)));
	}

	void FFmpeg::DoStuff()
	{
		HRESULT hr = S_OK;
		if (FAILED(hr = this->Init_()))
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
			av_strerror(hr, errbuf, AV_ERROR_MAX_STRING_SIZE);
			logging("Failure: %s", errbuf);
		}

		if (SUCCEEDED(hr) && FAILED(hr = this->DoStuff_()))
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
			av_strerror(hr, errbuf, AV_ERROR_MAX_STRING_SIZE);
			logging("Failure: %s", errbuf);
		}
	}

	inline HRESULT FFmpeg::Init_()
	{
		AVFormatContext* pFormatContext = this->m_Storage->m_InputFormatContext = avformat_alloc_context();

		if (pFormatContext == nullptr) return E_OUTOFMEMORY;

		HRESULT hr = S_OK;
		// Read the file headers
		if (FAILED(hr = avformat_open_input(&pFormatContext, this->m_FileIn, NULL, NULL))) return hr;
		logging("Found format in input: %s", (pFormatContext)->iformat->long_name);

		// Find the stream info(s)
		if (FAILED(hr = avformat_find_stream_info(pFormatContext, NULL))) return hr;

		av_dump_format(pFormatContext, 0, this->m_FileIn, 0);

		this->m_StreamIndex = av_find_best_stream(pFormatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &this->m_Storage->m_InputCodec, 0);
		AVCodec* pCodec = this->m_Storage->m_InputCodec;

		if (this->m_StreamIndex == AVERROR_STREAM_NOT_FOUND || pCodec == nullptr) return E_FAIL;

		AVCodecContext* pCodecContext = this->m_Storage->m_InputCodecContext = avcodec_alloc_context3(pCodec);
		if (pCodecContext == nullptr)
		{
			avformat_close_input(&pFormatContext);

			return E_OUTOFMEMORY;
		}

		if (FAILED(hr = avcodec_parameters_to_context(pCodecContext, pFormatContext->streams[this->m_StreamIndex]->codecpar)))
		{
			avformat_close_input(&pFormatContext);
			avcodec_free_context(&pCodecContext);

			return hr;
		}

		if (FAILED(hr = avcodec_open2(pCodecContext, pCodec, NULL)))
		{
			this->m_Storage->m_InputCodecContext = nullptr;

			return hr;
		}

		logging("Codec name: %s", pCodecContext->codec_descriptor->long_name);

		return hr;
	}

	
	inline HRESULT FFmpeg::DoStuff_()
	{
		AVFrame* pFrame = av_frame_alloc();
		if (pFrame == nullptr) return E_OUTOFMEMORY;

		AVPacket* pPacket = av_packet_alloc();
		if (pPacket == nullptr)
		{
			av_frame_free(&pFrame);

			return E_OUTOFMEMORY;
		}

		std::ofstream file(
			this->m_FileOut,
			std::ios::out | std::ios::binary
		);
		
		HRESULT hr = S_OK;
		while (SUCCEEDED(av_read_frame(this->m_Storage->m_InputFormatContext, pPacket)))
		{
			if (pPacket->stream_index == this->m_StreamIndex)
				this->DecodePacket_(&file, pPacket, pFrame);
			av_packet_unref(pPacket);
		}

		enum AVSampleFormat sfmt = this->m_Storage->m_InputCodecContext->sample_fmt;
		int channels = this->m_Storage->m_InputCodecContext->channels;
		const char* fmt;

		if (av_sample_fmt_is_planar(sfmt))
		{
			const char* packed = av_get_sample_fmt_name(sfmt);
			logging("Warning: the sample format the decoder produced is planar (%s). This example will output the first channel only.", packed ? packed : "?");
			sfmt = av_get_packed_sample_fmt(sfmt);
			channels = 1;
		}

		if (FAILED(get_format_from_sample_fmt(&fmt, sfmt)))
			return E_FAIL;

		logging("Play the output audio file with the command:\nffplay -f %s -ac %d -ar %d %s", fmt, channels, this->m_Storage->m_InputCodecContext->sample_rate, this->m_FileOut);

		file.close();
		av_packet_free(&pPacket);
		av_frame_free(&pFrame);

		return hr;
	}

	inline void FFmpeg::DecodePacket_(std::ofstream* ofstream, AVPacket* pPacket, AVFrame* pFrame)
	{

		if (FAILED(avcodec_send_packet(this->m_Storage->m_InputCodecContext, pPacket)))
			return;

		do
		{
			int hr = avcodec_receive_frame(this->m_Storage->m_InputCodecContext, pFrame);
			// we are at the end
			if (hr == AVERROR(EAGAIN) || hr == AVERROR_EOF) return;
			if (FAILED(hr)) return;

			size_t data_size = av_get_bytes_per_sample(this->m_Storage->m_InputCodecContext->sample_fmt);
			if (data_size == 0) return;

			int samples = pFrame->nb_samples;
			// aac files are weird, probably just my mistake somewhere
			if (this->m_Storage->m_InputCodecContext->channels != 1 && pFrame->data[1] == NULL)
				samples *= this->m_Storage->m_InputCodecContext->channels;

			for (int i = 0; i < samples; ++i)
			{
				for (int ch = 0; ch < this->m_Storage->m_InputCodecContext->channels; ++ch)
				{
					if (pFrame->data[ch] == NULL) continue;
					ofstream->write(reinterpret_cast<const char*>(pFrame->data[ch]) + data_size * i, data_size);
				}
			}
		} while (true);
	}
}