/*
 * GStreamer
 * Copyright (C) 2010 Texas Instruments, Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


/**
 * SECTION:element-wrappercamerabinsrc
 *
 * A camera bin src element that wraps a default video source with a single
 * pad into the 3pad model that camerabin2 expects.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstwrappercamerabinsrc.h"
#include "camerabingeneral.h"

enum
{
  PROP_0,
  PROP_VIDEO_SRC,
};

#define CAMERABIN_DEFAULT_VF_CAPS "video/x-raw-yuv,format=(fourcc)I420"

/* Using "bilinear" as default zoom method */
#define CAMERABIN_DEFAULT_ZOOM_METHOD 1

GST_DEBUG_CATEGORY (wrapper_camera_bin_src_debug);
#define GST_CAT_DEFAULT wrapper_camera_bin_src_debug

GST_BOILERPLATE (GstWrapperCameraBinSrc, gst_wrapper_camera_bin_src,
    GstBaseCameraSrc, GST_TYPE_BASE_CAMERA_SRC);

static void set_capsfilter_caps (GstWrapperCameraBinSrc * self,
    GstCaps * new_caps);

static void
gst_wrapper_camera_bin_src_dispose (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_wrapper_camera_bin_src_finalize (GstWrapperCameraBinSrc * self)
{
  G_OBJECT_CLASS (parent_class)->finalize ((GObject *) (self));
}

static void
gst_wrapper_camera_bin_src_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstWrapperCameraBinSrc *self = GST_WRAPPER_CAMERA_BIN_SRC (object);

  switch (prop_id) {
    case PROP_VIDEO_SRC:
      if (GST_STATE (self) != GST_STATE_NULL) {
        GST_ELEMENT_ERROR (self, CORE, FAILED,
            ("camerasrc must be in NULL state when setting the video source element"),
            (NULL));
      } else {
        if (self->app_vid_src)
          gst_object_unref (self->app_vid_src);
        self->app_vid_src = g_value_get_object (value);
        if (self->app_vid_src)
          gst_object_ref (self->app_vid_src);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
      break;
  }
}

static void
gst_wrapper_camera_bin_src_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstWrapperCameraBinSrc *self = GST_WRAPPER_CAMERA_BIN_SRC (object);

  switch (prop_id) {
    case PROP_VIDEO_SRC:
      if (self->src_vid_src)
        g_value_set_object (value, self->src_vid_src);
      else
        g_value_set_object (value, self->app_vid_src);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
      break;
  }
}

/**
 * gst_wrapper_camera_bin_src_imgsrc_probe:
 *
 * Buffer probe called before sending each buffer to image queue.
 */
static gboolean
gst_wrapper_camera_bin_src_imgsrc_probe (GstPad * pad, GstBuffer * buffer,
    gpointer data)
{
  GstWrapperCameraBinSrc *self = GST_WRAPPER_CAMERA_BIN_SRC (data);
  GstBaseCameraSrc *camerasrc = GST_BASE_CAMERA_SRC (data);
  gboolean ret = FALSE;

  g_mutex_lock (camerasrc->capturing_mutex);
  if (self->image_capture_count > 0) {
    ret = TRUE;
    self->image_capture_count--;
    if (self->image_capture_count == 0) {
      gst_base_camera_src_finish_capture (camerasrc);
    }
  }
  g_mutex_unlock (camerasrc->capturing_mutex);
  return ret;
}

/**
 * gst_wrapper_camera_bin_src_vidsrc_probe:
 *
 * Buffer probe called before sending each buffer to image queue.
 */
static gboolean
gst_wrapper_camera_bin_src_vidsrc_probe (GstPad * pad, GstBuffer * buffer,
    gpointer data)
{
  GstWrapperCameraBinSrc *self = GST_WRAPPER_CAMERA_BIN_SRC (data);
  GstBaseCameraSrc *camerasrc = GST_BASE_CAMERA_SRC_CAST (self);
  gboolean ret = FALSE;

  /* TODO do we want to lock for every buffer? */
  /*
   * Note that we can use gst_pad_push_event here because we are a buffer
   * probe.
   */
  /* TODO shouldn't access this directly */
  g_mutex_lock (camerasrc->capturing_mutex);
  if (self->video_rec_status == GST_VIDEO_RECORDING_STATUS_DONE) {
    /* NOP */
  } else if (self->video_rec_status == GST_VIDEO_RECORDING_STATUS_STARTING) {
    /* send the newseg */
    GST_DEBUG_OBJECT (self, "Starting video recording, pushing newsegment");
    gst_pad_push_event (pad, gst_event_new_new_segment (FALSE, 1.0,
            GST_FORMAT_TIME, GST_BUFFER_TIMESTAMP (buffer), -1, 0));
    self->video_rec_status = GST_VIDEO_RECORDING_STATUS_RUNNING;
    ret = TRUE;
  } else if (self->video_rec_status == GST_VIDEO_RECORDING_STATUS_FINISHING) {
    /* send eos */
    GST_DEBUG_OBJECT (self, "Finishing video recording, pushing eos");
    gst_pad_push_event (pad, gst_event_new_eos ());
    self->video_rec_status = GST_VIDEO_RECORDING_STATUS_DONE;
    gst_base_camera_src_finish_capture (camerasrc);
  } else {
    ret = TRUE;
  }
  g_mutex_unlock (camerasrc->capturing_mutex);
  return ret;
}

/**
 * gst_wrapper_camera_bin_src_construct_pipeline:
 * @bcamsrc: camerasrc object
 *
 * This function creates and links the elements of the camerasrc bin
 * videosrc ! cspconv ! capsfilter ! crop ! scale ! capsfilter ! tee ! ..
 *
 * Returns: TRUE, if elements were successfully created, FALSE otherwise
 */
static gboolean
gst_wrapper_camera_bin_src_construct_pipeline (GstBaseCameraSrc * bcamsrc)
{
  GstWrapperCameraBinSrc *self = GST_WRAPPER_CAMERA_BIN_SRC (bcamsrc);
  GstBin *cbin = GST_BIN (bcamsrc);
  GstElement *tee;
  gboolean ret = FALSE;

  GST_DEBUG_OBJECT (self, "constructing pipeline");

  /* Add application set or default video src element */
  if (!(self->src_vid_src = gst_camerabin_setup_default_element (cbin,
              self->app_vid_src, "autovideosrc", DEFAULT_VIDEOSRC))) {
    self->src_vid_src = NULL;
    goto done;
  } else {
    if (!gst_camerabin_add_element (cbin, self->src_vid_src)) {
      goto done;
    }
  }

  if (!gst_camerabin_create_and_add_element (cbin, "ffmpegcolorspace"))
    goto done;

  if (!(self->src_filter =
          gst_camerabin_create_and_add_element (cbin, "capsfilter")))
    goto done;

  if (!(self->src_zoom_crop =
          gst_camerabin_create_and_add_element (cbin, "videocrop")))
    goto done;
  if (!(self->src_zoom_scale =
          gst_camerabin_create_and_add_element (cbin, "videoscale")))
    goto done;
  if (!(self->src_zoom_filter =
          gst_camerabin_create_and_add_element (cbin, "capsfilter")))
    goto done;

  if (!(tee = gst_camerabin_create_and_add_element (cbin, "tee")))
    goto done;

  self->tee_vf_srcpad = gst_element_get_request_pad (tee, "src%d");
  self->tee_image_srcpad = gst_element_get_request_pad (tee, "src%d");
  self->tee_video_srcpad = gst_element_get_request_pad (tee, "src%d");

  gst_pad_add_buffer_probe (self->tee_image_srcpad,
      G_CALLBACK (gst_wrapper_camera_bin_src_imgsrc_probe), self);
  gst_pad_add_buffer_probe (self->tee_video_srcpad,
      G_CALLBACK (gst_wrapper_camera_bin_src_vidsrc_probe), self);

  /* hook-up the ghostpads */
  gst_ghost_pad_set_target (GST_GHOST_PAD (self->vfsrc), self->tee_vf_srcpad);
  gst_ghost_pad_set_target (GST_GHOST_PAD (self->imgsrc),
      self->tee_image_srcpad);
  gst_ghost_pad_set_target (GST_GHOST_PAD (self->vidsrc),
      self->tee_video_srcpad);

  gst_pad_set_active (self->vfsrc, TRUE);
  gst_pad_set_active (self->imgsrc, TRUE);      /* XXX ??? */
  gst_pad_set_active (self->vidsrc, TRUE);      /* XXX ??? */


  ret = TRUE;
done:
  return ret;
}

static gboolean
copy_missing_fields (GQuark field_id, const GValue * value, gpointer user_data)
{
  GstStructure *st = (GstStructure *) user_data;
  const GValue *val = gst_structure_id_get_value (st, field_id);

  if (G_UNLIKELY (val == NULL)) {
    gst_structure_id_set_value (st, field_id, value);
  }

  return TRUE;
}

/**
 * adapt_image_capture:
 * @self: camerasrc object
 * @in_caps: caps object that describes incoming image format
 *
 * Adjust capsfilters and crop according image capture caps if necessary.
 * The captured image format from video source might be different from
 * what application requested, so we can try to fix that in camerabin.
 *
 */
static void
adapt_image_capture (GstWrapperCameraBinSrc * self, GstCaps * in_caps)
{
  GstBaseCameraSrc *bcamsrc = GST_BASE_CAMERA_SRC (self);
  GstStructure *in_st, *new_st, *req_st;
  gint in_width = 0, in_height = 0, req_width = 0, req_height = 0, crop = 0;
  gdouble ratio_w, ratio_h;
  GstCaps *filter_caps = NULL;

  GST_LOG_OBJECT (self, "in caps: %" GST_PTR_FORMAT, in_caps);
  GST_LOG_OBJECT (self, "requested caps: %" GST_PTR_FORMAT,
      self->image_capture_caps);

  in_st = gst_caps_get_structure (in_caps, 0);
  gst_structure_get_int (in_st, "width", &in_width);
  gst_structure_get_int (in_st, "height", &in_height);

  req_st = gst_caps_get_structure (self->image_capture_caps, 0);
  gst_structure_get_int (req_st, "width", &req_width);
  gst_structure_get_int (req_st, "height", &req_height);

  GST_INFO_OBJECT (self, "we requested %dx%d, and got %dx%d", req_width,
      req_height, in_width, in_height);

  new_st = gst_structure_copy (req_st);
  /* If new fields have been added, we need to copy them */
  gst_structure_foreach (in_st, copy_missing_fields, new_st);

  gst_structure_set (new_st, "width", G_TYPE_INT, in_width, "height",
      G_TYPE_INT, in_height, NULL);

  GST_LOG_OBJECT (self, "new image capture caps: %" GST_PTR_FORMAT, new_st);

  /* Crop if requested aspect ratio differs from incoming frame aspect ratio */
  if (self->src_zoom_crop) {

    ratio_w = (gdouble) in_width / req_width;
    ratio_h = (gdouble) in_height / req_height;

    if (ratio_w < ratio_h) {
      crop = in_height - (req_height * ratio_w);
      self->base_crop_top = crop / 2;
      self->base_crop_bottom = crop / 2;
    } else {
      crop = in_width - (req_width * ratio_h);
      self->base_crop_left = crop / 2;
      self->base_crop_right += crop / 2;
    }

    GST_INFO_OBJECT (self,
        "setting base crop: left:%d, right:%d, top:%d, bottom:%d",
        self->base_crop_left, self->base_crop_right, self->base_crop_top,
        self->base_crop_bottom);
    g_object_set (G_OBJECT (self->src_zoom_crop),
        "top", self->base_crop_top,
        "bottom", self->base_crop_bottom,
        "left", self->base_crop_left, "right", self->base_crop_right, NULL);
  }

  /* Update capsfilters */
  gst_caps_replace (&self->image_capture_caps,
      gst_caps_new_full (new_st, NULL));
  set_capsfilter_caps (self, self->image_capture_caps);

  /* Adjust the capsfilter before crop and videoscale elements if necessary */
  if (in_width == bcamsrc->width && in_height == bcamsrc->height) {
    GST_DEBUG_OBJECT (self, "no adaptation with resolution needed");
  } else {
    GST_DEBUG_OBJECT (self,
        "changing %" GST_PTR_FORMAT " from %dx%d to %dx%d", self->src_filter,
        bcamsrc->width, bcamsrc->height, in_width, in_height);
    /* Apply the width and height to filter caps */
    g_object_get (G_OBJECT (self->src_filter), "caps", &filter_caps, NULL);
    filter_caps = gst_caps_make_writable (filter_caps);
    gst_caps_set_simple (filter_caps, "width", G_TYPE_INT, in_width, "height",
        G_TYPE_INT, in_height, NULL);
    g_object_set (G_OBJECT (self->src_filter), "caps", filter_caps, NULL);
    gst_caps_unref (filter_caps);
  }
}

/**
 * img_capture_prepared:
 * @data: camerasrc object
 * @caps: caps describing the prepared image format
 *
 * Callback which is called after image capture has been prepared.
 */
static void
img_capture_prepared (gpointer data, GstCaps * caps)
{
  GstWrapperCameraBinSrc *self = GST_WRAPPER_CAMERA_BIN_SRC (data);

  GST_INFO_OBJECT (self, "image capture prepared");

  /* It is possible we are about to get something else that we requested */
  if (!gst_caps_is_equal (self->image_capture_caps, caps)) {
    adapt_image_capture (self, caps);
  } else {
    set_capsfilter_caps (self, self->image_capture_caps);
  }
}

/**
 *
 */
static gboolean
start_image_capture (GstWrapperCameraBinSrc * self)
{
  GstBaseCameraSrc *bcamsrc = GST_BASE_CAMERA_SRC (self);
  GstPhotography *photography = gst_base_camera_src_get_photography (bcamsrc);
  gboolean ret = FALSE;

  if (photography) {

    if (!self->image_capture_caps || self->image_capture_caps_update) {
      /* Capture resolution not set. Use viewfinder resolution */
      self->image_capture_caps = gst_caps_copy (self->view_finder_caps);
      self->image_capture_caps_update = FALSE;
    }

    /* Start preparations for image capture */
    GST_DEBUG_OBJECT (self, "prepare image capture caps %" GST_PTR_FORMAT,
        self->image_capture_caps);

    ret = gst_photography_prepare_for_capture (photography,
        (GstPhotoCapturePrepared) img_capture_prepared,
        self->image_capture_caps, self);

  } else {
    ret = TRUE;
  }

  return ret;
}

static gboolean
gst_wrapper_camera_bin_src_set_mode (GstBaseCameraSrc * bcamsrc,
    GstCameraBinMode mode)
{
  GstPhotography *photography = gst_base_camera_src_get_photography (bcamsrc);

  if (photography) {
    if (g_object_class_find_property (G_OBJECT_GET_CLASS (photography),
            "capture-mode")) {
      g_object_set (G_OBJECT (photography), "capture-mode", mode, NULL);
    }
  }
  return TRUE;
}

static gboolean
set_videosrc_zoom (GstWrapperCameraBinSrc * self, gint zoom)
{
  gboolean ret = FALSE;

  if (g_object_class_find_property (G_OBJECT_GET_CLASS (self->src_vid_src),
          "zoom")) {
    g_object_set (G_OBJECT (self->src_vid_src), "zoom",
        (gfloat) zoom / 100, NULL);
    ret = TRUE;
  }
  return ret;
}

static gboolean
set_element_zoom (GstWrapperCameraBinSrc * self, gint zoom)
{
  gboolean ret = FALSE;
  GstBaseCameraSrc *bcamsrc = GST_BASE_CAMERA_SRC (self);
  gint w2_crop = 0, h2_crop = 0;
  GstPad *pad_zoom_sink = NULL;
  gint left = self->base_crop_left;
  gint right = self->base_crop_right;
  gint top = self->base_crop_top;
  gint bottom = self->base_crop_bottom;

  if (self->src_zoom_crop) {
    /* Update capsfilters to apply the zoom */
    GST_INFO_OBJECT (self, "zoom: %d, orig size: %dx%d", zoom,
        bcamsrc->width, bcamsrc->height);

    if (zoom != ZOOM_1X) {
      w2_crop = (bcamsrc->width - (bcamsrc->width * ZOOM_1X / zoom)) / 2;
      h2_crop = (bcamsrc->height - (bcamsrc->height * ZOOM_1X / zoom)) / 2;

      left += w2_crop;
      right += w2_crop;
      top += h2_crop;
      bottom += h2_crop;

      /* force number of pixels cropped from left to be even, to avoid slow code
       * path on videoscale */
      left &= 0xFFFE;
    }

    pad_zoom_sink = gst_element_get_static_pad (self->src_zoom_crop, "sink");

    GST_INFO_OBJECT (self,
        "sw cropping: left:%d, right:%d, top:%d, bottom:%d", left, right, top,
        bottom);

    GST_PAD_STREAM_LOCK (pad_zoom_sink);
    g_object_set (self->src_zoom_crop, "left", left, "right", right, "top",
        top, "bottom", bottom, NULL);
    GST_PAD_STREAM_UNLOCK (pad_zoom_sink);
    gst_object_unref (pad_zoom_sink);
    ret = TRUE;
  }
  return ret;
}

static void
gst_wrapper_camera_bin_src_set_zoom (GstBaseCameraSrc * bcamsrc, gint zoom)
{
  GstWrapperCameraBinSrc *self = GST_WRAPPER_CAMERA_BIN_SRC (bcamsrc);

  GST_INFO_OBJECT (self, "setting zoom %d", zoom);

  if (set_videosrc_zoom (self, zoom)) {
    set_element_zoom (self, ZOOM_1X);
    GST_INFO_OBJECT (self, "zoom set using videosrc");
  } else if (set_element_zoom (self, zoom)) {
    GST_INFO_OBJECT (self, "zoom set using gst elements");
  } else {
    GST_INFO_OBJECT (self, "setting zoom failed");
  }
}

static GstCaps *
gst_wrapper_camera_bin_src_get_allowed_input_caps (GstBaseCameraSrc * bcamsrc)
{
  GstWrapperCameraBinSrc *self = GST_WRAPPER_CAMERA_BIN_SRC (bcamsrc);
  GstCaps *caps = NULL;
  GstPad *pad = NULL, *peer_pad = NULL;
  GstState state;
  GstElement *videosrc;

  videosrc = self->src_vid_src ? self->src_vid_src : self->app_vid_src;

  if (!videosrc) {
    GST_WARNING_OBJECT (self, "no videosrc, can't get allowed caps");
    goto failed;
  }

  if (self->allowed_caps) {
    GST_DEBUG_OBJECT (self, "returning cached caps");
    goto done;
  }

  pad = gst_element_get_static_pad (videosrc, "src");

  if (!pad) {
    GST_WARNING_OBJECT (self, "no srcpad in videosrc");
    goto failed;
  }

  state = GST_STATE (videosrc);

  /* Make this function work also in NULL state */
  if (state == GST_STATE_NULL) {
    GST_DEBUG_OBJECT (self, "setting videosrc to ready temporarily");
    peer_pad = gst_pad_get_peer (pad);
    if (peer_pad) {
      gst_pad_unlink (pad, peer_pad);
    }
    /* Set videosrc to READY to open video device */
    gst_element_set_locked_state (videosrc, TRUE);
    gst_element_set_state (videosrc, GST_STATE_READY);
  }

  self->allowed_caps = gst_pad_get_caps (pad);

  /* Restore state and re-link if necessary */
  if (state == GST_STATE_NULL) {
    GST_DEBUG_OBJECT (self, "restoring videosrc state %d", state);
    /* Reset videosrc to NULL state, some drivers seem to need this */
    gst_element_set_state (videosrc, GST_STATE_NULL);
    if (peer_pad) {
      gst_pad_link (pad, peer_pad);
      gst_object_unref (peer_pad);
    }
    gst_element_set_locked_state (videosrc, FALSE);
  }

  gst_object_unref (pad);

done:
  if (self->allowed_caps) {
    caps = gst_caps_copy (self->allowed_caps);
  }
  GST_DEBUG_OBJECT (self, "allowed caps:%" GST_PTR_FORMAT, caps);
failed:
  return caps;
}

/**
 * update_aspect_filter:
 * @self: camerasrc object
 * @new_caps: new caps of next buffers arriving to view finder sink element
 *
 * Updates aspect ratio capsfilter to maintain aspect ratio, if we need to
 * scale frames for showing them in view finder.
 */
static void
update_aspect_filter (GstWrapperCameraBinSrc * self, GstCaps * new_caps)
{
  // XXX why not instead add a preserve-aspect-ratio property to videoscale?
#if 0
  if (camera->flags & GST_CAMERABIN_FLAG_VIEWFINDER_SCALE) {
    GstCaps *sink_caps, *ar_caps;
    GstStructure *st;
    gint in_w = 0, in_h = 0, sink_w = 0, sink_h = 0, target_w = 0, target_h = 0;
    gdouble ratio_w, ratio_h;
    GstPad *sink_pad;
    const GValue *range;

    sink_pad = gst_element_get_static_pad (camera->view_sink, "sink");

    if (sink_pad) {
      sink_caps = gst_pad_get_caps (sink_pad);
      gst_object_unref (sink_pad);
      if (sink_caps) {
        if (!gst_caps_is_any (sink_caps)) {
          GST_DEBUG_OBJECT (camera, "sink element caps %" GST_PTR_FORMAT,
              sink_caps);
          /* Get maximum resolution that view finder sink accepts */
          st = gst_caps_get_structure (sink_caps, 0);
          if (gst_structure_has_field_typed (st, "width", GST_TYPE_INT_RANGE)) {
            range = gst_structure_get_value (st, "width");
            sink_w = gst_value_get_int_range_max (range);
          }
          if (gst_structure_has_field_typed (st, "height", GST_TYPE_INT_RANGE)) {
            range = gst_structure_get_value (st, "height");
            sink_h = gst_value_get_int_range_max (range);
          }
          GST_DEBUG_OBJECT (camera, "sink element accepts max %dx%d", sink_w,
              sink_h);

          /* Get incoming frames' resolution */
          if (sink_h && sink_w) {
            st = gst_caps_get_structure (new_caps, 0);
            gst_structure_get_int (st, "width", &in_w);
            gst_structure_get_int (st, "height", &in_h);
            GST_DEBUG_OBJECT (camera, "new caps with %dx%d", in_w, in_h);
          }
        }
        gst_caps_unref (sink_caps);
      }
    }

    /* If we get bigger frames than view finder sink accepts, then we scale.
       If we scale we need to adjust aspect ratio capsfilter caps in order
       to maintain aspect ratio while scaling. */
    if (in_w && in_h && (in_w > sink_w || in_h > sink_h)) {
      ratio_w = (gdouble) sink_w / in_w;
      ratio_h = (gdouble) sink_h / in_h;

      if (ratio_w < ratio_h) {
        target_w = sink_w;
        target_h = (gint) (ratio_w * in_h);
      } else {
        target_w = (gint) (ratio_h * in_w);
        target_h = sink_h;
      }

      GST_DEBUG_OBJECT (camera, "setting %dx%d filter to maintain aspect ratio",
          target_w, target_h);
      ar_caps = gst_caps_copy (new_caps);
      gst_caps_set_simple (ar_caps, "width", G_TYPE_INT, target_w, "height",
          G_TYPE_INT, target_h, NULL);
    } else {
      GST_DEBUG_OBJECT (camera, "no scaling");
      ar_caps = new_caps;
    }

    GST_DEBUG_OBJECT (camera, "aspect ratio filter caps %" GST_PTR_FORMAT,
        ar_caps);
    g_object_set (G_OBJECT (camera->aspect_filter), "caps", ar_caps, NULL);
    if (ar_caps != new_caps)
      gst_caps_unref (ar_caps);
  }
#endif
}


/**
 * set_capsfilter_caps:
 * @self: camerasrc object
 * @new_caps: pointer to caps object to set
 *
 * Set given caps to camerabin capsfilters.
 */
static void
set_capsfilter_caps (GstWrapperCameraBinSrc * self, GstCaps * new_caps)
{
  GST_INFO_OBJECT (self, "new_caps:%" GST_PTR_FORMAT, new_caps);

  /* Update zoom */
  gst_base_camera_src_setup_zoom (GST_BASE_CAMERA_SRC (self));

  /* Update capsfilters */
  g_object_set (G_OBJECT (self->src_filter), "caps", new_caps, NULL);
  if (self->src_zoom_filter)
    g_object_set (G_OBJECT (self->src_zoom_filter), "caps", new_caps, NULL);
  update_aspect_filter (self, new_caps);
  GST_INFO_OBJECT (self, "udpated");
}

static gboolean
gst_wrapper_camera_bin_src_start_capture (GstBaseCameraSrc * camerasrc)
{
  GstWrapperCameraBinSrc *src = GST_WRAPPER_CAMERA_BIN_SRC (camerasrc);

  /* TODO shoud we access this directly? Maybe a macro is better? */
  if (camerasrc->mode == MODE_IMAGE) {
    src->image_capture_count = 1;
    start_image_capture (src);
  } else if (camerasrc->mode == MODE_VIDEO) {
    if (src->video_rec_status == GST_VIDEO_RECORDING_STATUS_DONE) {
      src->video_rec_status = GST_VIDEO_RECORDING_STATUS_STARTING;
    }
  } else {
    g_assert_not_reached ();
    return FALSE;
  }
  return TRUE;
}

static void
gst_wrapper_camera_bin_src_stop_capture (GstBaseCameraSrc * camerasrc)
{
  GstWrapperCameraBinSrc *src = GST_WRAPPER_CAMERA_BIN_SRC (camerasrc);

  /* TODO shoud we access this directly? Maybe a macro is better? */
  if (camerasrc->mode == MODE_VIDEO) {
    if (src->video_rec_status == GST_VIDEO_RECORDING_STATUS_STARTING) {
      GST_DEBUG_OBJECT (src, "Aborting, had not started recording");
      src->video_rec_status = GST_VIDEO_RECORDING_STATUS_DONE;

    } else if (src->video_rec_status == GST_VIDEO_RECORDING_STATUS_RUNNING) {
      GST_DEBUG_OBJECT (src, "Marking video recording as finishing");
      src->video_rec_status = GST_VIDEO_RECORDING_STATUS_FINISHING;
    }
  } else {
    src->image_capture_count = 0;
  }
}

static void
gst_wrapper_camera_bin_src_base_init (gpointer g_class)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (g_class);

  GST_DEBUG_CATEGORY_INIT (wrapper_camera_bin_src_debug, "wrappercamerabinsrc",
      0, "V4l2 camera src");

  gst_element_class_set_details_simple (gstelement_class,
      "V4l2 camera src element for camerabin", "Source/Video",
      "V4l2 camera src element for camerabin", "Rob Clark <rob@ti.com>");
}

static void
gst_wrapper_camera_bin_src_class_init (GstWrapperCameraBinSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseCameraSrcClass *gstbasecamerasrc_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstbasecamerasrc_class = GST_BASE_CAMERA_SRC_CLASS (klass);

  gobject_class->dispose = gst_wrapper_camera_bin_src_dispose;
  gobject_class->finalize =
      (GObjectFinalizeFunc) gst_wrapper_camera_bin_src_finalize;
  gobject_class->set_property = gst_wrapper_camera_bin_src_set_property;
  gobject_class->get_property = gst_wrapper_camera_bin_src_get_property;

  /* g_object_class_install_property .... */
  g_object_class_install_property (gobject_class, PROP_VIDEO_SRC,
      g_param_spec_object ("video-src", "Video source",
          "The video source element to be used",
          GST_TYPE_ELEMENT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstbasecamerasrc_class->construct_pipeline =
      gst_wrapper_camera_bin_src_construct_pipeline;
  gstbasecamerasrc_class->set_zoom = gst_wrapper_camera_bin_src_set_zoom;
  gstbasecamerasrc_class->set_mode = gst_wrapper_camera_bin_src_set_mode;
  gstbasecamerasrc_class->get_allowed_input_caps =
      gst_wrapper_camera_bin_src_get_allowed_input_caps;
  gstbasecamerasrc_class->start_capture =
      gst_wrapper_camera_bin_src_start_capture;
  gstbasecamerasrc_class->stop_capture =
      gst_wrapper_camera_bin_src_stop_capture;
}

static void
gst_wrapper_camera_bin_src_init (GstWrapperCameraBinSrc * self,
    GstWrapperCameraBinSrcClass * klass)
{
  self->vfsrc =
      gst_ghost_pad_new_no_target (GST_BASE_CAMERA_SRC_VIEWFINDER_PAD_NAME,
      GST_PAD_SRC);
  gst_element_add_pad (GST_ELEMENT (self), self->vfsrc);

  self->imgsrc =
      gst_ghost_pad_new_no_target (GST_BASE_CAMERA_SRC_IMAGE_PAD_NAME,
      GST_PAD_SRC);
  gst_element_add_pad (GST_ELEMENT (self), self->imgsrc);

  self->vidsrc =
      gst_ghost_pad_new_no_target (GST_BASE_CAMERA_SRC_VIDEO_PAD_NAME,
      GST_PAD_SRC);
  gst_element_add_pad (GST_ELEMENT (self), self->vidsrc);


  /* TODO where are variables reset? */
  self->image_capture_count = 0;
  self->video_rec_status = GST_VIDEO_RECORDING_STATUS_DONE;
}

gboolean
gst_wrapper_camera_bin_src_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "wrappercamerabinsrc", GST_RANK_NONE,
      gst_wrapper_camera_bin_src_get_type ());
}
