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

#include "mediastreamer2/mscommon.h"
#include "mediastreamer2/msvideo.h"
#include "mediastreamer2/msfilter.h"
#include "mediastreamer2/msticker.h"
#include "mediastreamer2/mswebcam.h"
#include "mediastreamer2/msfileplayer.h"
#include "ffmpeg_lock.h"
#include "ffmpeg-priv.h"

#if TARGET_OS_IPHONE
#include <CoreGraphics/CGDataProvider.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGBitmapContext.h>
#endif

#include <sys/stat.h>

#ifdef WIN32
#include <fcntl.h>
#include <sys/types.h>
#include <io.h>
#include <stdio.h>
#include <malloc.h>
#endif


#ifndef PACKAGE_DATA_DIR
#define PACKAGE_DATA_DIR "share"
#endif

#ifndef VIDEOFILE
#define VIDEOFILE "prerecordedvideo.mpg"
#endif

static char *def_image=NULL;

void* ms_vidplay_thread(void* args);

static const char *def_video_path=PACKAGE_DATA_DIR "/images/" VIDEOFILE;

typedef struct _VPData{
	char*			path;
	MSVideoSize 	vsize;
	AVFormatContext *av_format_ctx;
	AVCodecContext 	*av_context;
	AVCodec			*av_codec;
	struct SwsContext *sws_ctx;
	uint64_t 		lasttime;
	float 			fps;
	unsigned int 	start_time;
	unsigned int 	last_frame_time;
	int 			th_frame_count;
	int				video_stream_index;
	ms_thread_t 	thread;
	ms_mutex_t 		mutex;
	queue_t 		q;
	bool_t 			run;
	int				queued;
}VPData;

void msvideo_player_open(VPData *d, char *filename){
	int i;

	//Open file
	d->path=filename;
	if(avformat_open_input(&d->av_format_ctx, filename, NULL, NULL)!=0){
		ms_warning("Can't Open %s",filename);
		return;
	}

	// Retrieve stream information
	if(avformat_find_stream_info(d->av_format_ctx,NULL)<0){
		ms_warning("Can't Parse streams");
		return;
	}

	// Find the first video stream
	d->video_stream_index=-1;
	for(i=0; i<d->av_format_ctx->nb_streams; i++)
	    if(d->av_format_ctx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO)
	    {
	    	d->video_stream_index=i;
	        break;
	    }
	if(d->video_stream_index==-1){
		ms_warning("No video streams");
		return;
	}

	// Get a pointer to the codec context for the video stream
	d->av_context=d->av_format_ctx->streams[d->video_stream_index]->codec;

	// Find the decoder for the video stream
	d->av_codec=avcodec_find_decoder(d->av_context->codec_id);
	if(d->av_codec==NULL){
		ms_warning("Codec not found");
		return;
	}

	// Inform the codec that we can handle truncated bitstreams -- i.e.,
	// bitstreams where frame boundaries can fall in the middle of packets
	if(d->av_codec->capabilities & CODEC_CAP_TRUNCATED){
	    d->av_context->flags|=CODEC_FLAG_TRUNCATED;
	}

	// Open codec
	ms_mutex_lock(&ffmpeg_avcodec_open_close_global_mutex);
	if(avcodec_open2(d->av_context, d->av_codec,NULL)<0){
		ms_warning("Couldn't open Codec");
		ms_mutex_unlock(&ffmpeg_avcodec_open_close_global_mutex);
		return;
	}
	ms_mutex_unlock(&ffmpeg_avcodec_open_close_global_mutex);

	d->sws_ctx=sws_getContext(d->av_context->width,d->av_context->height,d->av_context->pix_fmt,
		d->vsize.width,d->vsize.height,PIX_FMT_YUV420P,SWS_FAST_BILINEAR,NULL, NULL, NULL);
	if (d->sws_ctx==NULL) {
		ms_mutex_lock(&ffmpeg_avcodec_open_close_global_mutex);
		avcodec_close(d->av_context);
		ms_mutex_unlock(&ffmpeg_avcodec_open_close_global_mutex);
		return;
	}
}

void msvideo_player_close(VPData *d){
	sws_freeContext(d->sws_ctx);

	// Close the codec
	ms_mutex_lock(&ffmpeg_avcodec_open_close_global_mutex);
	avcodec_close(d->av_context);
	ms_mutex_unlock(&ffmpeg_avcodec_open_close_global_mutex);

	// Close the video file
	avformat_close_input(&d->av_format_ctx);

}

void msvideo_player_init(MSFilter *f){
	av_register_all();

	VPData *d=(VPData*)ms_new(VPData,1);
	d->av_codec=NULL;
	d->av_context=NULL;
	d->av_format_ctx=NULL;
	d->sws_ctx=NULL;
	d->vsize.width=MS_VIDEO_SIZE_CIF_W;
	d->vsize.height=MS_VIDEO_SIZE_CIF_H;
	d->run=FALSE;
	d->queued=0;
	qinit(&d->q);
	ms_mutex_init(&d->mutex, NULL);

	if (def_image==NULL)
		def_image=ms_strdup(def_video_path);
	
	msvideo_player_open(d,def_image);

	d->fps=d->av_context->time_base.num/(d->av_context->time_base.den*1.0);
	d->th_frame_count=-1;
	f->data=d;
}

void msvideo_player_uninit(MSFilter *f){
	VPData *d=(VPData*)f->data;
	msvideo_player_close(d);
	ms_free(d);
}

void msvideo_player_preprocess(MSFilter *f){
	VPData *d=(VPData*)f->data;
	d->run=TRUE;
	if(ms_thread_create(&d->thread, NULL, ms_vidplay_thread,f)<0){
		ms_message("Creating thread failed!");
	}
}

void* ms_vidplay_thread(void* args){
	MSFilter *f=(MSFilter*)args;
	VPData *d=(VPData*)f->data;
	AVPacket packet;
	int frameFinished;
	AVFrame *pFrame;
	MSPicture dest;
	mblk_t *o=NULL;
	int ret;

	while(d->run){
		ms_mutex_lock(&d->mutex);
		if(d->queued < 25){
			ms_mutex_unlock(&d->mutex);
			/*Grab new frame*/
			pFrame=avcodec_alloc_frame();
			if(pFrame==NULL){
				ms_error("msvideo_player: avcodec_alloc_frame() failed!");
			}

			while((ret=av_read_frame(d->av_format_ctx, &packet))>=0){
				if(packet.stream_index==d->video_stream_index){
				  if(avcodec_decode_video2(d->av_context, pFrame, &frameFinished, &packet)<0){
					  ms_error("msvideo_player: avcodec_decode_video2() failed!");
				  }else{
					  // Did we get a video frame?
					  if(frameFinished){
							o=ms_yuv_buf_alloc(&dest, d->vsize.width,d->vsize.height);

							if (sws_scale(d->sws_ctx,(const uint8_t* const *)pFrame->data,pFrame->linesize,
									0,d->av_context->height,dest.planes,dest.strides)<0){
								ms_error("msvideo_player: ms_sws_scale() failed.");
								return NULL;
							}

						  av_free_packet(&packet);
						  break;
					  }
				  }
				}
				av_free_packet(&packet);
			}

			if(ret<0){
				ms_message("msvideo_player: av_read_frame() error or EOF");
				if(av_seek_frame(d->av_format_ctx, -1, 0, AVSEEK_FLAG_ANY | AVSEEK_FLAG_BYTE) < 0){
					ms_error("msvideo_player: av_seek_frame() failed!");
				}
			}


			if(o!=NULL){
				mblk_set_marker_info(o,TRUE);
				ms_mutex_lock(&d->mutex);
				putq(&d->q,o);
				d->queued++;
				ms_mutex_unlock(&d->mutex);
			}
			/*Can't use avcodec_free_frame(). It doesn't exist for all libav*/
			av_free(pFrame);
		}else{
			ms_mutex_unlock(&d->mutex);
			usleep(10000);
		}
	}
	return NULL;
}

void msvideo_player_process(MSFilter *f){
	VPData *d=(VPData*)f->data;
	float elapsed;
	int cur_frame;
	uint32_t curtime=f->ticker->time;
	mblk_t *o=NULL;
	uint32_t timestamp;



	if (d->th_frame_count==-1){
		d->start_time=curtime;
		d->th_frame_count=0;
	}
	elapsed=((float)(curtime-d->start_time))/1000.0;
	cur_frame=elapsed*d->fps;

	if (cur_frame>=d->th_frame_count){
		ms_filter_lock(f);
		ms_mutex_lock(&d->mutex);
		if ((o=getq(&d->q))!=0){
			timestamp=f->ticker->time*90;/* rtp uses a 90000 Hz clockrate for video*/
			mblk_set_timestamp_info(o,timestamp);
			ms_queue_put(f->outputs[0],o);
			d->th_frame_count++;
			d->last_frame_time=curtime;
			d->queued--;
		}
		ms_mutex_unlock(&d->mutex);
		ms_filter_unlock(f);
	}
}

void msvideo_player_postprocess(MSFilter *f){
	VPData *d=(VPData*)f->data;

	d->run=FALSE;
	ms_thread_join(d->thread,NULL);
}

static int msvideo_player_set_fps(MSFilter *f, void *arg){
	VPData *d=(VPData*)f->data;
	d->fps=*((float*)arg);
	return 0;
}

static int msvideo_player_get_fps(MSFilter *f, void *arg){
	VPData *d=(VPData*)f->data;
	*((float*)arg) = d->fps;
	return 0;
}

int msvideo_player_set_vsize(MSFilter *f, void* data){
	VPData *d=(VPData*)f->data;
	d->vsize=*(MSVideoSize*)data;
	ms_filter_lock(f);
	msvideo_player_close(d);
	msvideo_player_open(d,d->path);
	ms_filter_unlock(f);
	return 0;
}

int msvideo_player_get_vsize(MSFilter *f, void* data){
	VPData *d=(VPData*)f->data;
	*(MSVideoSize*)data=d->vsize;
	return 0;
}

int msvideo_player_get_pix_fmt(MSFilter *f, void *data){
	*(MSPixFmt*)data=MS_YUV420P;
	return 0;
}

MSFilterMethod msvideo_player_methods[]={
	{	MS_FILTER_SET_FPS,	msvideo_player_set_fps	},
	{	MS_FILTER_GET_FPS,	msvideo_player_get_fps	},
	{	MS_FILTER_SET_VIDEO_SIZE, msvideo_player_set_vsize },
	{	MS_FILTER_GET_VIDEO_SIZE, msvideo_player_get_vsize },
	{	MS_FILTER_GET_PIX_FMT, msvideo_player_get_pix_fmt },
	{	0,0 }
};

MSFilterDesc msvideo_player_desc={
	MS_VIDEO_PLAYER,
	"MSVideoPlayer",
	N_("A filter that outputs a video file."),
	MS_FILTER_OTHER,
	NULL,
	0,
	1,
	msvideo_player_init,
	msvideo_player_preprocess,
	msvideo_player_process,
	msvideo_player_postprocess,
	msvideo_player_uninit,
	msvideo_player_methods
};

MS_FILTER_DESC_EXPORT(msvideo_player_desc)

static void msvideo_player_detect(MSWebCamManager *obj);

static void msvideo_player_cinit(MSWebCam *cam){
	cam->name=ms_strdup("Video File");
}


static MSFilter *msvideo_player_create_reader(MSWebCam *obj){
	return ms_filter_new_from_desc(&msvideo_player_desc);
}

MSWebCamDesc video_player_desc={
	"VideoPlayer",
	&msvideo_player_detect,
	&msvideo_player_cinit,
	&msvideo_player_create_reader,
	NULL
};

static void msvideo_player_detect(MSWebCamManager *obj){
	MSWebCam *cam=ms_web_cam_new(&video_player_desc);
	ms_web_cam_manager_add_cam(obj,cam);
}
