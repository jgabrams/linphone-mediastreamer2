/*
mediastreamer2 library - modular sound and video processing and streaming
Copyright (C) 2006  Simon MORLAT (simon.morlat@linphone.org)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifdef HAVE_CONFIG_H
#include "mediastreamer-config.h"
#endif

#include "mediastreamer2/msfilter.h"
#include "mediastreamer2/msvideo.h"
#include "layouts.h"
#include "ffmpeg_lock.h"
#include "ffmpeg-priv.h"

#ifndef VIDEOFILE
#define VIDEOFILE "outputvideo"
#endif
#ifndef VIDEOEXT
#define VIDEOEXT "mp4"
#endif

#define FPS 25

static int bit_buffer_size = 4*1024*1024;
static uint8_t *bit_buffer = NULL;
static int file_num=0;

typedef struct _RecordedVideo
{
	char*			path;
	AVFormatContext *av_format_ctx;
	AVCodecContext 	*av_context;
	AVCodec			*av_codec;
	AVOutputFormat	*av_format_out;
	AVStream 		*av_stream;
	struct SwsContext *sws_ctx;
	AVPacket 		pkt;
	MSPicture 		fbuf;
	mblk_t 			*local_msg;
	MSVideoSize 	wsize; /*wished window size */
	bool_t 			ready;
	bool_t 			run;
	bool_t			restart;
	long			frame;
	bool_t			autoresize;
	ms_thread_t 	thread;
	ms_mutex_t 		mutex;
	queue_t 		rq;
} RecordedVideo;

static void msvidrec_unprepare(RecordedVideo *s);
static void msvidrec_processframe(RecordedVideo *obj, mblk_t *inm);
void *thread(void* args);
static void msvidrec_start(MSFilter *f);
static void msvidrec_prepare(RecordedVideo *s);
static void msvidrec_unprepare(RecordedVideo *s);


static void msvidrec_init(MSFilter  *f){
	RecordedVideo *obj=(RecordedVideo*)ms_new0(RecordedVideo,1);
	MSVideoSize def_size;
	def_size.width=MS_VIDEO_SIZE_CIF_W;
	def_size.height=MS_VIDEO_SIZE_CIF_H;
	obj->local_msg=NULL;
	obj->ready=FALSE;
	obj->wsize=def_size;
	obj->run=FALSE;
	obj->restart=FALSE;
	qinit(&obj->rq);
	ms_mutex_init(&obj->mutex,NULL);

	obj->path=malloc(strlen(VIDEOFILE)+strlen(VIDEOEXT)+25);
	if(obj->path==NULL){
		ms_message("Can't allocate buffer");
		return;
	}
	sprintf(obj->path,"./%s.%i.%s", VIDEOFILE,file_num,VIDEOEXT);
	file_num++;

	obj->av_codec=NULL;
	obj->av_context=NULL;
	obj->av_format_ctx=NULL;
	obj->sws_ctx=NULL;
	obj->av_format_out=NULL;
	obj->av_stream=NULL;
	f->data=obj;

	bit_buffer=malloc(bit_buffer_size);
	if(bit_buffer==NULL){
		ms_message("Can't allocate buffer");
		return;
	}

	av_register_all();

	msvidrec_start(f);
}

static void msvidrec_start(MSFilter *f){
	RecordedVideo *s=(RecordedVideo*)f->data;

	if(s->run==TRUE){
		return;
	}

	s->run=TRUE;
	msvidrec_prepare(s);
	if(ms_thread_create(&s->thread,NULL,thread,s)<0){
		ms_message("creating thread failed!");
	}
}

static void msvidrec_stop(MSFilter *f){
	RecordedVideo *s=(RecordedVideo*)f->data;

	if(s->run==FALSE){
		return;
	}

	s->run=FALSE;
	ms_thread_join(s->thread,NULL);
	msvidrec_unprepare(s);
}

static void msvidrec_uninit(MSFilter *f){
	RecordedVideo *obj=(RecordedVideo*)f->data;
	msvidrec_stop(f);
	ms_mutex_destroy(&obj->mutex);
	ms_free(obj->path);
	ms_free(obj);
	free(bit_buffer);
}

static int pick_bitrate(MSVideoSize *size){
	if(size->width==MS_VIDEO_SIZE_1080P_W && size->height==MS_VIDEO_SIZE_1080P_H){
		return 20971520;
	}else if(size->width==MS_VIDEO_SIZE_720P_W && size->height==MS_VIDEO_SIZE_720P_H){
		return 8192000;
	}else if(size->width==MS_VIDEO_SIZE_SVGA_W && size->height==MS_VIDEO_SIZE_SVGA_H){
		return 4096000;
	}else if(size->width==MS_VIDEO_SIZE_VGA_W && size->height==MS_VIDEO_SIZE_VGA_H){
		return 1024000;
	}else if(size->width==MS_VIDEO_SIZE_CIF_W && size->height==MS_VIDEO_SIZE_CIF_H){
		return 800000;
	}else if(size->width==MS_VIDEO_SIZE_QVGA_W && size->height==MS_VIDEO_SIZE_QVGA_H){
		return 170000;
	}else if(size->width==MS_VIDEO_SIZE_QCIF_W && size->height==MS_VIDEO_SIZE_QCIF_H){
		return 128000;
	}else{
		return 800000;
	}
}

static void msvidrec_prepare(RecordedVideo *s){

	if(s->ready==TRUE){
		return;
	}

	/*Grab output format*/
	s->av_format_out=av_guess_format(NULL, s->path, NULL);
    if(!s->av_format_out){
    	ms_message("Could not deduce output format from file extension: using MPEG");
    	s->av_format_out = av_guess_format("mpeg", NULL, NULL);
    }
    if (!s->av_format_out) {
    	ms_message("Could not find suitable output format");
        return;
    }

    s->av_format_ctx=avformat_alloc_context();
	if(!s->av_format_ctx){
		return;
	}

	s->av_format_ctx->oformat = s->av_format_out;
	snprintf(s->av_format_ctx->filename, sizeof(s->av_format_ctx->filename), "%s", s->path);

	/*Find Encoder*/
	s->av_format_out->video_codec=CODEC_ID_MPEG4;
	s->av_codec = avcodec_find_encoder(s->av_format_out->video_codec);
	if (!s->av_codec) {
		ms_message("Codec not found");
		return;
	}

	// add the video stream using the default format codec and initialize the codecs
	s->av_stream = avformat_new_stream(s->av_format_ctx, s->av_codec);
	if (!s->av_stream) {
		return;
	}
	s->av_context=s->av_stream->codec;
	if (s->av_stream->codec == NULL) {
		return;
	}

	/* put sample parameters */
	s->av_context->bit_rate = pick_bitrate(&s->wsize);
	s->av_context->bit_rate_tolerance=s->av_context->bit_rate;
	s->av_context->qmin=2;
	s->av_context->width = s->wsize.width;
	s->av_context->height = s->wsize.height;
	/* frames per second */
	s->av_context->time_base= (AVRational){1,FPS};
	s->av_context->gop_size = 250;
	s->av_context->max_b_frames=0;
	s->av_context->pix_fmt = PIX_FMT_YUV420P;

	/* open codec*/
	ms_mutex_lock(&ffmpeg_avcodec_open_close_global_mutex);
	if (avcodec_open2(s->av_context, s->av_codec, NULL) < 0) {
		ms_message("Could not open codec");
		ms_mutex_unlock(&ffmpeg_avcodec_open_close_global_mutex);
		return;
	}
	ms_mutex_unlock(&ffmpeg_avcodec_open_close_global_mutex);

	/* open the output file, if needed */
	if (!(s->av_format_out->flags & AVFMT_NOFILE)) {
		if (avio_open(&s->av_format_ctx->pb, s->path, AVIO_FLAG_WRITE) <0) {
			ms_message("Could not open '%s'", s->path);
			return;
		}
	}

	// some formats want stream headers to be separate
	if(s->av_format_ctx->oformat->flags & AVFMT_GLOBALHEADER)
			s->av_context->flags |= CODEC_FLAG_GLOBAL_HEADER;

	/*Write header, if needed*/
    avformat_write_header(s->av_format_ctx, NULL);

    s->sws_ctx=sws_getContext(s->wsize.width,s->wsize.height,PIX_FMT_YUV420P,
    		s->av_context->width,s->av_context->height,s->av_context->pix_fmt,
    		SWS_FAST_BILINEAR,NULL, NULL, NULL);
    if(s->sws_ctx==NULL){
    	ms_message("sws_getContext() failed!");
    	return;
    }

	s->frame=0;

	s->ready=TRUE;


}

static void msvidrec_unprepare(RecordedVideo *s){
	int got_output=1;
	int i=0;

	if(s->ready==FALSE){
		return;
	}

	/*Flush the input queue*/
	flushq(&s->rq,0);

	while(got_output){
		/* encode the image */
		s->pkt.data = bit_buffer;
		s->pkt.size = bit_buffer_size;
		s->pkt.pts=s->frame;
		s->pkt.dts=s->frame;
		s->pkt.stream_index=s->av_stream->index;
		s->frame++;
		if((s->pkt.size=avcodec_encode_video(s->av_context, s->pkt.data,s->pkt.size , NULL))<0){
			ms_error("msvideo_recorder: avcodec_encode_video() failed");
			return;
		}

		if(s->pkt.size>0){
			got_output=1;
			/* write the compressed frame in the media file */
			if(av_interleaved_write_frame(s->av_format_ctx, &s->pkt)<0){
				ms_error("msvideo_recorder: av_write_frame() failed");
				return;
			}
			av_free_packet(&s->pkt);
		}else{
			got_output=0;
		}
	}

	/* write the trailer, if any.  the trailer must be written
	 * before you close the CodecContexts open when you wrote the
	 * header; otherwise write_trailer may try to use memory that
	 * was freed on av_codec_close() */
	av_write_trailer(s->av_format_ctx);

	ms_mutex_lock(&ffmpeg_avcodec_open_close_global_mutex);
	avcodec_close(s->av_context);
	ms_mutex_unlock(&ffmpeg_avcodec_open_close_global_mutex);


	/* free the streams */
	for(i=0; i < s->av_format_ctx->nb_streams; i++){
		av_freep(&s->av_format_ctx->streams[i]->codec);
		av_freep(&s->av_format_ctx->streams[i]);
	}

	if (!(s->av_format_out->flags & AVFMT_NOFILE)) {
		/* close the output file */
		avio_close(s->av_format_ctx->pb);
	}

	/* free the stream */
	av_free(s->av_format_ctx);

	sws_freeContext(s->sws_ctx);

	s->ready=FALSE;
}

static void msvidrec_process(MSFilter *f){
	RecordedVideo *s=(RecordedVideo*)f->data;
	mblk_t *om=NULL;

	if(s->run && s->restart){
		ms_filter_lock(f);
		s->restart=FALSE;
		msvidrec_stop(f);
		msvidrec_start(f);
		ms_filter_unlock(f);
	}

	if(s->run){
		ms_filter_lock(f);
		if (f->inputs[0]!=NULL && (om=ms_queue_get(f->inputs[0]))!=0){
			ms_mutex_lock(&s->mutex);
			putq(&s->rq,om);
			ms_mutex_unlock(&s->mutex);
		}
		ms_filter_unlock(f);
	}

	if (f->inputs[0]!=NULL)
		ms_queue_flush(f->inputs[0]);
	if (f->inputs[1]!=NULL)
		ms_queue_flush(f->inputs[1]);
}

void *thread(void* args){
	RecordedVideo *s=(RecordedVideo*)args;
	mblk_t* inm;
	while(s->run==TRUE){
		ms_mutex_lock(&s->mutex);
		if((inm=getq(&s->rq))!=0){
			ms_mutex_unlock(&s->mutex);
			msvidrec_processframe(s,inm);
			freemsg(inm);
		}else{
			ms_mutex_unlock(&s->mutex);
		}
		if(peekq(&s->rq)==NULL){
			sched_yield();
		}
	}
	return NULL;
}

static void msvidrec_processframe(RecordedVideo *obj, mblk_t *inm){
	MSPicture src={0};
	AVFrame frame;

	av_init_packet(&obj->pkt);
	obj->pkt.data = bit_buffer;
	obj->pkt.size = bit_buffer_size;

	if (!obj->ready){
		return;
	}
	
	if(inm==NULL){return;}

	if (ms_yuv_buf_init_from_mblk(&src,inm)==0){
		frame.format = obj->av_context->pix_fmt;
		frame.width  = obj->av_context->width;
		frame.height = obj->av_context->height;
		if(av_image_alloc(frame.data, frame.linesize, obj->av_context->width,
				obj->av_context->height, obj->av_context->pix_fmt, 32)<0){
			ms_error("msvideo_recorder: av_image_alloc() failed!");
			return;
		}

		if(obj->wsize.height!=src.h || obj->wsize.width!=src.w){
			obj->wsize.height=src.h;
			obj->wsize.width=src.w;
			if(obj->autoresize){
				obj->restart=TRUE;
			}
			sws_freeContext(obj->sws_ctx);
			obj->sws_ctx=sws_getContext(src.w,src.h,PIX_FMT_YUV420P,
					obj->av_context->width,obj->av_context->height,obj->av_context->pix_fmt,
					SWS_FAST_BILINEAR,NULL, NULL, NULL);
			if(obj->sws_ctx==NULL){
				ms_error("msvideo_recorder: sws_getContext() failed!");
				return;
			}
		}

		if (sws_scale(obj->sws_ctx, (const uint8_t* const *)&src.planes, src.strides,
				0,src.h,frame.data,frame.linesize)<0){
			ms_error("msvideo_recorder: ms_sws_scale() failed.");
			return;
		}


		frame.pts=obj->frame;
		obj->pkt.pts=obj->frame;
		obj->pkt.dts=obj->frame;
		obj->pkt.stream_index=obj->av_stream->index;
		/* encode the image */
		if((obj->pkt.size=avcodec_encode_video(obj->av_context, &obj->pkt.data[0],obj->pkt.size , &frame))<0){
			ms_error("msvideo_recorder: avcodec_encode_video() failed");
			return;
		}

		if(obj->pkt.size>0){
			/* write the compressed frame in the media file */
			if(av_interleaved_write_frame(obj->av_format_ctx, &obj->pkt)<0){
				ms_error("msvideo_recorder: av_write_frame() failed");
				return;
			}
			av_free_packet(&obj->pkt);
		}

		av_freep(&frame.data[0]);
	}
	obj->frame++;
}

static int msvidrec_set_vsize(MSFilter *f,void *arg){
	RecordedVideo *s=(RecordedVideo*)f->data;
	ms_filter_lock(f);
	s->wsize=*(MSVideoSize*)arg;
	if(s->run==TRUE){
		msvidrec_stop(f);
		msvidrec_start(f);
	}
	ms_filter_unlock(f);
	return 0;
}

static int msvidrec_auto_fit(MSFilter *f, void *arg){
	RecordedVideo *s=(RecordedVideo*)f->data;
		ms_filter_lock(f);
		s->autoresize=*(int*)arg;
		ms_filter_unlock(f);
	return 0;
}

static int msvidrec_show_video(MSFilter *f, void *arg){
	bool_t show=*(bool_t*)arg;
	ms_filter_lock(f);
	if (show==TRUE) {
		msvidrec_start(f);
	}else{
		msvidrec_stop(f);
	}
	ms_filter_unlock(f);
	return 0;
}

static int msvidrec_set_corner(MSFilter *f,void *arg){
	return 0;
}

static int msvidrec_set_scalefactor(MSFilter *f,void *arg){
	return 0;
}

static int msvidrec_enable_mirroring(MSFilter *f,void *arg){
	return 0;
}

static int msvidrec_get_native_window_id(MSFilter *f, void*arg){
	*((unsigned long*)arg)=0;
	return 0;
}

static int msvidrec_set_native_window_id(MSFilter *f, void*arg){
	return 0;
}

static int msvidrec_set_background_color(MSFilter *f,void *arg){
	return 0;
}

static MSFilterMethod methods[]={
	{	MS_FILTER_SET_VIDEO_SIZE	,	msvidrec_set_vsize },
/* methods for compatibility with the MSVideoDisplay interface*/
	{	MS_VIDEO_DISPLAY_SET_LOCAL_VIEW_MODE , msvidrec_set_corner },
	{	MS_VIDEO_DISPLAY_ENABLE_AUTOFIT			, msvidrec_auto_fit },
	{	MS_VIDEO_DISPLAY_ENABLE_MIRRORING		, msvidrec_enable_mirroring },
	{	MS_VIDEO_DISPLAY_GET_NATIVE_WINDOW_ID	, msvidrec_get_native_window_id },
	{	MS_VIDEO_DISPLAY_SET_NATIVE_WINDOW_ID	, msvidrec_set_native_window_id },
	{	MS_VIDEO_DISPLAY_SET_LOCAL_VIEW_SCALEFACTOR	, msvidrec_set_scalefactor },
	{	MS_VIDEO_DISPLAY_SET_BACKGROUND_COLOR    ,  msvidrec_set_background_color},
	{	MS_VIDEO_DISPLAY_SHOW_VIDEO			, msvidrec_show_video },
	{	0	,NULL}
};


MSFilterDesc msvideo_record_desc={
	.id=MS_VIDEO_RECORD,
	.name="MSFileRecord",
	.text=N_("A video display recording to a file"),
	.category=MS_FILTER_OTHER,
	.ninputs=2,
	.noutputs=0,
	.init=msvidrec_init,
	.process=msvidrec_process,
	.uninit=msvidrec_uninit,
	.methods=methods
};


MS_FILTER_DESC_EXPORT(msvideo_record_desc)
