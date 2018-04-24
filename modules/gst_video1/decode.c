/**
 * @file gst_video/decode.c  Video codecs using Gstreamer video pipeline
 *
 * Copyright (C) 2018 - rustedsword
 */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <stdatomic.h>
#include "gst_video.h"

#define PACKET_SIZE 1300

struct viddec_state
{
        GstElement *pipeline;       /* Gstreamer Pipeline Bin */

        GstAppSink *sink;           /* Gstreamer AppSink Element */
        GstPad *sink_pad;           /* AppSink's sink pad */
        gulong sink_probe_id;       /* Probe id for AppSink's sink pad */

        GstAppSrc *source;          /* Gstreamer AppSrc Element */

        GstSample *sample;          /* Sample with decoded picture */
        atomic_int sample_counter;  /* Decoded picture counter */
        GstVideoInfo v_info;        /* Contains decoded picture properties */
        GstVideoFrame v_frame;      /* Contains decoded picture properties */
        bool buffer_mapped;         /* Indicates that decoded picture is mapped to v_frame */

        GstBuffer *encoded_buf;     /* Buffer for incoming encoded video */

        bool playing;               /* State of pipeline */

        bool got_keyframe;          /* if true then we are received SPS Nal Unit */
        bool frag;                  /* If true then we are assembilmg Fragmentation Unit */
        uint16_t prev_seq;          /* Previous rtp packet sequence number */

        const struct vidcodec *vc;  /* Video codec structure */
};

static void destructor(void *arg) {
        struct viddec_state *st = (struct viddec_state*)arg;
        if(st->pipeline){
                gst_element_set_state(st->pipeline, GST_STATE_NULL);
                gst_object_unref(st->pipeline);
        }

        if(st->sink_pad) {
                if(st->sink_probe_id)
                        gst_pad_remove_probe(st->sink_pad, st->sink_probe_id);

                gst_object_unref(st->sink_pad);
        }

        if(st->sink)
                gst_object_unref(st->sink);

        if(st->source)
                gst_object_unref(st->source);

        if(st->sample) {
                if(st->buffer_mapped)
                        gst_video_frame_unmap(&st->v_frame);

                gst_sample_unref(st->sample);
        }

        if(st->encoded_buf)
                gst_buffer_unref(st->encoded_buf);
}

static GstPadProbeReturn query_parser(GstPad *pad, GstPadProbeInfo *info, gpointer user_data){
        (void)user_data;
        (void)pad;
        GstQuery *query;
        query = gst_pad_probe_info_get_query(info);
        if(query->type == GST_QUERY_ALLOCATION)
                gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);

        return GST_PAD_PROBE_OK;
}

static void vframe_map(GstVideoFrame *v_frame, struct vidframe *frame) {
        frame->size.w = GST_VIDEO_FRAME_WIDTH(v_frame);
        frame->size.h = GST_VIDEO_FRAME_HEIGHT(v_frame);

        frame->linesize[0] = GST_VIDEO_FRAME_PLANE_STRIDE(v_frame, 0);
        frame->linesize[1] = GST_VIDEO_FRAME_PLANE_STRIDE(v_frame, 1);
        frame->linesize[2] = GST_VIDEO_FRAME_PLANE_STRIDE(v_frame, 2);

        frame->data[0] = GST_VIDEO_FRAME_PLANE_DATA(v_frame, 0);
        frame->data[1] = GST_VIDEO_FRAME_PLANE_DATA(v_frame, 1);
        frame->data[2] = GST_VIDEO_FRAME_PLANE_DATA(v_frame, 2);

        frame->fmt = VID_FMT_YUV420P;
}

static GstFlowReturn new_sample(GstAppSink *appsink, gpointer data) {
        (void)appsink;
        struct viddec_state *st = (struct viddec_state *)data;
        atomic_fetch_add(&st->sample_counter, 1);
        return GST_FLOW_OK;
}

static int get_frame(struct viddec_state *st, struct vidframe *frame) {
        if(atomic_load(&st->sample_counter) <= 0)
                return 0;

        atomic_fetch_sub(&st->sample_counter, 1);

        if(st->sample) {
                if(st->buffer_mapped) {
                        gst_video_frame_unmap(&st->v_frame);
                        st->buffer_mapped = false;
                }
                gst_sample_unref(st->sample);
        }

        st->sample = gst_app_sink_pull_sample(st->sink);
        if(!st->sample)
                return ENOBUFS;

        GstBuffer *buf = gst_sample_get_buffer(st->sample);
        if(!buf)
                return ENOBUFS;

        GstCaps *caps = gst_sample_get_caps(st->sample);
        if(!caps)
                return ENOBUFS;

        gst_video_info_from_caps(&st->v_info, caps);
        gst_video_frame_map(&st->v_frame, &st->v_info, buf, GST_MAP_READ);
        st->buffer_mapped = true;

        vframe_map(&st->v_frame, frame);
        return 0;
}

static int append_data_to_buffer(GstBuffer **buf, gconstpointer data, gsize datasize) {
        gsize offset, maxsize, newsize, currsize;

        if(!GST_IS_BUFFER(*buf)) {
                *buf = gst_buffer_new_and_alloc(PACKET_SIZE * 60);
                if(!(*buf))
                        goto err;

                gst_buffer_set_size(*buf, 0);
        }

        currsize = gst_buffer_get_sizes(*buf, &offset, &maxsize);
        newsize = currsize + datasize;

        while( (maxsize - offset) < newsize ) {
                re_printf("Current buffer with size:%lu is not large enough to hold data with size:%lu, allocating another one\n", (maxsize - offset), newsize);
                GstBuffer * new_buf = gst_buffer_new_and_alloc(PACKET_SIZE * 60);
                if(!new_buf)
                        goto err;

                gst_buffer_set_size(new_buf, 0);
                *buf = gst_buffer_append(*buf, new_buf);

                gst_buffer_get_sizes(*buf, &offset, &maxsize);
        }

        gst_buffer_set_size(*buf, newsize);
        currsize = gst_buffer_fill(*buf, currsize, data, datasize);
        if(currsize != datasize) {
                re_printf("Filled bytes %lu do not match requested %lu \n", currsize, datasize);
                goto err;
        }

#if 0
        re_printf("Adding data with size %lu to buffer. Current buffer size now is %lu\n", size, newsize);
#endif

        return 0;
err:
        if(GST_IS_BUFFER(*buf)) {
                gst_buffer_unref(*buf);
        }
        *buf = NULL;
        return ENOMEM;
}

static inline int16_t seq_diff(uint16_t x, uint16_t y)
{
        return (int16_t)(y - x);
}

static inline void buffer_empty(GstBuffer **buf) {
        if(*buf) {
                gst_buffer_unref(*buf);
                *buf = NULL;
        }
}

static int parse_h264(struct viddec_state *st, bool *intra, bool marker, uint16_t seq, struct mbuf *src) {
        int err;
        struct h264_hdr h264_hdr;
        static const uint8_t nal_seq[] = {0, 0, 0, 1};

        *intra = false;

        err = h264_hdr_decode(&h264_hdr, src);
        if (err)
                goto out;

        if (h264_hdr.f) {
                info("gst_video1: H264 forbidden bit set!\n");
                err = EBADMSG;
                goto out;
        }

        /* If we already have some data in buffer, but packet with wrong rtp sequence received */
        if (seq_diff(st->prev_seq, seq) != 1 && st->encoded_buf) {
                printf("gst_video1: lost fragments detected (prev seq: %u, current seq: %u)\n", st->prev_seq, seq);
                err = EPROTO;
                goto out;
        }

        /* if we are in process of assembling frame from FU-A Packets but receving non FU-A packet*/
        if (st->frag && h264_hdr.type != H264_NAL_FU_A) {
                printf("gst_video1: lost fragments; discarding previous NAL\n");
                st->frag = false;
                buffer_empty(&st->encoded_buf);
        }

        if (1 <= h264_hdr.type && h264_hdr.type <= 23) {
                if (h264_is_keyframe(h264_hdr.type))
                        *intra = true;

                --src->pos;

                /* prepend H.264 NAL start sequence */
                err = append_data_to_buffer(&st->encoded_buf, nal_seq, sizeof(nal_seq));
                if(!err)
                        err = append_data_to_buffer(&st->encoded_buf, mbuf_buf(src), mbuf_get_left(src));

                if(err)
                        return err;

        } else if (H264_NAL_FU_A == h264_hdr.type) {
                struct h264_fu fu;
                err = h264_fu_hdr_decode(&fu, src);
                if(err)
                        goto out;

                if(fu.s) {
                        uint8_t nal_header;
                        nal_header = h264_hdr.f<<7 | h264_hdr.nri<<5 | fu.type<<0;

                        err = append_data_to_buffer(&st->encoded_buf, nal_seq, sizeof(nal_seq));
                        if(!err)
                                err = append_data_to_buffer(&st->encoded_buf, &nal_header, 1);

                        if(err)
                                goto out;

                        if(h264_is_keyframe(fu.type))
                                *intra = true;

                        st->frag = true;
                } else {
                        if (!st->frag) {
                                printf("gst_video1: ignoring fragment (nal=%u)\n", fu.type);
                                err = EPROTO;
                                goto out;
                        }
                }

                err = append_data_to_buffer(&st->encoded_buf,  mbuf_buf(src), mbuf_get_left(src));
                if(err)
                        goto out;

                /*
                 * This is against standard: single FU-A can not
                 * have start and end bits set in single packet.
                 * But some payloaders actually doing this
                 */

                if(fu.e)
                        st->frag = false;

        } else {
                warning("gst_video1: unknown NAL type %u\n", h264_hdr.type);
                return EBADMSG;
        }

        /* Store current sequence */
        st->prev_seq = seq;

        if(*intra)
                st->got_keyframe = true;

        if(!marker)
                return 0;

        if(st->frag) {
                err = EPROTO;
                goto out;
        }

        if(!st->got_keyframe) {
                info("Waiting for keyframe\n");
                err = EPROTO;
                goto out;
        }

        GstFlowReturn ret = gst_app_src_push_buffer(st->source, st->encoded_buf);
        st->encoded_buf = NULL;

        if(ret != GST_FLOW_OK)
                return EINVAL;

        return 0;
out:
        buffer_empty(&st->encoded_buf);
        st->frag = false;
        return err;
}

static int gstreamer_init (struct viddec_state *st);

static int gstreamer_restart(struct viddec_state *st) {
        const struct vidcodec *vc = st->vc;
        destructor(st);
        *st = (struct viddec_state){0};
        st->vc = vc;
        return gstreamer_init(st);
}

int gst_video1_decode(struct viddec_state *st, struct vidframe *frame, bool *intra, bool marker, uint16_t seq, struct mbuf *src) {
        int err;
        if(!st->playing) {
                warning("gst_video1: pipline is broken, trying to recover\n");
                err = gstreamer_restart(st);
                if(err)
                        return err;
        }

        err = parse_h264(st, intra, marker, seq, src);
        if(err)
                return err;

        return get_frame(st, frame);
}

static GstBusSyncReply bus_sync_handler_cb(GstBus *bus, GstMessage *msg,
                                           struct viddec_state *st)
{
        (void)bus;

        if ((GST_MESSAGE_TYPE (msg)) == GST_MESSAGE_ERROR) {
                GError *err = NULL;
                gchar *dbg_info = NULL;
                gst_message_parse_error (msg, &err, &dbg_info);
                warning("gst_video: Error: %d(%m) message=%s\n",
                        err->code, err->code, err->message);
                warning("gst_video: Debug: %s\n", dbg_info);
                g_error_free (err);
                g_free (dbg_info);

                /* mark pipeline as broken */
                st->playing = false;
        }

        gst_message_unref(msg);
        return GST_BUS_DROP;
}

static int gstreamer_init (struct viddec_state *st) {
        int err;
        GstCaps * caps;
        GstBus *bus;
        const char * pipeline_string = "appsrc name=source is-live=true do-timestamp=true block=false ! "
                                       "h264parse ! "
                                       "avdec_h264 ! "
                                       "appsink name=sink drop=true sync=false";

        atomic_init(&st->sample_counter, 0);

        GError *gerror = NULL;
        st->pipeline = gst_parse_launch(pipeline_string, &gerror);
        if (gerror) {
                warning("gst_video: launch error: %d: %s: %s\n",
                        gerror->code, gerror->message, pipeline_string);
                err = gerror->code;
                g_error_free(gerror);
                return err;
        }

        /* AppSrc configuration */
        st->source = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(st->pipeline), "source"));
        if(!st->source)
                return EINVAL;

        caps = gst_caps_new_simple ("video/x-h264",
                                    "stream-format", G_TYPE_STRING, "byte-stream",
                                    "alignment", G_TYPE_STRING, "au",
                                    NULL);
        gst_app_src_set_caps(st->source, caps);
        gst_caps_unref(caps);

        /* AppSink configuration */
        st->sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(st->pipeline), "sink"));
        if(!st->sink)
                return EINVAL;

        caps = gst_caps_new_simple ("video/x-raw",
                                    "format", G_TYPE_STRING, "I420",
                                    NULL);
        gst_app_sink_set_caps(st->sink, caps);
        gst_caps_unref(caps);

        st->sink_pad = gst_element_get_static_pad(GST_ELEMENT(st->sink), "sink");
        if(!st->sink_pad)
                return EINVAL;

        st->sink_probe_id = gst_pad_add_probe(st->sink_pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, query_parser, NULL, NULL);
        if(!st->sink_probe_id)
                return EINVAL;

        GstAppSinkCallbacks appsink_callbacks = { NULL, NULL, new_sample };
        gst_app_sink_set_callbacks (st->sink, &appsink_callbacks, st, NULL);

        /* Bus Watcher */
        bus = gst_pipeline_get_bus(GST_PIPELINE(st->pipeline));
        gst_bus_set_sync_handler(bus, (GstBusSyncHandler)bus_sync_handler_cb,
                                 st, NULL);
        gst_object_unref(GST_OBJECT(bus));

        err = gst_element_set_state(st->pipeline, GST_STATE_PLAYING);
        if (GST_STATE_CHANGE_FAILURE == err) {
                g_warning("gst_video1: set state returned GST_STATE_CHANGE_FAILURE\n");
                return EPROTO;
        }

        st->encoded_buf = NULL;
        st->playing = true;
        st->got_keyframe = false;
        st->frag = false;
        st->buffer_mapped = false;
        return 0;
}

int gst_video1_decoder_set(struct viddec_state **vdsp, const struct vidcodec *vc, const char *fmtp) {

        printf("Gst init\n");

        struct viddec_state *st;
        int err = 0;

        if (!vdsp || !vc)
                return EINVAL;

        if (*vdsp)
                return 0;

        (void)fmtp;

        st = mem_zalloc(sizeof(*st), destructor);
        if (!st)
                return ENOMEM;

        st->vc = vc;
        err = gstreamer_init(st);
        if (err) {
                warning("gst_video1: %s: could not init decoder\n", vc->name);
                goto out;
        }
out:
        if (err)
                mem_deref(st);
        else
                *vdsp = st;

        return err;
}
