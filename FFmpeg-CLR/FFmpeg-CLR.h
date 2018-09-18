#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <winerror.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "Storage.h"

namespace FFmpeg
{
	public ref class FFmpeg sealed
	{
	public:
		FFmpeg(System::String^ fileIn, System::String^ fileOut);
		void DoStuff();

	private:
		~FFmpeg();
		!FFmpeg();

		// Hack to not have interior_ptr<T> but native pointers
		Storage* m_Storage;

		const char* m_FileIn;
		const char* m_FileOut;
		int m_StreamIndex;
		int m_GotFrame;
		int m_FrameCount = 0;

		inline HRESULT Init_();
		inline HRESULT DoStuff_();
		inline void DecodePacket_(std::ofstream* ofstream, AVPacket* pPacket, AVFrame* pFrame);
	};
}
