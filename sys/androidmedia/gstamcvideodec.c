/*
 * Initially based on gst-omx/omx/gstomxvideodec.c
 *
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 *
 * Copyright (C) 2012, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * Copyright (C) 2012, Rafaël Carré <funman@videolanorg>
 *
 * Copyright (C) 2014-2015, Collabora Ltd.
 *   Author: Matthieu Bouron <matthieu.bouron@gcollabora.com>
 *
 * Copyright (C) 2015, Edward Hervey
 *   Author: Edward Hervey <bilboed@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/gl/gl.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <string.h>

#ifdef HAVE_ORC
#include <orc/orc.h>
#else
#define orc_memcpy memcpy
#endif

#include "gstamcvideodec.h"
#include "gstamc-constants.h"
#include "gstamc2dtexturerenderer.h"

GST_DEBUG_CATEGORY_STATIC (gst_amc_video_dec_debug_category);
#define GST_CAT_DEFAULT gst_amc_video_dec_debug_category

#define GST_VIDEO_DECODER_ERROR_FROM_ERROR(el, err) G_STMT_START { \
  gchar *__dbg = g_strdup (err->message);                               \
  GstVideoDecoder *__dec = GST_VIDEO_DECODER (el);                      \
  GST_WARNING_OBJECT (el, "error: %s", __dbg);                          \
  _gst_video_decoder_error (__dec, 1,                                   \
    err->domain, err->code,                                             \
    NULL, __dbg, __FILE__, GST_FUNCTION, __LINE__);                     \
  g_clear_error (&err); \
} G_STMT_END

/* Assume the device is able to decode at 30fps by default */
#define GST_AMC_VIDEO_DEC_ON_FRAME_AVAILABLE_DEFAULT_TIMEOUT (33 * G_TIME_SPAN_MILLISECOND)

#if GLIB_SIZEOF_VOID_P == 8
#define JLONG_TO_GST_AMC_VIDEO_DEC(value) (GstAmcVideoDec *)(value)
#define GST_AMC_VIDEO_DEC_TO_JLONG(value) (jlong)(value)
#else
#define JLONG_TO_GST_AMC_VIDEO_DEC(value) (GstAmcVideoDec *)(jint)(value)
#define GST_AMC_VIDEO_DEC_TO_JLONG(value) (jlong)(jint)(value)
#endif

typedef struct _BufferIdentification BufferIdentification;
struct _BufferIdentification
{
  guint64 timestamp;
};

static BufferIdentification *
buffer_identification_new (GstClockTime timestamp)
{
  BufferIdentification *id = g_slice_new (BufferIdentification);

  id->timestamp = timestamp;

  return id;
}

static void
buffer_identification_free (BufferIdentification * id)
{
  g_slice_free (BufferIdentification, id);
}

/* prototypes */
static void gst_amc_video_dec_finalize (GObject * object);

static GstStateChangeReturn
gst_amc_video_dec_change_state (GstElement * element,
    GstStateChange transition);

static gboolean gst_amc_video_dec_open (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_close (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_start (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_stop (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static gboolean gst_amc_video_dec_flush (GstVideoDecoder * decoder);
static GstFlowReturn gst_amc_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);
static GstFlowReturn gst_amc_video_dec_finish (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_decide_allocation (GstVideoDecoder * bdec,
    GstQuery * query);

static GstFlowReturn gst_amc_video_dec_drain (GstAmcVideoDec * self);
static gboolean gst_amc_video_dec_check_codec_config (GstAmcVideoDec * self);
static void
gst_amc_video_dec_on_frame_available (JNIEnv * env, jobject thiz,
    long long context, jobject surfaceTexture);

enum
{
  PROP_0
};

/* class initialization */

static void gst_amc_video_dec_class_init (GstAmcVideoDecClass * klass);
static void gst_amc_video_dec_init (GstAmcVideoDec * self);
static void gst_amc_video_dec_base_init (gpointer g_class);

static GstVideoDecoderClass *parent_class = NULL;

GType
gst_amc_video_dec_get_type (void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstAmcVideoDecClass),
      gst_amc_video_dec_base_init,
      NULL,
      (GClassInitFunc) gst_amc_video_dec_class_init,
      NULL,
      NULL,
      sizeof (GstAmcVideoDec),
      0,
      (GInstanceInitFunc) gst_amc_video_dec_init,
      NULL
    };

    _type = g_type_register_static (GST_TYPE_VIDEO_DECODER, "GstAmcVideoDec",
        &info, 0);

    GST_DEBUG_CATEGORY_INIT (gst_amc_video_dec_debug_category, "amcvideodec", 0,
        "Android MediaCodec video decoder");

    g_once_init_leave (&type, _type);
  }
  return type;
}

static const gchar *
caps_to_mime (GstCaps * caps)
{
  GstStructure *s;
  const gchar *name;

  s = gst_caps_get_structure (caps, 0);
  if (!s)
    return NULL;

  name = gst_structure_get_name (s);

  if (strcmp (name, "video/mpeg") == 0) {
    gint mpegversion;

    if (!gst_structure_get_int (s, "mpegversion", &mpegversion))
      return NULL;

    if (mpegversion == 4)
      return "video/mp4v-es";
    else if (mpegversion == 1 || mpegversion == 2)
      return "video/mpeg2";
  } else if (strcmp (name, "video/x-h263") == 0) {
    return "video/3gpp";
  } else if (strcmp (name, "video/x-h264") == 0) {
    return "video/avc";
  } else if (strcmp (name, "video/x-vp8") == 0) {
    return "video/x-vnd.on2.vp8";
  } else if (strcmp (name, "video/x-divx") == 0) {
    return "video/mp4v-es";
  }

  return NULL;
}

static void
gst_amc_video_dec_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstAmcVideoDecClass *amcvideodec_class = GST_AMC_VIDEO_DEC_CLASS (g_class);
  const GstAmcCodecInfo *codec_info;
  GstPadTemplate *templ;
  GstCaps *sink_caps, *src_caps, *all_src_caps;
  gchar *longname;

  codec_info =
      g_type_get_qdata (G_TYPE_FROM_CLASS (g_class), gst_amc_codec_info_quark);
  /* This happens for the base class and abstract subclasses */
  if (!codec_info)
    return;

  amcvideodec_class->codec_info = codec_info;

  gst_amc_codec_info_to_caps (codec_info, &sink_caps, &src_caps);

  all_src_caps =
      gst_caps_from_string (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
      (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, "RGBA"));

  if (codec_info->gl_output_only) {
    gst_caps_unref (src_caps);
  } else {
    gst_caps_append (all_src_caps, src_caps);
  }

  /* Add pad templates */
  templ =
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink_caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (sink_caps);

  templ =
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, all_src_caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (all_src_caps);

  longname = g_strdup_printf ("Android MediaCodec %s", codec_info->name);
  gst_element_class_set_metadata (element_class,
      codec_info->name,
      "Codec/Decoder/Video",
      longname, "Sebastian Dröge <sebastian.droege@collabora.co.uk>");
  g_free (longname);
}

static void
gst_amc_video_dec_class_init (GstAmcVideoDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *videodec_class = GST_VIDEO_DECODER_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->finalize = gst_amc_video_dec_finalize;

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_change_state);

  videodec_class->start = GST_DEBUG_FUNCPTR (gst_amc_video_dec_start);
  videodec_class->stop = GST_DEBUG_FUNCPTR (gst_amc_video_dec_stop);
  videodec_class->open = GST_DEBUG_FUNCPTR (gst_amc_video_dec_open);
  videodec_class->close = GST_DEBUG_FUNCPTR (gst_amc_video_dec_close);
  videodec_class->flush = GST_DEBUG_FUNCPTR (gst_amc_video_dec_flush);
  videodec_class->set_format = GST_DEBUG_FUNCPTR (gst_amc_video_dec_set_format);
  videodec_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_handle_frame);
  videodec_class->finish = GST_DEBUG_FUNCPTR (gst_amc_video_dec_finish);
  videodec_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_decide_allocation);
}

static void
gst_amc_video_dec_init (GstAmcVideoDec * self)
{
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);
  gst_video_decoder_set_needs_format (GST_VIDEO_DECODER (self), TRUE);

  g_mutex_init (&self->drain_lock);
  g_cond_init (&self->drain_cond);

  g_mutex_init (&self->on_frame_available_lock);
  g_cond_init (&self->on_frame_available_cond);
  self->on_frame_available = FALSE;
}

static gboolean
gst_amc_video_dec_open (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (decoder);
  GstAmcVideoDecClass *klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "Opening decoder");

  self->codec = gst_amc_codec_new (klass->codec_info->name, &err);
  if (!self->codec) {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }
  self->codec_config = AMC_CODEC_CONFIG_NONE;

  self->started = FALSE;
  self->flushing = TRUE;

  GST_DEBUG_OBJECT (self, "Opened decoder");

  return TRUE;
}

static gboolean
gst_amc_video_dec_close (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Closing decoder");

  if (self->codec) {
    GError *err = NULL;

    gst_amc_codec_release (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);

    gst_amc_codec_free (self->codec);
  }
  self->codec = NULL;
  self->codec_config = AMC_CODEC_CONFIG_NONE;

  if (self->surface) {
    gst_object_unref (self->surface);
    self->surface = NULL;
  }

  self->started = FALSE;
  self->flushing = TRUE;
  self->downstream_supports_gl = FALSE;

  GST_DEBUG_OBJECT (self, "Closed decoder");

  return TRUE;
}

static void
gst_amc_video_dec_finalize (GObject * object)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (object);

  g_mutex_clear (&self->drain_lock);
  g_cond_clear (&self->drain_cond);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstStateChangeReturn
gst_amc_video_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstAmcVideoDec *self;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GError *err = NULL;

  g_return_val_if_fail (GST_IS_AMC_VIDEO_DEC (element),
      GST_STATE_CHANGE_FAILURE);
  self = GST_AMC_VIDEO_DEC (element);

  GST_DEBUG_OBJECT (element, "changing state: %s => %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      self->downstream_flow_ret = GST_FLOW_OK;
      self->draining = FALSE;
      self->started = FALSE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->flushing = TRUE;
      if (self->started) {
        gst_amc_codec_flush (self->codec, &err);
        if (err)
          GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      }
      g_mutex_lock (&self->drain_lock);
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
      g_mutex_unlock (&self->drain_lock);
      break;
    default:
      break;
  }

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->downstream_flow_ret = GST_FLOW_FLUSHING;
      self->started = FALSE;

      GST_DEBUG_OBJECT (element, "Freeing GL context: %" GST_PTR_FORMAT,
          self->gl_context);
      if (self->gl_context) {
        gst_object_unref (self->gl_context);
        self->gl_context = NULL;
      }

      GST_DEBUG_OBJECT (element, "Freeing GL renderer: %p", self->renderer);
      if (self->renderer) {
        gst_amc_2d_texture_renderer_free (self->renderer);
        self->renderer = NULL;
      }

      break;
    default:
      break;
  }

  return ret;
}

#define MAX_FRAME_DIST_TIME  (5 * GST_SECOND)
#define MAX_FRAME_DIST_FRAMES (100)

static GstVideoCodecFrame *
_find_nearest_frame (GstAmcVideoDec * self, GstClockTime reference_timestamp)
{
  GList *l, *best_l = NULL;
  GList *finish_frames = NULL;
  GstVideoCodecFrame *best = NULL;
  guint64 best_timestamp = 0;
  guint64 best_diff = G_MAXUINT64;
  BufferIdentification *best_id = NULL;
  GList *frames;

  frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (self));

  for (l = frames; l; l = l->next) {
    GstVideoCodecFrame *tmp = l->data;
    BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
    guint64 timestamp, diff;

    /* This happens for frames that were just added but
     * which were not passed to the component yet. Ignore
     * them here!
     */
    if (!id)
      continue;

    timestamp = id->timestamp;

    if (timestamp > reference_timestamp)
      diff = timestamp - reference_timestamp;
    else
      diff = reference_timestamp - timestamp;

    if (best == NULL || diff < best_diff) {
      best = tmp;
      best_timestamp = timestamp;
      best_diff = diff;
      best_l = l;
      best_id = id;

      /* For frames without timestamp we simply take the first frame */
      if ((reference_timestamp == 0 && timestamp == 0) || diff == 0)
        break;
    }
  }

  if (best_id) {
    for (l = frames; l && l != best_l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;
      BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
      guint64 diff_time, diff_frames;

      if (id->timestamp > best_timestamp)
        break;

      if (id->timestamp == 0 || best_timestamp == 0)
        diff_time = 0;
      else
        diff_time = best_timestamp - id->timestamp;
      diff_frames = best->system_frame_number - tmp->system_frame_number;

      if (diff_time > MAX_FRAME_DIST_TIME
          || diff_frames > MAX_FRAME_DIST_FRAMES) {
        finish_frames =
            g_list_prepend (finish_frames, gst_video_codec_frame_ref (tmp));
      }
    }
  }

  if (finish_frames) {
    g_warning ("%s: Too old frames, bug in decoder -- please file a bug",
        GST_ELEMENT_NAME (self));
    for (l = finish_frames; l; l = l->next) {
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), l->data);
    }
  }

  if (best)
    gst_video_codec_frame_ref (best);

  g_list_foreach (frames, (GFunc) gst_video_codec_frame_unref, NULL);
  g_list_free (frames);

  return best;
}

static gboolean
gst_amc_video_dec_check_codec_config (GstAmcVideoDec * self)
{
  gboolean ret = (self->codec_config == AMC_CODEC_CONFIG_NONE
      || (self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE
          && self->downstream_supports_gl)
      || (self->codec_config == AMC_CODEC_CONFIG_WITHOUT_SURFACE
          && !self->downstream_supports_gl));

  if (!ret) {
    GST_ERROR_OBJECT
        (self,
        "Codec configuration (%d) is not compatible with downstream which %s support GL output",
        self->codec_config, self->downstream_supports_gl ? "does" : "does not");
  }

  return ret;
}

static gboolean
gst_amc_video_dec_set_src_caps (GstAmcVideoDec * self, GstAmcFormat * format)
{
  GstVideoCodecState *output_state;
  const gchar *mime;
  gint ret;
  gint color_format, width, height;
  gint stride, slice_height;
  gint crop_left, crop_right;
  gint crop_top, crop_bottom;
  GstVideoFormat gst_format;
  GstAmcVideoDecClass *klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);
  GError *err = NULL;

  if (!gst_amc_format_get_int (format, "color-format", &color_format, &err) ||
      !gst_amc_format_get_int (format, "width", &width, &err) ||
      !gst_amc_format_get_int (format, "height", &height, &err)) {
    GST_ERROR_OBJECT (self, "Failed to get output format metadata: %s",
        err->message);
    g_clear_error (&err);
    return FALSE;
  }

  if (!gst_amc_format_get_int (format, "stride", &stride, &err) ||
      !gst_amc_format_get_int (format, "slice-height", &slice_height, &err)) {
    GST_ERROR_OBJECT (self, "Failed to get stride and slice-height: %s",
        err->message);
    g_clear_error (&err);
    return FALSE;
  }

  if (!gst_amc_format_get_int (format, "crop-left", &crop_left, &err) ||
      !gst_amc_format_get_int (format, "crop-right", &crop_right, &err) ||
      !gst_amc_format_get_int (format, "crop-top", &crop_top, &err) ||
      !gst_amc_format_get_int (format, "crop-bottom", &crop_bottom, &err)) {
    GST_ERROR_OBJECT (self, "Failed to get crop rectangle: %s", err->message);
    g_clear_error (&err);
    return FALSE;
  }

  if (width == 0 || height == 0) {
    GST_ERROR_OBJECT (self, "Height or width not set");
    return FALSE;
  }

  if (crop_bottom)
    height = height - (height - crop_bottom - 1);
  if (crop_top)
    height = height - crop_top;

  if (crop_right)
    width = width - (width - crop_right - 1);
  if (crop_left)
    width = width - crop_left;

  mime = caps_to_mime (self->input_state->caps);
  if (!mime) {
    GST_ERROR_OBJECT (self, "Failed to convert caps to mime");
    return FALSE;
  }

  gst_format =
      gst_amc_color_format_to_video_format (klass->codec_info, mime,
      color_format);

  if (self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE) {
    gst_format = GST_VIDEO_FORMAT_RGBA;
  }

  if (gst_format == GST_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR_OBJECT (self, "Unknown color format 0x%08x", color_format);
    return FALSE;
  }

  output_state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
      gst_format, width, height, self->input_state);

  if (self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE) {
    if (output_state->caps)
      gst_caps_unref (output_state->caps);
    output_state->caps = gst_video_info_to_caps (&output_state->info);
    gst_caps_set_features (output_state->caps, 0,
        gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));
  }


  self->format = gst_format;
  self->width = width;
  self->height = height;
  if (!gst_amc_color_format_info_set (&self->color_format_info,
          klass->codec_info, mime, color_format, width, height, stride,
          slice_height, crop_left, crop_right, crop_top, crop_bottom)) {
    GST_ERROR_OBJECT (self, "Failed to set up GstAmcColorFormatInfo");
    return FALSE;
  }

  GST_DEBUG_OBJECT (self,
      "Color format info: {color_format=%d (0x%08x), width=%d, height=%d, "
      "stride=%d, slice-height=%d, crop-left=%d, crop-top=%d, "
      "crop-right=%d, crop-bottom=%d, frame-size=%d}",
      self->color_format_info.color_format,
      self->color_format_info.color_format, self->color_format_info.width,
      self->color_format_info.height, self->color_format_info.stride,
      self->color_format_info.slice_height, self->color_format_info.crop_left,
      self->color_format_info.crop_top, self->color_format_info.crop_right,
      self->color_format_info.crop_bottom, self->color_format_info.frame_size);

  ret = gst_video_decoder_negotiate (GST_VIDEO_DECODER (self));

  gst_video_codec_state_unref (output_state);
  self->input_state_changed = FALSE;

  return ret;
}

static gboolean
gst_amc_video_dec_fill_buffer (GstAmcVideoDec * self, gint idx,
    const GstAmcBufferInfo * buffer_info, GstBuffer * outbuf)
{
  GstAmcBuffer *buf;
  GstVideoCodecState *state =
      gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
  GstVideoInfo *info = &state->info;
  gboolean ret = FALSE;

  if (idx >= self->n_output_buffers) {
    GST_ERROR_OBJECT (self,
        "Invalid output buffer index %d of %" G_GSIZE_FORMAT, idx,
        self->n_output_buffers);
    goto done;
  }
  buf = &self->output_buffers[idx];

  ret =
      gst_amc_color_format_copy (&self->color_format_info, buf, buffer_info,
      info, outbuf, COLOR_FORMAT_COPY_OUT);

done:
  gst_video_codec_state_unref (state);
  return ret;
}

static void
gst_amc_video_dec_loop (GstAmcVideoDec * self)
{
  GstVideoCodecFrame *frame;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstClockTimeDiff deadline;
  gboolean is_eos;
  GstAmcBufferInfo buffer_info;
  gint idx;
  GError *err = NULL;
  gboolean release_buffer = TRUE;

  GST_VIDEO_DECODER_STREAM_LOCK (self);

retry:
  /*if (self->input_state_changed) {
     idx = INFO_OUTPUT_FORMAT_CHANGED;
     } else { */
  GST_DEBUG_OBJECT (self, "Waiting for available output buffer");
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  /* Wait at most 100ms here, some codecs don't fail dequeueing if
   * the codec is flushing, causing deadlocks during shutdown */
  idx =
      gst_amc_codec_dequeue_output_buffer (self->codec, &buffer_info, 100000,
      &err);
  GST_VIDEO_DECODER_STREAM_LOCK (self);
  /*} */

  GST_DEBUG_OBJECT (self, "dequeueOutputBuffer() returned %d (0x%x)", idx, idx);

  if (idx < 0) {
    if (self->flushing) {
      g_clear_error (&err);
      goto flushing;
    }

    switch (idx) {
      case INFO_OUTPUT_BUFFERS_CHANGED:{
        GST_DEBUG_OBJECT (self, "Output buffers have changed");

        /* If the decoder is configured with a surface, get_output_buffers returns null */
        if (self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE)
          break;

        if (self->output_buffers)
          gst_amc_codec_free_buffers (self->output_buffers,
              self->n_output_buffers);
        self->output_buffers =
            gst_amc_codec_get_output_buffers (self->codec,
            &self->n_output_buffers, &err);
        if (!self->output_buffers)
          goto get_output_buffers_error;
        break;
      }
      case INFO_OUTPUT_FORMAT_CHANGED:{
        GstAmcFormat *format;
        gchar *format_string;

        GST_DEBUG_OBJECT (self, "Output format has changed");

        format = gst_amc_codec_get_output_format (self->codec, &err);
        if (!format)
          goto format_error;

        format_string = gst_amc_format_to_string (format, &err);
        if (!format) {
          gst_amc_format_free (format);
          goto format_error;
        }
        GST_DEBUG_OBJECT (self, "Got new output format: %s", format_string);
        g_free (format_string);

        if (!gst_amc_video_dec_set_src_caps (self, format)) {
          gst_amc_format_free (format);
          goto format_error;
        }
        gst_amc_format_free (format);

        /* Setting source caps might have caused a reconfiguration */
        if (gst_pad_check_reconfigure (GST_VIDEO_DECODER (self)->srcpad)) {
          GST_DEBUG_OBJECT (self,
              "Reconfiguration needed after setting source caps");
          if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self))) {
            goto format_error;
          }
        }

        if (self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE)
          goto retry;

        if (self->output_buffers)
          gst_amc_codec_free_buffers (self->output_buffers,
              self->n_output_buffers);
        self->output_buffers =
            gst_amc_codec_get_output_buffers (self->codec,
            &self->n_output_buffers, &err);
        if (!self->output_buffers)
          goto get_output_buffers_error;

        goto retry;
      }
      case INFO_TRY_AGAIN_LATER:
        GST_DEBUG_OBJECT (self, "Dequeueing output buffer timed out");
        goto retry;
      case G_MININT:
        GST_ERROR_OBJECT (self, "Failure dequeueing output buffer");
        goto dequeue_error;
      default:
        g_assert_not_reached ();
        break;
    }

    goto retry;
  }

  GST_DEBUG_OBJECT (self,
      "Got output buffer at index %d: size %d time %" G_GINT64_FORMAT
      " flags 0x%08x", idx, buffer_info.size, buffer_info.presentation_time_us,
      buffer_info.flags);

  frame =
      _find_nearest_frame (self,
      gst_util_uint64_scale (buffer_info.presentation_time_us, GST_USECOND, 1));

  is_eos = ! !(buffer_info.flags & BUFFER_FLAG_END_OF_STREAM);

  if (frame
      && (deadline =
          gst_video_decoder_get_max_decode_time (GST_VIDEO_DECODER (self),
              frame)) < 0) {
    GST_WARNING_OBJECT (self,
        "Frame is too late, dropping (deadline %" GST_TIME_FORMAT ")",
        GST_TIME_ARGS (-deadline));
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
  } else if (self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE
      && buffer_info.size > 0) {
    GstMemory *mem;
    GstBuffer *outbuf;
    gint64 timeout;
    gint64 end_time;
    GstVideoCodecState *state;

    outbuf =
        gst_video_decoder_allocate_output_buffer (GST_VIDEO_DECODER (self));

    state = gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
    if (state && state->info.fps_n > 0 && state->info.fps_d > 0) {
      timeout =
          gst_util_uint64_scale_int (G_TIME_SPAN_SECOND, state->info.fps_d,
          state->info.fps_n);
    } else {
      timeout = GST_AMC_VIDEO_DEC_ON_FRAME_AVAILABLE_DEFAULT_TIMEOUT;
    }
    gst_video_codec_state_unref (state);

    mem = gst_buffer_peek_memory (outbuf, 0);
    if (gst_is_gl_memory (mem)) {
      GstGLMemory *gl_mem = (GstGLMemory *) mem;

      if (!self->renderer) {
        self->renderer =
            gst_amc_2d_texture_renderer_new (self->gl_context,
            self->surface->texture, self->width, self->height);
      }

      release_buffer = FALSE;
      self->on_frame_available = FALSE;
      g_mutex_lock (&self->on_frame_available_lock);

      /* Render the frame into the surface */
      if (!gst_amc_codec_release_output_buffer (self->codec, idx, TRUE, &err)) {
        gst_buffer_unref (outbuf);
        GST_ERROR_OBJECT (self, "Failed to render buffer, index %d", idx);
        GST_ELEMENT_ERROR_FROM_ERROR (self, err);

        goto gl_output_error;
      }

      /* Wait for the frame to become available */
      end_time = g_get_monotonic_time () + timeout;
      g_cond_wait_until (&self->on_frame_available_cond,
          &self->on_frame_available_lock, end_time);

      g_mutex_unlock (&self->on_frame_available_lock);

      /* Now that the frame is available, we can render it to a 2D texture.
       *
       * Calling updateTexImage seems necessary even if no frame is available
       * otherwise it could happen that the onFrameAvailable callback is not
       * executed anymore. */
      if (!gst_amc_2d_texture_renderer_render (self->renderer,
              gl_mem->tex_id, &err)) {
        gst_buffer_unref (outbuf);
        GST_ERROR_OBJECT (self, "Failed to render to a 2D texture");
        GST_ELEMENT_ERROR_FROM_ERROR (self, err);

        goto gl_output_error;
      }

    } else {
      GST_ERROR_OBJECT (self, "Wrong memory type for GL output mode");
      goto format_error;
    }

    if (self->on_frame_available) {
      if (frame) {
        frame->output_buffer = outbuf;
        flow_ret =
            gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
      } else {
        /* This sometimes happens at EOS or if the input is not properly framed,
         * let's handle it gracefully by allocating a new buffer for the current
         * caps and filling it
         */
        GST_BUFFER_PTS (outbuf) =
            gst_util_uint64_scale (buffer_info.presentation_time_us,
            GST_USECOND, 1);

        flow_ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), outbuf);
      }
    } else {
      GST_WARNING_OBJECT (self, "No frame available after %lldms",
          timeout / G_TIME_SPAN_MILLISECOND);

      if (frame) {
        flow_ret =
            gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
      }
      gst_buffer_unref (outbuf);
    }
  } else if (self->codec_config == AMC_CODEC_CONFIG_WITHOUT_SURFACE && !frame
      && buffer_info.size > 0) {
    GstBuffer *outbuf;

    /* This sometimes happens at EOS or if the input is not properly framed,
     * let's handle it gracefully by allocating a new buffer for the current
     * caps and filling it
     */
    GST_ERROR_OBJECT (self, "No corresponding frame found");

    outbuf =
        gst_video_decoder_allocate_output_buffer (GST_VIDEO_DECODER (self));

    if (!gst_amc_video_dec_fill_buffer (self, idx, &buffer_info, outbuf)) {
      gst_buffer_unref (outbuf);
      if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err))
        GST_ERROR_OBJECT (self, "Failed to release output buffer index %d",
            idx);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
      goto invalid_buffer;
    }

    GST_BUFFER_PTS (outbuf) =
        gst_util_uint64_scale (buffer_info.presentation_time_us, GST_USECOND,
        1);
    flow_ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), outbuf);
  } else if (self->codec_config == AMC_CODEC_CONFIG_WITHOUT_SURFACE && frame
      && buffer_info.size > 0) {
    if ((flow_ret =
            gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER (self),
                frame)) != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Failed to allocate buffer");
      if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err))
        GST_ERROR_OBJECT (self, "Failed to release output buffer index %d",
            idx);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
      goto flow_error;
    }

    if (!gst_amc_video_dec_fill_buffer (self, idx, &buffer_info,
            frame->output_buffer)) {
      gst_buffer_replace (&frame->output_buffer, NULL);
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
      if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err))
        GST_ERROR_OBJECT (self, "Failed to release output buffer index %d",
            idx);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
      goto invalid_buffer;
    }

    flow_ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
  } else if (frame != NULL) {
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
  }

  if (release_buffer) {
    if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err)) {
      if (self->flushing) {
        g_clear_error (&err);
        goto flushing;
      }
      goto failed_release;
    }
  }

  if (is_eos || flow_ret == GST_FLOW_EOS) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    if (self->draining) {
      GST_DEBUG_OBJECT (self, "Drained");
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
    } else if (flow_ret == GST_FLOW_OK) {
      GST_DEBUG_OBJECT (self, "Component signalled EOS");
      flow_ret = GST_FLOW_EOS;
    }
    g_mutex_unlock (&self->drain_lock);
    GST_VIDEO_DECODER_STREAM_LOCK (self);
  } else {
    GST_DEBUG_OBJECT (self, "Finished frame: %s", gst_flow_get_name (flow_ret));
  }

  self->downstream_flow_ret = flow_ret;

  if (flow_ret != GST_FLOW_OK)
    goto flow_error;

  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  return;

dequeue_error:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }

get_output_buffers_error:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }

format_error:
  {
    if (err)
      GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    else
      GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
          ("Failed to handle format"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }
failed_release:
  {
    GST_VIDEO_DECODER_ERROR_FROM_ERROR (self, err);
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- stopping task");
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_FLUSHING;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }

flow_error:
  {
    if (flow_ret == GST_FLOW_EOS) {
      GST_DEBUG_OBJECT (self, "EOS");
      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    } else if (flow_ret < GST_FLOW_EOS) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED,
          ("Internal data stream error."), ("stream stopped, reason %s",
              gst_flow_get_name (flow_ret)));
      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    } else if (flow_ret == GST_FLOW_FLUSHING) {
      GST_DEBUG_OBJECT (self, "Flushing -- stopping task");
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    }
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }

invalid_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Invalid sized input buffer"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }
gl_output_error:
  {
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }
}

static gboolean
gst_amc_video_dec_start (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;

  self = GST_AMC_VIDEO_DEC (decoder);
  self->last_upstream_ts = 0;
  self->drained = TRUE;
  self->downstream_flow_ret = GST_FLOW_OK;
  self->started = FALSE;
  self->flushing = TRUE;

  return TRUE;
}

static gboolean
gst_amc_video_dec_stop (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;
  GError *err = NULL;

  self = GST_AMC_VIDEO_DEC (decoder);
  GST_DEBUG_OBJECT (self, "Stopping decoder");
  self->flushing = TRUE;
  if (self->started) {
    gst_amc_codec_flush (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    gst_amc_codec_stop (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    self->started = FALSE;
    if (self->input_buffers)
      gst_amc_codec_free_buffers (self->input_buffers, self->n_input_buffers);
    self->input_buffers = NULL;
    if (self->output_buffers)
      gst_amc_codec_free_buffers (self->output_buffers, self->n_output_buffers);
    self->output_buffers = NULL;
  }
  gst_pad_stop_task (GST_VIDEO_DECODER_SRC_PAD (decoder));

  self->downstream_flow_ret = GST_FLOW_FLUSHING;
  self->drained = TRUE;
  g_mutex_lock (&self->drain_lock);
  self->draining = FALSE;
  g_cond_broadcast (&self->drain_cond);
  g_mutex_unlock (&self->drain_lock);
  g_free (self->codec_data);
  self->codec_data_size = 0;
  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;
  GST_DEBUG_OBJECT (self, "Stopped decoder");
  return TRUE;
}

static jobject
gst_amc_video_dec_new_on_frame_available_listener (GstAmcVideoDec * decoder,
    JNIEnv * env, GError ** err)
{
  jobject listener = NULL;
  jclass listener_cls = NULL;
  jmethodID constructor_id = 0;
  jmethodID set_context_id = 0;

  JNINativeMethod amcOnFrameAvailableListener = {
    "native_onFrameAvailable",
    "(JLandroid/graphics/SurfaceTexture;)V",
    (void *) gst_amc_video_dec_on_frame_available,
  };

  listener_cls =
      gst_amc_jni_get_application_class (env,
      "org/freedesktop/gstreamer/androidmedia/GstAmcOnFrameAvailableListener",
      err);
  if (!listener_cls) {
    return FALSE;
  }

  (*env)->RegisterNatives (env, listener_cls, &amcOnFrameAvailableListener, 1);
  if ((*env)->ExceptionCheck (env)) {
    (*env)->ExceptionClear (env);
    goto done;
  }

  constructor_id =
      gst_amc_jni_get_method_id (env, err, listener_cls, "<init>", "()V");
  if (!constructor_id) {
    goto done;
  }

  set_context_id =
      gst_amc_jni_get_method_id (env, err, listener_cls, "setContext", "(J)V");
  if (!set_context_id) {
    goto done;
  }

  listener =
      gst_amc_jni_new_object (env, err, TRUE, listener_cls, constructor_id);
  if (!listener) {
    goto done;
  }

  if (!gst_amc_jni_call_void_method (env, err, listener,
          set_context_id, GST_AMC_VIDEO_DEC_TO_JLONG (decoder))) {
    gst_amc_jni_object_unref (env, listener);
    listener = NULL;
  }

done:
  gst_amc_jni_object_unref (env, listener_cls);

  return listener;
}

static gboolean
gst_amc_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstAmcVideoDec *self;
  GstAmcVideoDecClass *klass;
  GstAmcFormat *format;
  const gchar *mime;
  gboolean is_format_change = FALSE;
  gboolean needs_disable = FALSE;
  gchar *format_string;
  guint8 *codec_data = NULL;
  gsize codec_data_size = 0;
  GError *err = NULL;
  jobject jsurface = NULL;

  self = GST_AMC_VIDEO_DEC (decoder);
  klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);

  GST_DEBUG_OBJECT (self, "Setting new caps %" GST_PTR_FORMAT, state->caps);

  /* Check if the caps change is a real format change or if only irrelevant
   * parts of the caps have changed or nothing at all.
   */
  is_format_change |= self->color_format_info.width != state->info.width;
  is_format_change |= self->color_format_info.height != state->info.height;
  if (state->codec_data) {
    GstMapInfo cminfo;

    gst_buffer_map (state->codec_data, &cminfo, GST_MAP_READ);
    codec_data = g_memdup (cminfo.data, cminfo.size);
    codec_data_size = cminfo.size;

    is_format_change |= (!self->codec_data
        || self->codec_data_size != codec_data_size
        || memcmp (self->codec_data, codec_data, codec_data_size) != 0);
    gst_buffer_unmap (state->codec_data, &cminfo);
  } else if (self->codec_data) {
    is_format_change |= TRUE;
  }

  needs_disable = self->started;

  /* If the component is not started and a real format change happens
   * we have to restart the component. If no real format change
   * happened we can just exit here.
   */
  if (needs_disable && !is_format_change) {
    g_free (codec_data);
    codec_data = NULL;
    codec_data_size = 0;

    /* Framerate or something minor changed */
    self->input_state_changed = TRUE;
    if (self->input_state)
      gst_video_codec_state_unref (self->input_state);
    self->input_state = gst_video_codec_state_ref (state);
    GST_DEBUG_OBJECT (self,
        "Already running and caps did not change the format");
    return TRUE;
  }

  if (needs_disable && is_format_change) {
    gst_amc_video_dec_drain (self);
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    gst_amc_video_dec_stop (GST_VIDEO_DECODER (self));
    GST_VIDEO_DECODER_STREAM_LOCK (self);
    gst_amc_video_dec_close (GST_VIDEO_DECODER (self));
    if (!gst_amc_video_dec_open (GST_VIDEO_DECODER (self))) {
      GST_ERROR_OBJECT (self, "Failed to open codec again");
      return FALSE;
    }

    if (!gst_amc_video_dec_start (GST_VIDEO_DECODER (self))) {
      GST_ERROR_OBJECT (self, "Failed to start codec again");
    }
  }
  /* srcpad task is not running at this point */
  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;

  g_free (self->codec_data);
  self->codec_data = codec_data;
  self->codec_data_size = codec_data_size;

  mime = caps_to_mime (state->caps);
  if (!mime) {
    GST_ERROR_OBJECT (self, "Failed to convert caps to mime");
    return FALSE;
  }

  format =
      gst_amc_format_new_video (mime, state->info.width, state->info.height,
      &err);
  if (!format) {
    GST_ERROR_OBJECT (self, "Failed to create video format");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }

  /* FIXME: This buffer needs to be valid until the codec is stopped again */
  if (self->codec_data) {
    gst_amc_format_set_buffer (format, "csd-0", self->codec_data,
        self->codec_data_size, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  }

  {
    gboolean downstream_supports_gl = FALSE;
    GstVideoDecoder *decoder = GST_VIDEO_DECODER (self);
    GstPad *src_pad = GST_VIDEO_DECODER_SRC_PAD (decoder);
    GstCaps *templ_caps = gst_pad_get_pad_template_caps (src_pad);
    GstCaps *downstream_caps = gst_pad_peer_query_caps (src_pad, templ_caps);

    gst_caps_unref (templ_caps);

    if (downstream_caps) {
      guint i, n;
      GstStaticCaps static_caps =
          GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
          (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, "RGBA"));
      GstCaps *gl_memory_caps = gst_static_caps_get (&static_caps);

      GST_DEBUG_OBJECT (self, "Available downstream caps: %" GST_PTR_FORMAT,
          downstream_caps);

      /* Check if downstream caps supports
       * video/x-raw(memory:GLMemory),format=RGBA */
      n = gst_caps_get_size (downstream_caps);
      for (i = 0; i < n; i++) {
        GstCaps *caps = NULL;
        GstStructure *structure = gst_caps_get_structure (downstream_caps, i);
        GstCapsFeatures *features = gst_caps_get_features (downstream_caps, i);

        caps = gst_caps_new_full (gst_structure_copy (structure), NULL);
        if (!caps)
          continue;

        gst_caps_set_features (caps, 0, gst_caps_features_copy (features));

        if (gst_caps_can_intersect (caps, gl_memory_caps)) {
          downstream_supports_gl = TRUE;
        }

        gst_caps_unref (caps);
        if (downstream_supports_gl)
          break;
      }

      gst_caps_unref (gl_memory_caps);

      /* If video/x-raw(memory:GLMemory),format=RGBA is supported,
       * update the video decoder output state accordingly and negotiate */
      if (downstream_supports_gl) {
        GstVideoCodecState *output_state = NULL;
        GstVideoCodecState *prev_output_state = NULL;

        prev_output_state = gst_video_decoder_get_output_state (decoder);

        output_state =
            gst_video_decoder_set_output_state (decoder, GST_VIDEO_FORMAT_RGBA,
            state->info.width, state->info.height, state);

        if (output_state->caps) {
          gst_caps_unref (output_state->caps);
        }

        output_state->caps = gst_video_info_to_caps (&output_state->info);
        gst_caps_set_features (output_state->caps, 0,
            gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));

        /* gst_amc_video_dec_decide_allocation will update
         * self->downstream_supports_gl */
        if (!gst_video_decoder_negotiate (decoder)) {
          GST_ERROR_OBJECT (self, "Failed to negotiate");

          /* Rollback output state changes */
          if (prev_output_state) {
            output_state->info = prev_output_state->info;
            gst_caps_replace (&output_state->caps, prev_output_state->caps);
          } else {
            gst_video_info_init (&output_state->info);
            gst_caps_replace (&output_state->caps, NULL);
          }
        }
        if (prev_output_state) {
          gst_video_codec_state_unref (prev_output_state);
        }
      }
    }
  }

  GST_INFO_OBJECT (self, "GL output: %s",
      self->downstream_supports_gl ? "enabled" : "disabled");

  if (klass->codec_info->gl_output_only && !self->downstream_supports_gl) {
    GST_ERROR_OBJECT (self,
        "Codec only supports GL output but downstream does not");
    return FALSE;
  }

  if (self->downstream_supports_gl && self->surface) {
    jsurface = self->surface->jobject;
  } else if (self->downstream_supports_gl && !self->surface) {
    int ret = TRUE;
    JNIEnv *env = NULL;
    jobject listener = NULL;
    GstAmcSurfaceTexture *surface_texture = NULL;

    env = gst_amc_jni_get_env ();
    surface_texture = gst_amc_surface_texture_new (&err);
    if (!surface_texture) {
      GST_ELEMENT_ERROR_FROM_ERROR (self, err);
      return FALSE;
    }

    listener =
        gst_amc_video_dec_new_on_frame_available_listener (self, env, &err);
    if (!listener) {
      ret = FALSE;
      goto done;
    }

    if (!gst_amc_surface_texture_set_on_frame_available_listener
        (surface_texture, listener, &err)) {
      ret = FALSE;
      goto done;
    }

    self->surface = gst_amc_surface_new (surface_texture, &err);
    jsurface = self->surface->jobject;

  done:
    g_object_unref (surface_texture);
    gst_amc_jni_object_unref (env, listener);
    if (!ret) {
      GST_ELEMENT_ERROR_FROM_ERROR (self, err);
      return FALSE;
    }
  }

  format_string = gst_amc_format_to_string (format, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  GST_DEBUG_OBJECT (self, "Configuring codec with format: %s",
      GST_STR_NULL (format_string));
  g_free (format_string);

  if (!gst_amc_codec_configure (self->codec, format, jsurface, 0, &err)) {
    GST_ERROR_OBJECT (self, "Failed to configure codec");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }
  if (jsurface) {
    self->codec_config = AMC_CODEC_CONFIG_WITH_SURFACE;
  } else {
    self->codec_config = AMC_CODEC_CONFIG_WITHOUT_SURFACE;
  }

  gst_amc_format_free (format);

  if (!gst_amc_codec_start (self->codec, &err)) {
    GST_ERROR_OBJECT (self, "Failed to start codec");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }

  if (self->input_buffers)
    gst_amc_codec_free_buffers (self->input_buffers, self->n_input_buffers);
  self->input_buffers =
      gst_amc_codec_get_input_buffers (self->codec, &self->n_input_buffers,
      &err);
  if (!self->input_buffers) {
    GST_ERROR_OBJECT (self, "Failed to get input buffers");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }

  self->started = TRUE;
  self->input_state = gst_video_codec_state_ref (state);
  self->input_state_changed = TRUE;

  /* Start the srcpad loop again */
  self->flushing = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_amc_video_dec_loop, decoder, NULL);

  return TRUE;
}

static gboolean
gst_amc_video_dec_flush (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;
  GError *err = NULL;

  self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Flushing decoder");

  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Codec not started yet");
    return TRUE;
  }

  self->flushing = TRUE;
  /* Wait until the srcpad loop is finished,
   * unlock GST_VIDEO_DECODER_STREAM_LOCK to prevent deadlocks
   * caused by using this lock from inside the loop function */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_VIDEO_DECODER_STREAM_LOCK (self);
  gst_amc_codec_flush (self->codec, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  self->flushing = FALSE;

  /* Start the srcpad loop again */
  self->last_upstream_ts = 0;
  self->drained = TRUE;
  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_amc_video_dec_loop, decoder, NULL);

  GST_DEBUG_OBJECT (self, "Flushed decoder");

  return TRUE;
}

static GstFlowReturn
gst_amc_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstAmcVideoDec *self;
  gint idx;
  GstAmcBuffer *buf;
  GstAmcBufferInfo buffer_info;
  guint offset = 0;
  GstClockTime timestamp, duration, timestamp_offset = 0;
  GstMapInfo minfo;
  GError *err = NULL;

  memset (&minfo, 0, sizeof (minfo));

  self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Handling frame");

  if (!self->started) {
    GST_ERROR_OBJECT (self, "Codec not started yet");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (self->flushing)
    goto flushing;

  if (self->downstream_flow_ret != GST_FLOW_OK)
    goto downstream_error;

  timestamp = frame->pts;
  duration = frame->duration;

  gst_buffer_map (frame->input_buffer, &minfo, GST_MAP_READ);

  while (offset < minfo.size) {
    /* Make sure to release the base class stream lock, otherwise
     * _loop() can't call _finish_frame() and we might block forever
     * because no input buffers are released */
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    /* Wait at most 100ms here, some codecs don't fail dequeueing if
     * the codec is flushing, causing deadlocks during shutdown */
    idx = gst_amc_codec_dequeue_input_buffer (self->codec, 100000, &err);
    GST_VIDEO_DECODER_STREAM_LOCK (self);

    if (idx < 0) {
      if (self->flushing || self->downstream_flow_ret == GST_FLOW_FLUSHING) {
        g_clear_error (&err);
        goto flushing;
      }

      switch (idx) {
        case INFO_TRY_AGAIN_LATER:
          GST_DEBUG_OBJECT (self, "Dequeueing input buffer timed out");
          continue;             /* next try */
          break;
        case G_MININT:
          GST_ERROR_OBJECT (self, "Failed to dequeue input buffer");
          goto dequeue_error;
        default:
          g_assert_not_reached ();
          break;
      }

      continue;
    }

    if (idx >= self->n_input_buffers)
      goto invalid_buffer_index;

    if (self->flushing) {
      memset (&buffer_info, 0, sizeof (buffer_info));
      gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info, NULL);
      goto flushing;
    }

    if (self->downstream_flow_ret != GST_FLOW_OK) {
      memset (&buffer_info, 0, sizeof (buffer_info));
      gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info, &err);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
      goto downstream_error;
    }

    /* Now handle the frame */

    /* Copy the buffer content in chunks of size as requested
     * by the port */
    buf = &self->input_buffers[idx];

    memset (&buffer_info, 0, sizeof (buffer_info));
    buffer_info.offset = 0;
    buffer_info.size = MIN (minfo.size - offset, buf->size);

    orc_memcpy (buf->data, minfo.data + offset, buffer_info.size);

    /* Interpolate timestamps if we're passing the buffer
     * in multiple chunks */
    if (offset != 0 && duration != GST_CLOCK_TIME_NONE) {
      timestamp_offset = gst_util_uint64_scale (offset, duration, minfo.size);
    }

    if (timestamp != GST_CLOCK_TIME_NONE) {
      buffer_info.presentation_time_us =
          gst_util_uint64_scale (timestamp + timestamp_offset, 1, GST_USECOND);
      self->last_upstream_ts = timestamp + timestamp_offset;
    }
    if (duration != GST_CLOCK_TIME_NONE)
      self->last_upstream_ts += duration;

    if (offset == 0) {
      BufferIdentification *id =
          buffer_identification_new (timestamp + timestamp_offset);
      if (GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame))
        buffer_info.flags |= BUFFER_FLAG_SYNC_FRAME;
      gst_video_codec_frame_set_user_data (frame, id,
          (GDestroyNotify) buffer_identification_free);
    }

    offset += buffer_info.size;
    GST_DEBUG_OBJECT (self,
        "Queueing buffer %d: size %d time %" G_GINT64_FORMAT
        " flags 0x%08x", idx, buffer_info.size,
        buffer_info.presentation_time_us, buffer_info.flags);
    if (!gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info,
            &err)) {
      if (self->flushing) {
        g_clear_error (&err);
        goto flushing;
      }
      goto queue_error;
    }
    self->drained = FALSE;
  }

  gst_buffer_unmap (frame->input_buffer, &minfo);
  gst_video_codec_frame_unref (frame);

  return self->downstream_flow_ret;

downstream_error:
  {
    GST_ERROR_OBJECT (self, "Downstream returned %s",
        gst_flow_get_name (self->downstream_flow_ret));
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return self->downstream_flow_ret;
  }
invalid_buffer_index:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("Invalid input buffer index %d of %" G_GSIZE_FORMAT, idx,
            self->n_input_buffers));
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
dequeue_error:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
queue_error:
  {
    GST_VIDEO_DECODER_ERROR_FROM_ERROR (self, err);
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- returning FLUSHING");
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_FLUSHING;
  }
}

static GstFlowReturn
gst_amc_video_dec_finish (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;

  self = GST_AMC_VIDEO_DEC (decoder);

  return gst_amc_video_dec_drain (self);
}

static GstFlowReturn
gst_amc_video_dec_drain (GstAmcVideoDec * self)
{
  GstFlowReturn ret;
  gint idx;
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "Draining codec");
  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Codec not started yet");
    return GST_FLOW_OK;
  }

  /* Don't send drain buffer twice, this doesn't work */
  if (self->drained) {
    GST_DEBUG_OBJECT (self, "Codec is drained already");
    return GST_FLOW_OK;
  }

  /* Make sure to release the base class stream lock, otherwise
   * _loop() can't call _finish_frame() and we might block forever
   * because no input buffers are released */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  /* Send an EOS buffer to the component and let the base
   * class drop the EOS event. We will send it later when
   * the EOS buffer arrives on the output port.
   * Wait at most 0.5s here. */
  idx = gst_amc_codec_dequeue_input_buffer (self->codec, 500000, &err);
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  if (idx >= 0 && idx < self->n_input_buffers) {
    GstAmcBufferInfo buffer_info;

    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = TRUE;

    memset (&buffer_info, 0, sizeof (buffer_info));
    buffer_info.size = 0;
    buffer_info.presentation_time_us =
        gst_util_uint64_scale (self->last_upstream_ts, 1, GST_USECOND);
    buffer_info.flags |= BUFFER_FLAG_END_OF_STREAM;

    if (gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info, &err)) {
      GST_DEBUG_OBJECT (self, "Waiting until codec is drained");
      g_cond_wait (&self->drain_cond, &self->drain_lock);
      GST_DEBUG_OBJECT (self, "Drained codec");
      ret = GST_FLOW_OK;
    } else {
      GST_ERROR_OBJECT (self, "Failed to queue input buffer");
      if (self->flushing) {
        g_clear_error (&err);
        ret = GST_FLOW_FLUSHING;
      } else {
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
        ret = GST_FLOW_ERROR;
      }
    }

    self->drained = TRUE;
    self->draining = FALSE;
    g_mutex_unlock (&self->drain_lock);
    GST_VIDEO_DECODER_STREAM_LOCK (self);
  } else if (idx >= self->n_input_buffers) {
    GST_ERROR_OBJECT (self,
        "Invalid input buffer index %d of %" G_GSIZE_FORMAT, idx,
        self->n_input_buffers);
    ret = GST_FLOW_ERROR;
  } else {
    GST_ERROR_OBJECT (self, "Failed to acquire buffer for EOS: %d", idx);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    ret = GST_FLOW_ERROR;
  }

  return ret;
}

static gboolean
_caps_are_rgba_with_gl_memory (GstCaps * caps)
{
  GstVideoInfo info;
  GstCapsFeatures *features;

  if (!caps)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  if (info.finfo->format != GST_VIDEO_FORMAT_RGBA)
    return FALSE;

  if (!(features = gst_caps_get_features (caps, 0)))
    return FALSE;

  return gst_caps_features_contains (features,
      GST_CAPS_FEATURE_MEMORY_GL_MEMORY);
}

static gboolean
gst_amc_video_dec_decide_allocation (GstVideoDecoder * bdec, GstQuery * query)
{
  GstCaps *caps = NULL;
  gboolean need_pool = FALSE;
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (bdec);

  if (!GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (bdec, query))
    return FALSE;

  self->downstream_supports_gl = FALSE;
  gst_query_parse_allocation (query, &caps, &need_pool);
  if (_caps_are_rgba_with_gl_memory (caps)) {
    guint i, n_allocation_pools;
    GstGLBufferPool *gl_pool = NULL;
    GstGLContext *gl_context = NULL;
    GstGLDisplay *gl_display = NULL;

    n_allocation_pools = MAX (gst_query_get_n_allocation_pools (query), 1);
    for (i = 0; i < n_allocation_pools; i++) {
      GstBufferPool *pool = NULL;
      GstStructure *config = NULL;
      guint min, max, size;

      gst_query_parse_nth_allocation_pool (query, i, &pool, &size, &min, &max);
      config = gst_buffer_pool_get_config (pool);
      if (!config) {
        gst_object_unref (pool);
        continue;
      }

      if (!GST_IS_GL_BUFFER_POOL (pool)) {
        gst_object_unref (pool);
        continue;
      }

      gl_pool = GST_GL_BUFFER_POOL (pool);
      break;
    }

    if (!gl_pool) {
      GST_WARNING_OBJECT (bdec, "Failed to get gl pool from downstream");
      gst_object_unref (gl_pool);
      return FALSE;
    }

    gl_context = gl_pool->context;
    if (!gl_context) {
      GST_WARNING_OBJECT (bdec, "Failed to get gl context from downstream");
      gst_object_unref (gl_pool);
      return FALSE;
    }

    if (self->gl_context) {
      gst_object_unref (self->gl_context);
    }

    if (self->renderer) {
      gst_amc_2d_texture_renderer_free (self->renderer);
      self->renderer = NULL;
    }

    gl_display = gst_gl_context_get_display (gl_context);
    self->gl_context = gst_gl_context_new (gl_display);
    gst_object_unref (gl_display);

    gst_gl_context_create (self->gl_context, gl_context, NULL);

    gst_object_unref (gl_pool);

    self->downstream_supports_gl = TRUE;
  }

  return gst_amc_video_dec_check_codec_config (self);
}

static void
gst_amc_video_dec_on_frame_available (JNIEnv * env, jobject thiz,
    long long context, jobject surfaceTexture)
{
  GstAmcVideoDec *dec = JLONG_TO_GST_AMC_VIDEO_DEC (context);

  g_mutex_lock (&dec->on_frame_available_lock);
  dec->on_frame_available = TRUE;
  g_cond_signal (&dec->on_frame_available_cond);
  g_mutex_unlock (&dec->on_frame_available_lock);
}
