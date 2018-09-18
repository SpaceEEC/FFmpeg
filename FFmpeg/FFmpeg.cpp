#include "FFmpeg.h"

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
	static FFmpeg::FFmpeg()
	{
		av_register_all();
		avcodec_register_all();
	}

	FFmpeg::FFmpeg(System::String^ fileIn, System::String^ fileOut)
	{
		this->m_FileIn = reinterpret_cast<char*>(System::Runtime::InteropServices::Marshal::StringToHGlobalAnsi(fileIn).ToPointer());
		this->m_FileOut = reinterpret_cast<char*>(System::Runtime::InteropServices::Marshal::StringToHGlobalAnsi(fileOut).ToPointer());
	}
	FFmpeg::~FFmpeg() { this->!FFmpeg(); }
	FFmpeg::!FFmpeg()
	{
		System::Runtime::InteropServices::Marshal::FreeHGlobal(static_cast<System::IntPtr>(this->m_FileIn));
		System::Runtime::InteropServices::Marshal::FreeHGlobal(static_cast<System::IntPtr>(this->m_FileOut));
	}

	inline HRESULT FFmpeg::Init_(AVFormatContext** ppFormatContext, AVCodec** ppCodec, AVCodecContext** ppCodecContext)
	{
		HRESULT hr = S_OK;

		*ppFormatContext = avformat_alloc_context();

		if (*ppFormatContext == nullptr) return E_OUTOFMEMORY;

		// Read the file headers
		if (FAILED(hr = avformat_open_input(&*ppFormatContext, this->m_FileIn, NULL, NULL))) return hr;
		logging("Found format in input: %s", (*ppFormatContext)->iformat->long_name);

		// Find the stream info(s)
		if (FAILED(hr = avformat_find_stream_info(*ppFormatContext, NULL))) return hr;

		av_dump_format(*ppFormatContext, 0, this->m_FileIn, 0);

		*ppCodec = nullptr;

		// Looking for mp3, if there is one just take it.
		this->m_StreamIndex = static_cast<int>(std::find_if(
			(*ppFormatContext)->streams,
			(*ppFormatContext)->streams + (*ppFormatContext)->nb_streams,
			isStreamMp3
		) - (*ppFormatContext)->streams);

		// Get the decoder
		if (this->m_StreamIndex != (*ppFormatContext)->nb_streams)
			*ppCodec = avcodec_find_decoder(AV_CODEC_ID_MP3);

		// No suitable stream and encoder found, try to fallback to best otherwise existing ones
		if (*ppCodec == nullptr)
			this->m_StreamIndex = av_find_best_stream(*ppFormatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &*ppCodec, 0);

		if (this->m_StreamIndex == AVERROR_STREAM_NOT_FOUND || ppCodec == nullptr) return E_FAIL;

		*ppCodecContext = avcodec_alloc_context3(*ppCodec);
		if (*ppCodecContext == nullptr)
		{
			avformat_close_input(&*ppFormatContext);

			return E_OUTOFMEMORY;
		}

		if (FAILED(hr = avcodec_parameters_to_context(*ppCodecContext, (*ppFormatContext)->streams[this->m_StreamIndex]->codecpar)))
		{
			avformat_close_input(&*ppFormatContext);
			avcodec_free_context(&*ppCodecContext);

			return hr;
		}

		
		if (FAILED(hr = avcodec_open2(*ppCodecContext, *ppCodec, NULL)))
		{
			*ppCodecContext = nullptr;

			return hr;
		}

		logging("Codec name: %s", (*ppCodecContext)->codec_descriptor->long_name);

		return hr;
	}

	void FFmpeg::DoStuff()
	{
		AVFormatContext* pFormatContext = nullptr;
		AVCodec* pCodec = nullptr;
		AVCodecContext* pCodecContext = nullptr;

		HRESULT hr = S_OK;
		if (FAILED(hr = this->Init_(&pFormatContext, &pCodec, &pCodecContext)))
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
			av_strerror(hr, errbuf, AV_ERROR_MAX_STRING_SIZE);
			logging("Failure: %s", errbuf);
		}

		if (SUCCEEDED(hr) && FAILED(hr = this->DoStuff_(pFormatContext, pCodec, pCodecContext)))
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
			av_strerror(hr, errbuf, AV_ERROR_MAX_STRING_SIZE);
			logging("Failure: %s", errbuf);
		}

		if (pFormatContext != nullptr)
		{
			avformat_close_input(&pFormatContext);
			avformat_free_context(pFormatContext);
		}

		if (pCodecContext != nullptr) avcodec_free_context(&pCodecContext);
	}
	
	inline HRESULT FFmpeg::DoStuff_(AVFormatContext* pFormatContext, AVCodec* pCodec, AVCodecContext* pCodecContext)
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
		while (SUCCEEDED(av_read_frame(pFormatContext, pPacket)))
		{
			if (pPacket->stream_index == this->m_StreamIndex) {
				if (FAILED(hr = this->DecodePacket_(&file, pCodecContext, pPacket, pFrame)))
					break;
			}
			av_packet_unref(pPacket);
		}

		enum AVSampleFormat sfmt = pCodecContext->sample_fmt;
		int channels = pCodecContext->channels;
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

		logging("Play the output audio file with the command:\nffplay -f %s -ac %d -ar %d %s", fmt, channels, pCodecContext->sample_rate, "[name]");

		file.close();
		av_packet_free(&pPacket);
		av_frame_free(&pFrame);

		return hr;
	}

	inline HRESULT FFmpeg::DecodePacket_(std::ofstream* ofstream, AVCodecContext* pCodecContext, AVPacket* pPacket, AVFrame* pFrame)
	{
		HRESULT hr = S_OK;

		if (FAILED(hr = avcodec_send_packet(pCodecContext, pPacket)))
			return hr;

		do
		{
			hr = avcodec_receive_frame(pCodecContext, pFrame);
			if (hr == AVERROR(EAGAIN) || hr == AVERROR_EOF) return S_OK;
			if (FAILED(hr)) return hr;

			size_t data_size = av_get_bytes_per_sample(pCodecContext->sample_fmt);
			if (data_size == 0) return E_FAIL;

			for (int i = 0; i < pFrame->nb_samples; ++i)
				for (int ch = 0; ch < pCodecContext->channels; ++ch)
				{
					if (pFrame->data[ch] == NULL) continue;
					ofstream->write(reinterpret_cast<const char*>(pFrame->data[ch]) + data_size * i, data_size);
				}


		} while (SUCCEEDED(hr));

		return hr;
	}

}