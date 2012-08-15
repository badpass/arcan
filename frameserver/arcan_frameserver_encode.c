#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <assert.h>

#include "../arcan_math.h"
#include "../arcan_general.h"
#include "../arcan_event.h"

#include "arcan_frameserver.h"
#include "../arcan_frameserver_shmpage.h"
#include "arcan_frameserver_encode.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

/* although there's a libswresampler already "handy", speex is used (a) for the simple relief in not having to deal with
 * more ffmpeg "APIs" and (b) for future VoIP features */
#include <speex/speex_resampler.h>

struct resampler {
	arcan_aobj_id source;
	float weight;
	
	SpeexResamplerState* resampler;
	
	struct resampler* next;
};

struct {
	struct frameserver_shmcont shmcont;
	
	struct arcan_evctx inevq;
	struct arcan_evctx outevq;

	AVFormatContext* fcontext;
	AVCodecContext* acontext;
	AVCodecContext* vcontext;

	struct SwsContext* ccontext;

	AVCodec* vcodec;
	long long lastframe; /* monotonic clock time-stamp */
	unsigned long framecount; 
	float fps;
	uint8_t* encvbuf;
	size_t encvbuf_sz;
		
	AVCodec* acodec;
	unsigned frequency;
	
/* muxing */
	AVStream* astream;
	AVStream* vstream;

	AVPacket packet;
	AVFrame* pframe;

/* two channels output + one resampler for now, 
 * add a float mixing buffer for 0.2.2 */
	uint8_t* encabuf;
	size_t encabuf_sz;
	struct resampler* resamplers;

/* precalc dstptrs into shm */
	uint8_t* vidp, (* audp);
	
/* sent from parent */
	file_handle lastfd;
	
} ffmpegctx = {0};

static struct resampler* grab_resampler(arcan_aobj_id id, unsigned frequency, unsigned channels)
{
/* no resampler setup, allocate */
	struct resampler* res = ffmpegctx.resamplers;
	struct resampler** ip = &ffmpegctx.resamplers;
	
	while (res){
		if (res->source == id)
			return res;

		if (res->next == NULL)
			ip = &res->next;
		
		res = res->next;
	}

/* no match, allocate */
	res = *ip = malloc( sizeof(struct resampler) );
	res->next = NULL;

	int errc;
	res->resampler = speex_resampler_init(channels, frequency, ffmpegctx.frequency, 3, &errc);
	res->weight = 1.0; 
	res->source = id;
	
	return res;
}

static size_t flush_audbuf()
{
	size_t cur_sz = ffmpegctx.shmcont.addr->abufused;
	uint8_t* audb = ffmpegctx.audp;
	off_t ofs = 0;
	unsigned hdr[5] = {/* src */ 0, /* size_t */ 0, /* frequency */ 0, /* channels */ 0, 0/* 0xfeedface */};
	
/* plow through the number of available bytes, these will be gone when stepframe is completed */
	while(cur_sz > sizeof(hdr) && ofs < ffmpegctx.encabuf_sz){
		memcpy(&hdr, audb, sizeof(hdr));
		assert(hdr[4] == 0xfeedface);
		audb += sizeof(hdr);
		struct resampler* rsmp = grab_resampler(hdr[0], hdr[2], hdr[3]);

/* parent guarantees stereo, SINT16LE, size in bytes, so 4 bytes per sample but resampler 
 * takes channels into account and wants the in count to be samples per channel (so half again) */
		unsigned inlen = hdr[1] >> 2; 

/* same samplerate, no conversion needed, just push */
		if (hdr[2] == ffmpegctx.frequency){
			memcpy(&ffmpegctx.encabuf[ofs], audb, hdr[1]);
			ofs += hdr[1];
		}
/* pass through speex resampler before pushing, no mixing of channels happen here */
		else {
			size_t out_sz     = inlen << 2;
			unsigned int outc = out_sz;

/* note the quirk that outc first represents the size of the buffer, THEN after resampling, it's the number of samples */
			speex_resampler_process_interleaved_int(rsmp->resampler, (uint16_t*) audb, &inlen, (uint16_t*) &ffmpegctx.encabuf[ofs], &outc);
			ofs += outc << 2;
		}

		audb += hdr[1];
		cur_sz -= hdr[1] + sizeof(hdr); 
	}
	
	return ofs;
}

static void encode_audio(bool flush)
{
/* if astream open and abuf, flush abuf into audio codec, feed both into muxer */
	size_t numb = flush_audbuf();
	int numf, got_packet;

	if (numb == 0 && !flush)
		return;

/* will be populated by encoder */
	do{
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.data = NULL;
		pkt.size = 0;

/* fill with result from audio buffer flush */
		AVFrame frame;
		avcodec_get_frame_defaults(&frame);
		frame.nb_samples = numb >> 2;
		avcodec_fill_audio_frame(&frame, 2, ffmpegctx.acontext->sample_fmt, ffmpegctx.encabuf, numb, 1); 

/* encode + send to multiplexer */
		if ( avcodec_encode_audio2(ffmpegctx.acontext, &pkt, flush ? NULL : &frame, &got_packet) == 0 && got_packet){
			pkt.flags |= AV_PKT_FLAG_KEY;
			pkt.stream_index = ffmpegctx.astream->index;

/* really no way to recover from this, if we can't mux once, feeding empty audio frames and hoping
 * that the video frame stays intact is a dead race, give up */
			if (av_interleaved_write_frame(ffmpegctx.fcontext, &pkt) != 0){
				ffmpegctx.acontext = NULL;
				return;
			}
				
			av_free_packet(&pkt);
		}
	}
	while (flush && got_packet);
}

void encode_video(bool flush)
{
	uint8_t* srcpl[4] = {ffmpegctx.vidp, NULL, NULL, NULL};
	int srcstr[4] = {0, 0, 0, 0};
	srcstr[0] = ffmpegctx.vcontext->width * 4;

/* the main problem here is that the source material may encompass many framerates, in fact, even be variable (!) 
 * the samplerate we're running with that is of interest. Thus compare the current time against the next expected time-slots,
 * if we're running behind, just repeat the last frame N times as to not get out of synch with possible audio. */
	double mspf = 1000.0 / ffmpegctx.fps;
	double thresh = mspf * 0.5;
	long long encb    = frameserver_timemillis();
	long long cft     = encb - ffmpegctx.lastframe; /* assumed >= 0 */
	long long nf      = round( mspf * (double)ffmpegctx.framecount );
	long long delta   = cft - nf;
	unsigned fc       = 1;

/* ahead by too much? give up */
	if (delta < -thresh)
		return;
		
/* running behind, need to repeat a frame */
	if ( delta > thresh )
		fc += 1 + floor( delta / mspf);
		
	int rv = sws_scale(ffmpegctx.ccontext, (const uint8_t* const*) srcpl, srcstr, 0, ffmpegctx.vcontext->height, ffmpegctx.pframe->data, ffmpegctx.pframe->linesize);
	ffmpegctx.framecount += fc;
//		printf("frame(delta:%lld) @ %lld vs %lld\n", cft - nf, cft, nf);
		
	while (fc--){
		int rs = avcodec_encode_video(ffmpegctx.vcontext, ffmpegctx.encvbuf, ffmpegctx.encvbuf_sz, ffmpegctx.pframe);
		AVCodecContext* ctx = ffmpegctx.vcontext;
		AVPacket pkt;
		av_init_packet(&pkt);
			
		if (ctx->coded_frame->pts != AV_NOPTS_VALUE)
			pkt.pts = av_rescale_q(ctx->coded_frame->pts, ctx->time_base, ffmpegctx.vstream->time_base);
	
		pkt.stream_index = ffmpegctx.vstream->index;
		pkt.data = ffmpegctx.encvbuf;
		pkt.size = rs;

		int rv = av_interleaved_write_frame(ffmpegctx.fcontext, &pkt);
	}
}

void arcan_frameserver_stepframe(bool flush)
{
	if (ffmpegctx.acontext)
		encode_audio(flush);
	
	if (ffmpegctx.vcontext)
		encode_video(flush);
	
	ffmpegctx.shmcont.addr->abufused = 0;
	arcan_sem_post(ffmpegctx.shmcont.vsem);
}

static void encoder_atexit()
{
	if (ffmpegctx.lastfd != BADFD){
		arcan_frameserver_stepframe(true);

		if (ffmpegctx.fcontext)
			av_write_trailer(ffmpegctx.fcontext);
	}
}

static void setup_videocodec(const char* req)
{
	if (req){
		ffmpegctx.vcodec = avcodec_find_encoder_by_name(req);
		if (!ffmpegctx.vcodec)
		LOG("arcan_frameserver(encode) -- requested codec (%s) not found, trying h264.\n", req);
	}
	
	if (!ffmpegctx.vcodec)
		ffmpegctx.vcodec = avcodec_find_encoder(CODEC_ID_H264);
	
	if (!ffmpegctx.vcodec){
		LOG("arcan_frameserver(encode) -- no h264 support found, trying FLV1 (lossless) video.\n");
		ffmpegctx.vcodec = avcodec_find_encoder(CODEC_ID_FLV1);
	}
	
/* fallback to the lossless RLEish "almost always built in" codec */
	if (!ffmpegctx.vcodec){
		LOG("arcan_frameserver(encode) -- no FLV1 support found, trying mpeg1video.\n");
		ffmpegctx.vcodec = avcodec_find_encoder(CODEC_ID_MPEG1VIDEO); 
	}

	if (!ffmpegctx.vcodec){
		LOG("arcan_frameserver(encode) -- no suitable video codec found, no video- stream will be stored.\n");
	}
}

static void setup_audiocodec(const char* req)
{
	ffmpegctx.acodec = avcodec_find_encoder_by_name(req);
	
	if (req){
		ffmpegctx.acodec = avcodec_find_encoder_by_name(req);
		if (!ffmpegctx.acodec)
			LOG("arcan_frameserver(encode) -- requested audio codec (%s) not found, trying FLAC.\n", req);
	}
	
	if (!ffmpegctx.acodec)
		ffmpegctx.acodec = avcodec_find_encoder(CODEC_ID_FLAC);

	if (!ffmpegctx.acodec){
		LOG("arcan_frameserver(encode) -- no FLAC support found, trying vorbis.\n", req);
		ffmpegctx.acodec = avcodec_find_encoder(CODEC_ID_VORBIS);
	}
	
	if (!ffmpegctx.acodec){
		LOG("arcan_frameserver(encode) -- no vorbis support found, trying raw (PCM/S16LE).\n", req);
		ffmpegctx.acodec = avcodec_find_encoder(CODEC_ID_PCM_S16LE);
	}
	
	if (!ffmpegctx.acodec)
		LOG("arcan_frameserver(encode) -- no suitable audio codec found, no audio- stream will be stored.\n");
}

static void setup_container(const char* req)
{
	AVOutputFormat* fmt = NULL;
	
	if (req){
		fmt = av_guess_format(req, NULL, NULL);
		if (!fmt)
			LOG("arcan_frameserver(encode) -- requested container (%s) not found, trying Matroska (MKV).\n", req);
	}
	
	if (!fmt)
		fmt = av_guess_format("matroska", NULL, NULL);

	if (!fmt){
		fmt = av_guess_format("mp4", NULL, NULL);
		LOG("arcan_frameserver(encode) -- Matroska container not found, trying MP4.\n", req);
	}
	
	if (!fmt){
		LOG("arcan_frameserver(encode) -- MP4 container not found, trying AVI.\n", req);
		fmt = av_guess_format("avi", NULL, NULL);
	}
	
	if (!fmt)
		LOG("arcan_frameserver(encode) -- no suitable container format found, giving up.\n");
	else {
		char fdbuf[16];
		
		ffmpegctx.fcontext = avformat_alloc_context();
		ffmpegctx.fcontext->oformat = fmt;

		sprintf(fdbuf, "pipe:%d", ffmpegctx.lastfd);
		int rv = avio_open2(&ffmpegctx.fcontext->pb, fdbuf, AVIO_FLAG_WRITE, NULL, NULL);
		if (rv == 0){
			LOG("arcan_frameserver(encode) -- muxer output prepared.\n");
		}
		else {
			LOG("arcan_frameserver(encode) -- muxer output failed (%d).\n", rv);
			ffmpegctx.fcontext = NULL; /* give up */
		}
	}
}

static bool setup_ffmpeg_encode(const char* resource)
{
	struct frameserver_shmpage* shared = ffmpegctx.shmcont.addr;

	static bool initialized = false;
	if (!initialized){
		avcodec_register_all();
		av_register_all();
		initialized = true;
	}
	
/* codec stdvals */
	unsigned vbr = 720 * 1024;
	unsigned abr = 192 * 1024;
	unsigned afreq = 44100;
	float fps    = 25.0;
	unsigned gop = 10;
/* decode encoding options from the resourcestr */
	char* base = strdup(resource);
	char* blockb = base;
	char* vck = NULL, (* ac) = NULL, (* cont) = NULL;

/* parse user args and override defaults */
	while (*blockb){
		blockb = (blockb = index(base, ':')) ? (*blockb = '\0', blockb) : base + strlen(base);

		char* splitp = index(base, '=');
		if (!splitp){
			LOG("arcan_frameserver(encode) -- couldn't parse resource (%s)\n", resource);
			break;
		}
		
		else {
			*splitp++ = '\0';
/* video bit-rate */
			LOG("check: %s, %s\n", base, splitp);
			if (strcmp(base, "vbitrate") == 0)
				vbr = strtoul(splitp, NULL, 10);
			else if (strcmp(base, "vcodec") == 0)
/* videocodec */
				vck = splitp;
			else if (strcmp(base, "abitrate") == 0)
/* audio bit-rate */
				abr = strtoul(splitp, NULL, 10);
			else if (strcmp(base, "acodec") == 0)
/* audiocodec */
				ac = splitp;
/* mux format */
			else if (strcmp(base, "container") == 0)
				cont = splitp;
			else if (strcmp(base, "samplerate") == 0)
/* audio samples per second */
				afreq = strtoul(splitp, NULL, 0);
			else if (strcmp(base, "fps") == 0)
/* videoframes per second */
				fps = strtof(splitp, NULL);	
			else
				LOG("arcan_frameserver(encode) -- parsing resource string, unknown base : %s\n", base);
		}
		
		blockb++; /* should now point to new base or NULL */
		base = blockb;
	}

/* sanity- check decoded values */
	if (fps < 4 || fps > 60){
			LOG("arcan_frameserver(encode:%s) -- bad framerate (fps) argument, defaulting to 25.0fps\n", resource);
			fps = 25;
	}
	
	LOG("arcan_frameserver(encode:args) -- Parsing complete, values:\nvcodec: (%s:%f fps @ %d b/s), acodec: (%s:%d rate %d b/s), container: (%s)\n",
			vck?vck:"default",  fps, vbr, ac?ac:"default", afreq, abr, cont?cont:"default");
	setup_videocodec(vck);
	setup_audiocodec(ac);
	setup_container(cont);

	if (ffmpegctx.fcontext == NULL || (!ffmpegctx.vcodec && !ffmpegctx.acodec)){
		return false;
	}

	int contextc = 0;
/* at least some combination was found */
	frameserver_shmpage_calcofs(shared, &(ffmpegctx.vidp), &(ffmpegctx.audp) );
	
	if (ffmpegctx.vcodec){
/* tend to be among the color formats codec people mess up the least */
		enum PixelFormat c_pix_fmt = PIX_FMT_YUV420P;

		struct AVRational timebase = {1, fps};
		AVCodecContext* ctx = avcodec_alloc_context();
	
		ctx->bit_rate = vbr;
		ctx->pix_fmt = c_pix_fmt;
		ctx->gop_size = gop;
		ctx->time_base = timebase;
		ffmpegctx.fps = fps;
		ffmpegctx.frequency = afreq;
		ctx->width = shared->w;
		ctx->height = shared->h;
	
		ffmpegctx.vcontext = ctx;
	
		if (avcodec_open2(ffmpegctx.vcontext, ffmpegctx.vcodec, NULL) < 0){
			LOG("arcan_frameserver_ffmpeg_encode() -- video codec open failed.\n");
			avcodec_close(ctx);
			ffmpegctx.vcodec = NULL;
		} else {
/* default to YUV420P, so then we need this framelayout */
			size_t base_sz = shared->w * shared->h;
			ffmpegctx.pframe = avcodec_alloc_frame();
			ffmpegctx.pframe->data[0] = malloc( (base_sz * 3) / 2);
			ffmpegctx.pframe->data[1] = ffmpegctx.pframe->data[0] + base_sz;
			ffmpegctx.pframe->data[2] = ffmpegctx.pframe->data[1] + base_sz / 4;
			ffmpegctx.pframe->linesize[0] = shared->w;
			ffmpegctx.pframe->linesize[1] = shared->w / 2;
			ffmpegctx.pframe->linesize[2] = shared->w / 2;

/* larger than this and we have a really crap codec ;-) */
			ffmpegctx.encvbuf_sz = base_sz * 4;
			ffmpegctx.encvbuf    = malloc(ffmpegctx.encvbuf_sz);
	
			ffmpegctx.ccontext = sws_getContext(ffmpegctx.vcontext->width, ffmpegctx.vcontext->height, PIX_FMT_RGBA, ffmpegctx.vcontext->width,
				ffmpegctx.vcontext->height, PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);
			
/* attach to output container */
			ffmpegctx.vstream = av_new_stream(ffmpegctx.fcontext, contextc++);
			ffmpegctx.vstream->codec = ffmpegctx.vcontext;
		}
	}

	if (ffmpegctx.acodec){
		AVCodecContext* ctx = avcodec_alloc_context();
		
		ctx->sample_rate = afreq;
		ctx->time_base   = av_d2q(1.0 / ctx->sample_rate, 1000000);
		ctx->channels    = 2;
		ctx->sample_fmt  = AV_SAMPLE_FMT_S16;
		ffmpegctx.acontext = ctx;
		
		if (avcodec_open2(ffmpegctx.acontext, ffmpegctx.acodec, NULL) < 0){
			LOG("arcan_frameserver_ffmpeg_encode() -- audio codec open failed.\n");
			avcodec_close(ctx);
			ffmpegctx.acodec = NULL;
		} 
		else{
			ffmpegctx.encabuf_sz = SHMPAGE_AUDIOBUF_SIZE * 2;
			ffmpegctx.encabuf = malloc(ffmpegctx.encabuf_sz); 
			ffmpegctx.astream = av_new_stream(ffmpegctx.fcontext, contextc++);
			ffmpegctx.astream->codec = ffmpegctx.acontext;
		}
	}
	
/* setup container, we now have valid codecs, streams attached to the codecs and a container with the streams */
	if (ffmpegctx.fcontext){
		avformat_write_header(ffmpegctx.fcontext, NULL);
	}
	else {
			LOG("arcan_frameserver_ffmpeg_encode() -- no suitable output container, giving up.\n");
			return false;
	}
	
/* signal that we are ready to receive */
	arcan_sem_post(ffmpegctx.shmcont.vsem);
	return true;
}

void arcan_frameserver_ffmpeg_encode(const char* resource, const char* keyfile)
{
/* setup shmpage etc. resolution etc. is already in place thanks to the parent */
	ffmpegctx.shmcont = frameserver_getshm(keyfile, true);
	struct frameserver_shmpage* shared = ffmpegctx.shmcont.addr;
	frameserver_shmpage_setevqs(ffmpegctx.shmcont.addr, ffmpegctx.shmcont.esem, &(ffmpegctx.inevq), &(ffmpegctx.outevq), false); 
	ffmpegctx.lastfd = BADFD;
	atexit(encoder_atexit);

/* main event loop */
	while (true){
		arcan_event* ev = arcan_event_poll(&ffmpegctx.inevq);

		if (ev){
			switch (ev->kind){
				case TARGET_COMMAND_FDTRANSFER:
					if (ffmpegctx.lastfd != BADFD)
						arcan_frameserver_stepframe(true); /* flush */
					
					ffmpegctx.lastframe = frameserver_timemillis();
					ffmpegctx.lastfd = frameserver_readhandle(ev);
					if (!setup_ffmpeg_encode(resource))
						return;
				break;

				case TARGET_COMMAND_STEPFRAME: 
					if (ffmpegctx.lastfd != BADFD)
						arcan_frameserver_stepframe(false);	
				break;

				case TARGET_COMMAND_STORE:
					return;
				break;
			}
		}

		frameserver_delay(1);
	}
}