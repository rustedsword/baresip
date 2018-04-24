/**
 * @file gst_video1/gst_video.h  Gstreamer video pipeline -- internal API
 *
 * Copyright (C) 2010 - 2014 Creytiv.com
 * Copyright (C) 2014 Fadeev Alexander
 */


/* Encode */
struct videnc_state;

int gst_video1_encoder_set(struct videnc_state **stp,
			  const struct vidcodec *vc,
			  struct videnc_param *prm, const char *fmtp,
			  videnc_packet_h *pkth, void *arg);
int gst_video1_encode(struct videnc_state *st, bool update,
		     const struct vidframe *frame);

int gst_video1_decoder_set(struct viddec_state **vdsp,
                           const struct vidcodec *vc,
                           const char *fmtp);

int gst_video1_decode(struct viddec_state *st, struct vidframe *frame,
                      bool *intra, bool marker, uint16_t seq, struct mbuf *src);

/* SDP */
uint32_t gst_video1_h264_packetization_mode(const char *fmtp);
int      gst_video1_fmtp_enc(struct mbuf *mb, const struct sdp_format *fmt,
			    bool offer, void *arg);
bool     gst_video1_fmtp_cmp(const char *fmtp1, const char *fmtp2, void *data);
