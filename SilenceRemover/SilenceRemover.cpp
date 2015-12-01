/************************************************************************/
/* SilenceRemover                                                       */
/* 2015 by Johannes Schultz; http://sagamusix.de/                       */
/*                                                                      */
/* As the name implies, this program removes silence from the beginning */
/* of a WAV or FLAC file. Useful in batch-processing batch-recorded     */
/* samples.                                                             */
/* License: BSD 3-clause                                                */
/************************************************************************/

#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <sys/utime.h>
#define FLAC__NO_DLL
#include <flac/include/FLAC/stream_decoder.h>
#include <flac/include/FLAC/stream_encoder.h>
#include <flac/include/FLAC/metadata.h>

static double delay = 0.0;
static uint32_t sampleRate = 0;

struct WAVFormatChunk
{
	// Sample formats
	enum SampleFormats
	{
		fmtPCM			= 1,
		fmtFloat		= 3,
		fmtExtensible	= 0xFFFE,
	};

	uint16_t format;			// Sample format, see SampleFormats
	uint16_t numChannels;		// Number of audio channels
	uint32_t sampleRate;		// Sample rate in Hz
	uint32_t byteRate;			// Bytes per second (should be freqHz * blockAlign)
	uint16_t blockAlign;		// Size of a sample, in bytes (do not trust this value, it's incorrect in some files)
	uint16_t bitsPerSample;		// Bits per sample
};

static_assert(sizeof(WAVFormatChunk) == 16, "Check alignment");

// Sample information chunk
struct WAVSampleInfoChunk
{
	uint32_t manufacturer;
	uint32_t product;
	uint32_t samplePeriod;	// 1000000000 / sampleRate
	uint32_t baseNote;		// MIDI base note of sample
	uint32_t pitchFraction;
	uint32_t SMPTEFormat;
	uint32_t SMPTEOffset;
	uint32_t numLoops;		// number of loops
	uint32_t samplerData;
};

static_assert(sizeof(WAVSampleInfoChunk) == 36, "Check Alignemnt");


// Sample loop information chunk (found after WAVSampleInfoChunk in "smpl" chunk)
struct WAVSampleLoop
{
	uint32_t identifier;
	uint32_t loopType;		// See LoopType
	uint32_t loopStart;		// Loop start in samples
	uint32_t loopEnd;		// Loop end in samples
	uint32_t fraction;
	uint32_t playCount;		// Loop Count, 0 = infinite
};

static_assert(sizeof(WAVSampleLoop) == 24, "Check Alignemnt");

static bool DecodeWAV(FILE *f, FILE *of)
{
	fseek(f, 0, SEEK_SET);
	char magic[12];
	fread(magic, 12, 1, f);
	if(memcmp(magic, "RIFF", 4) || memcmp(magic + 8, "WAVE", 4))
	{
		return false;
	}
	fwrite(magic, 12, 1, of);

	WAVFormatChunk fmt;
	memset(&fmt, 0, sizeof(fmt));
	char buffer[4096];
	uint32_t delayBytes = 0, delaySamples = 0;
	
	while(!feof(f))
	{
		int32_t chunkSize;
		magic[0] = 0;
		if(!fread(magic, 4, 1, f) || !fread(&chunkSize, 4, 1, f))
		{
			break;
		}
		fwrite(magic, 4, 1, of);

		long nextPos = ftell(f) + chunkSize;
		bool oddSize = (chunkSize & 1);

		if(!memcmp(magic, "fmt ", 4) && chunkSize == sizeof(fmt))
		{
			fread(&fmt, sizeof(fmt), 1, f);
			fwrite(&chunkSize, 4, 1, of);
			fwrite(&fmt, sizeof(fmt), 1, of);

			if(!sampleRate) sampleRate = fmt.sampleRate;
			else fmt.sampleRate = sampleRate;
			delaySamples = static_cast<uint32_t>(0.5 + (delay * sampleRate) / 1000.0);
			delayBytes = static_cast<uint32_t>(0.5 + (delay * (sampleRate * fmt.numChannels * ((fmt.bitsPerSample + 7) / 8))) / 1000.0);
		} else if(!memcmp(magic, "data", 4))
		{
			if(fmt.format != WAVFormatChunk::fmtPCM && fmt.format != WAVFormatChunk::fmtFloat)
			{
				return false;
			}
			fseek(f, delayBytes, SEEK_CUR);
			chunkSize -= delayBytes;
			fwrite(&chunkSize, 4, 1, of);
			bool oddWrite = (chunkSize & 1);

			while(chunkSize > 0)
			{
				uint32_t thisRead = std::min<uint32_t>(chunkSize, sizeof(buffer));
				fread(buffer, thisRead, 1, f);
				fwrite(buffer, thisRead, 1, of);
				chunkSize -= thisRead;
			}
			if(oddWrite)
			{
				buffer[0] = 0;
				fwrite(buffer, 1, 1, of);
			}
		} else if(!memcmp(magic, "smpl", 4))
		{
			fwrite(&chunkSize, 4, 1, of);
			WAVSampleInfoChunk smpl;
			fread(&smpl, sizeof(smpl), 1, f);
			fwrite(&smpl, sizeof(smpl), 1, of);
			for(uint32_t i = 0; i < smpl.numLoops; i++)
			{
				WAVSampleLoop loop;
				fread(&loop, sizeof(loop), 1, f);
				if(loop.loopStart >= delaySamples) loop.loopStart -= delaySamples;
				if(loop.loopEnd >= delaySamples) loop.loopEnd -= delaySamples;
				fwrite(&loop, sizeof(loop), 1, of);
			}
		} else
		{
			fwrite(&chunkSize, 4, 1, of);
			if(oddSize) chunkSize++;
			while(chunkSize > 0)
			{
				uint32_t thisRead = std::min<uint32_t>(chunkSize, sizeof(buffer));
				fread(buffer, thisRead, 1, f);
				fwrite(buffer, thisRead, 1, of);
				chunkSize -= thisRead;
			}
		}

		if(fseek(f, nextPos + (oddSize ? 1 : 0), SEEK_SET))
		{
			return false;
		}
	}
	uint32_t size = ftell(of);
	fseek(of, 4, SEEK_SET);
	fwrite(&size, 4, 1, of);

	return true;
}

struct FLACClientData
{
	std::vector<FLAC__StreamMetadata *> metadata;
	FILE *of;
	FLAC__StreamEncoder *encoder;
	uint32_t delaySamples;
	uint32_t channels;
	bool started;
};

static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data)
{
	FLACClientData &client = *static_cast<FLACClientData *>(client_data);

	if(!client.started)
	{
		client.started = true;
		client.delaySamples = static_cast<uint32_t>(0.5 + (delay * sampleRate) / 1000.0);

		for(auto m = client.metadata.begin(); m != client.metadata.end(); m++)
		{
			auto metadata = *m;
			if(metadata->type == FLAC__METADATA_TYPE_APPLICATION && !memcmp(metadata->data.application.id, "riff", 4))
			{
				if(metadata->length > 8 + sizeof(WAVSampleInfoChunk) && !memcmp(metadata->data.application.data, "smpl", 4))
				{
					WAVSampleInfoChunk *smpl = reinterpret_cast<WAVSampleInfoChunk *>(metadata->data.application.data + 8);
					for(uint32_t i = 0; i < smpl->numLoops; i++)
					{
						WAVSampleLoop *loop = reinterpret_cast<WAVSampleLoop *>(smpl + 1) + i;
						if(loop->loopStart >= client.delaySamples) loop->loopStart -= client.delaySamples;
						if(loop->loopEnd >= client.delaySamples) loop->loopEnd -= client.delaySamples;
					}
				}
			}
		}

		if(!FLAC__stream_encoder_set_metadata(client.encoder, client.metadata.data(), client.metadata.size()))
		{
			std::cerr << "Cannot set FLAC metadata!" << std::endl;
			client.started = false;
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
		
		if(FLAC__stream_encoder_init_FILE(client.encoder, client.of, nullptr, nullptr) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		{
			std::cerr << "Cannot init FLAC encoder!" << std::endl;
			client.started = false;
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
	}
	
	if(client.delaySamples >= frame->header.blocksize)
	{
		client.delaySamples -= frame->header.blocksize;
	} else if(client.delaySamples > 0)
	{
		std::vector<const FLAC__int32 *> newBuffer(client.channels);
		for(uint32_t chn = 0; chn < client.channels; chn++)
		{
			newBuffer[chn] = buffer[chn] + client.delaySamples;
		}
		FLAC__stream_encoder_process(client.encoder, newBuffer.data(), frame->header.blocksize - client.delaySamples);
		client.delaySamples = 0;
	} else
	{
		FLAC__stream_encoder_process(client.encoder, buffer, frame->header.blocksize);
	}

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_cb(const FLAC__StreamDecoder *, const FLAC__StreamMetadata *metadata, void *client_data)
{
	FLACClientData &client = *static_cast<FLACClientData *>(client_data);

	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO && metadata->data.stream_info.total_samples != 0)
	{
		if(!sampleRate) sampleRate = metadata->data.stream_info.sample_rate;
		client.channels = metadata->data.stream_info.channels;

		if(!FLAC__format_sample_rate_is_subset(sampleRate))
		{
			// FLAC only supports 10 Hz granularity for frequencies above 65535 Hz if the streamable subset is chosen.
			FLAC__stream_encoder_set_streamable_subset(client.encoder, false);
		}
		FLAC__stream_encoder_set_channels(client.encoder, client.channels);
		FLAC__stream_encoder_set_bits_per_sample(client.encoder, metadata->data.stream_info.bits_per_sample);
		FLAC__stream_encoder_set_sample_rate(client.encoder, sampleRate <= FLAC__MAX_SAMPLE_RATE ? sampleRate : FLAC__MAX_SAMPLE_RATE);
		FLAC__stream_encoder_set_total_samples_estimate(client.encoder, metadata->data.stream_info.total_samples);
		FLAC__stream_encoder_set_compression_level(client.encoder, 8);
	} else if(metadata->type == FLAC__METADATA_TYPE_VORBIS_COMMENT)
	{
		for(FLAC__uint32 i = 0; i < metadata->data.vorbis_comment.num_comments; i++)
		{
			const char *tag = reinterpret_cast<const char *>(metadata->data.vorbis_comment.comments[i].entry);
			const FLAC__uint32 length = metadata->data.vorbis_comment.comments[i].length;
			if(length > 11 && !_strnicmp(tag, "SAMPLERATE=", 11))
			{
				sampleRate = atoi(tag + 11);
			}
		}
		client.metadata.push_back(FLAC__metadata_object_clone(metadata));
	} else
	{
		client.metadata.push_back(FLAC__metadata_object_clone(metadata));
	}
}

static void error_cb(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus, void *)
{
}

static bool DecodeFLAC(FILE *f, FILE *of)
{
	fseek(f, 0, SEEK_SET);
	FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
	if(decoder == nullptr)
	{
		return false;
	}

	FLAC__stream_decoder_set_metadata_respond_all(decoder);

	FLACClientData client;
	client.of = of;
	client.started = false;
	client.encoder = FLAC__stream_encoder_new();

	FLAC__StreamDecoderInitStatus initStatus = FLAC__stream_decoder_init_FILE(decoder, f, write_cb, metadata_cb, error_cb, &client);

	if(initStatus != FLAC__STREAM_DECODER_INIT_STATUS_OK || client.encoder == nullptr)
	{
		FLAC__stream_decoder_delete(decoder);
		return false;
	}

	FLAC__stream_decoder_process_until_end_of_stream(decoder);
	FLAC__stream_decoder_finish(decoder);
	FLAC__stream_decoder_delete(decoder);
	FLAC__stream_encoder_finish(client.encoder);
	FLAC__stream_encoder_delete(client.encoder);

	for(auto m = client.metadata.begin(); m != client.metadata.end(); m++)
	{
		FLAC__metadata_object_delete(*m);
	}

	return client.started;
}

int wmain(int argc, wchar_t* argv[])
{
	if(argc < 3)
	{
		std::wcerr << L"Usage: " << argv[0] << L" infile delay [forced samplerate]" << std::endl;
		std::wcerr << L"Delay is in milliseconds and may be fractional." << std::endl;
		return 1;
	}

	delay = _wtof(argv[2]);
	if(delay <= 0)
	{
		std::wcerr << L"Error: Delay must be positive millisecond value, may be fractional." << std::endl;
		return 1;
	}

	if(argc >= 4)
	{
		sampleRate = _wtoi(argv[3]);
	}

	FILE *f = _wfopen(argv[1], L"rb");
	if(f == nullptr)
	{
		std::wcerr << L"Cannot open: " << argv[1] << L" for reading!" << std::endl;
		return 1;
	}

	std::wstring newFile = argv[1];
	newFile += L".tmp";
	FILE *of = _wfopen(newFile.c_str(), L"wb");
	if(of == nullptr)
	{
		std::wcerr << L"Cannot open output file: " << newFile.c_str() << L" for writing!" << std::endl;
		return 1;
	}

	if(!DecodeWAV(f, of) && !DecodeFLAC(f, of))
	{
		std::wcerr << L"Error: Unknown file type" << std::endl;
		fclose(of);
		_wremove(newFile.c_str());
		return 1;
	}

	fclose(f);
	fclose(of);

	_stat stat;
	_wstat(argv[1], &stat);
	_utimbuf timbuf;
	timbuf.actime = stat.st_atime;
	timbuf.modtime = stat.st_mtime;
	_wutime(newFile.c_str(), &timbuf);
	
	_wremove(argv[1]);
	_wrename(newFile.c_str(), argv[1]);

	return 0;
}
