/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include "kmsutils.h"
#include "kmselement.h"
#include "kmsagnosticcaps.h"
#include "kmsplayerendpoint.h"
#include "kmsloop.h"

#define PLUGIN_NAME "playerendpoint"
#define AUDIO_APPSRC "audio_appsrc"
#define VIDEO_APPSRC "video_appsrc"
#define URIDECODEBIN "uridecodebin"

#define APPSRC_DATA "appsrc_data"
#define APPSINK_DATA "appsink_data"
#define BASE_TIME_DATA "base_time_data"

GST_DEBUG_CATEGORY_STATIC (kms_player_endpoint_debug_category);
#define GST_CAT_DEFAULT kms_player_endpoint_debug_category

#define KMS_PLAYER_ENDPOINT_GET_PRIVATE(obj) (  \
  G_TYPE_INSTANCE_GET_PRIVATE (                 \
    (obj),                                      \
    KMS_TYPE_PLAYER_ENDPOINT,                   \
    KmsPlayerEndpointPrivate                    \
  )                                             \
)

typedef void (*KmsActionFunc) (gpointer user_data);

struct _KmsPlayerEndpointPrivate
{
  GstElement *pipeline;
  GstElement *uridecodebin;
  KmsLoop *loop;
  gboolean use_encoded_media;
};

enum
{
  PROP_0,
  PROP_USE_ENCODED_MEDIA,
  N_PROPERTIES
};

enum
{
  SIGNAL_EOS,
  SIGNAL_INVALID_URI,
  SIGNAL_INVALID_MEDIA,
  LAST_SIGNAL
};

static guint kms_player_endpoint_signals[LAST_SIGNAL] = { 0 };

/* pad templates */

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsPlayerEndpoint, kms_player_endpoint,
    KMS_TYPE_URI_ENDPOINT,
    GST_DEBUG_CATEGORY_INIT (kms_player_endpoint_debug_category, PLUGIN_NAME,
        0, "debug category for playerendpoint element"));

static void
kms_player_endpoint_set_caps (KmsPlayerEndpoint * self)
{
  GstCaps *deco_caps;

  deco_caps = gst_caps_from_string (KMS_AGNOSTIC_CAPS_CAPS);
  g_object_set (G_OBJECT (self->priv->uridecodebin), "caps", deco_caps, NULL);
  gst_caps_unref (deco_caps);
}

void
kms_player_endpoint_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  KmsPlayerEndpoint *playerendpoint = KMS_PLAYER_ENDPOINT (object);

  switch (property_id) {
    case PROP_USE_ENCODED_MEDIA:{
      playerendpoint->priv->use_encoded_media = g_value_get_boolean (value);
      if (playerendpoint->priv->use_encoded_media) {
        kms_player_endpoint_set_caps (playerendpoint);
      }
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
kms_player_endpoint_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  KmsPlayerEndpoint *playerendpoint = KMS_PLAYER_ENDPOINT (object);

  switch (property_id) {
    case PROP_USE_ENCODED_MEDIA:
      g_value_set_boolean (value, playerendpoint->priv->use_encoded_media);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
kms_player_endpoint_dispose (GObject * object)
{
  KmsPlayerEndpoint *self = KMS_PLAYER_ENDPOINT (object);

  g_clear_object (&self->priv->loop);

  if (self->priv->pipeline != NULL) {
    GstBus *bus;

    bus = gst_pipeline_get_bus (GST_PIPELINE (self->priv->pipeline));
    gst_bus_set_sync_handler (bus, NULL, NULL, NULL);
    g_object_unref (bus);

    gst_element_set_state (self->priv->pipeline, GST_STATE_NULL);
    gst_object_unref (GST_OBJECT (self->priv->pipeline));
    self->priv->pipeline = NULL;
  }

  /* clean up as possible. May be called multiple times */

  G_OBJECT_CLASS (kms_player_endpoint_parent_class)->dispose (object);
}

static void
release_gst_clock (gpointer data)
{
  g_slice_free (GstClockTime, data);
}

static GstFlowReturn
new_sample_cb (GstElement * appsink, gpointer user_data)
{
  GstElement *appsrc = GST_ELEMENT (user_data);
  GstFlowReturn ret;
  GstSample *sample;
  GstBuffer *buffer;
  GstClockTime *base_time;
  GstPad *src, *sink;

  g_signal_emit_by_name (appsink, "pull-sample", &sample);

  if (sample == NULL)
    return GST_FLOW_OK;

  buffer = gst_sample_get_buffer (sample);

  if (buffer == NULL) {
    ret = GST_FLOW_OK;
    goto end;
  }

  gst_buffer_ref (buffer);

  buffer = gst_buffer_make_writable (buffer);

  KMS_ELEMENT_LOCK (GST_OBJECT_PARENT (appsrc));

  base_time =
      g_object_get_data (G_OBJECT (GST_OBJECT_PARENT (appsrc)), BASE_TIME_DATA);

  if (base_time == NULL) {
    GstClock *clock;

    clock = gst_element_get_clock (appsrc);
    base_time = g_slice_new0 (GstClockTime);

    g_object_set_data_full (G_OBJECT (GST_OBJECT_PARENT (appsrc)),
        BASE_TIME_DATA, base_time, release_gst_clock);
    *base_time =
        gst_clock_get_time (clock) - gst_element_get_base_time (appsrc);
    g_object_unref (clock);
    GST_DEBUG ("Setting base time to: %" G_GUINT64_FORMAT, *base_time);
  }

  src = gst_element_get_static_pad (appsrc, "src");
  sink = gst_pad_get_peer (src);

  if (sink != NULL) {
    if (GST_OBJECT_FLAG_IS_SET (sink, GST_PAD_FLAG_EOS)) {
      GST_INFO_OBJECT (sink, "Sending flush events");
      gst_pad_send_event (sink, gst_event_new_flush_start ());
      gst_pad_send_event (sink, gst_event_new_flush_stop (FALSE));
    }
    g_object_unref (sink);
  }

  g_object_unref (src);

  if (GST_BUFFER_PTS_IS_VALID (buffer))
    buffer->pts += *base_time;
  if (GST_BUFFER_DTS_IS_VALID (buffer))
    buffer->dts += *base_time;

  KMS_ELEMENT_UNLOCK (GST_OBJECT_PARENT (appsrc));

  // TODO: Do something to fix a possible previous EOS event
  g_signal_emit_by_name (appsrc, "push-buffer", buffer, &ret);

  gst_buffer_unref (buffer);

  if (ret != GST_FLOW_OK) {
    /* something wrong */
    GST_ERROR ("Could not send buffer to appsrc %s. Cause: %s",
        GST_ELEMENT_NAME (appsrc), gst_flow_get_name (ret));
  }

end:
  if (sample != NULL)
    gst_sample_unref (sample);

  return ret;
}

static void
eos_cb (GstElement * appsink, gpointer user_data)
{
  GstElement *appsrc = GST_ELEMENT (user_data);
  GstPad *srcpad;

  GST_DEBUG_OBJECT (appsrc, "Sending eos event to main pipeline");

  srcpad = gst_element_get_static_pad (appsrc, "src");
  if (srcpad == NULL) {
    GST_ERROR ("Can not get source pad from %s", GST_ELEMENT_NAME (appsrc));
    return;
  }

  if (!gst_pad_push_event (srcpad, gst_event_new_eos ()))
    GST_ERROR ("EOS event could not be sent");

  g_object_unref (srcpad);
}

static void
pad_added (GstElement * element, GstPad * pad, KmsPlayerEndpoint * self)
{
  GST_DEBUG ("Pad added");
  GstElement *appsrc, *agnosticbin, *appsink;
  GstPad *sinkpad;
  GstCaps *audio_caps, *video_caps;
  GstCaps *src_caps;

  /* Create and link appsrc--agnosticbin with proper caps */
  audio_caps = gst_caps_from_string (KMS_AGNOSTIC_AUDIO_CAPS);
  video_caps = gst_caps_from_string (KMS_AGNOSTIC_VIDEO_CAPS);
  src_caps = gst_pad_query_caps (pad, NULL);
  GST_TRACE ("caps are %" GST_PTR_FORMAT, src_caps);

  if (gst_caps_can_intersect (audio_caps, src_caps))
    agnosticbin = kms_element_get_audio_agnosticbin (KMS_ELEMENT (self));
  else if (gst_caps_can_intersect (video_caps, src_caps))
    agnosticbin = kms_element_get_video_agnosticbin (KMS_ELEMENT (self));
  else {
    GST_ELEMENT_WARNING (self, CORE, CAPS,
        ("Unsupported media received: %" GST_PTR_FORMAT, src_caps),
        ("Unsupported media received: %" GST_PTR_FORMAT, src_caps));
    goto end;
  }

  /* Create appsrc element and link to agnosticbin */
  appsrc = gst_element_factory_make ("appsrc", NULL);
  g_object_set (G_OBJECT (appsrc), "is-live", TRUE, "do-timestamp", FALSE,
      "min-latency", G_GUINT64_CONSTANT (0),
      "max-latency", G_GUINT64_CONSTANT (0), "format", GST_FORMAT_TIME,
      "caps", src_caps, NULL);

  gst_bin_add (GST_BIN (self), appsrc);
  gst_element_sync_state_with_parent (appsrc);
  if (!gst_element_link (appsrc, agnosticbin)) {
    GST_ERROR ("Could not link %s to element %s", GST_ELEMENT_NAME (appsrc),
        GST_ELEMENT_NAME (agnosticbin));
  }

  /* Create appsink and link to pad */
  appsink = gst_element_factory_make ("appsink", NULL);
  g_object_set (appsink, "sync", TRUE, "enable-last-sample",
      FALSE, "emit-signals", TRUE, "qos", TRUE, "max-buffers", 1,
      "async", FALSE, NULL);
  gst_bin_add (GST_BIN (self->priv->pipeline), appsink);
  gst_element_sync_state_with_parent (appsink);

  sinkpad = gst_element_get_static_pad (appsink, "sink");
  gst_pad_link (pad, sinkpad);
  GST_DEBUG_OBJECT (self, "Linked %s---%s", GST_ELEMENT_NAME (element),
      GST_ELEMENT_NAME (appsink));
  g_object_unref (sinkpad);

  g_object_set_data (G_OBJECT (pad), APPSRC_DATA, appsrc);
  g_object_set_data (G_OBJECT (pad), APPSINK_DATA, appsink);

  /* Connect new-sample signal to callback */
  g_signal_connect (appsink, "new-sample", G_CALLBACK (new_sample_cb), appsrc);
  g_signal_connect (appsink, "eos", G_CALLBACK (eos_cb), appsrc);

end:
  if (src_caps != NULL)
    gst_caps_unref (src_caps);

  if (audio_caps != NULL)
    gst_caps_unref (audio_caps);

  if (video_caps != NULL)
    gst_caps_unref (video_caps);
}

static void
pad_removed (GstElement * element, GstPad * pad, KmsPlayerEndpoint * self)
{
  GST_DEBUG ("Pad removed");
  GstElement *appsink, *appsrc;

  if (GST_PAD_IS_SINK (pad))
    return;

  appsink = g_object_steal_data (G_OBJECT (pad), APPSINK_DATA);
  appsrc = g_object_steal_data (G_OBJECT (pad), APPSRC_DATA);

  if (appsrc != NULL) {
    GST_INFO ("Removing %" GST_PTR_FORMAT " from its parent", appsrc);
    if (GST_OBJECT_PARENT (appsrc) != NULL) {
      g_object_ref (appsrc);
      gst_bin_remove (GST_BIN (GST_OBJECT_PARENT (appsrc)), appsrc);
      gst_element_set_state (appsrc, GST_STATE_NULL);
      g_object_unref (appsrc);
    }
  }

  if (appsink == NULL) {
    GST_ERROR ("No appsink was found associated with %" GST_PTR_FORMAT, pad);
    return;
  }
  if (!gst_element_set_locked_state (appsink, TRUE))
    GST_ERROR ("Could not block element %s", GST_ELEMENT_NAME (appsink));

  GST_DEBUG ("Removing appsink %s from %s", GST_ELEMENT_NAME (appsink),
      GST_ELEMENT_NAME (self->priv->pipeline));

  gst_element_set_state (appsink, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (self->priv->pipeline), appsink);
}

static void
kms_player_endpoint_stopped (KmsUriEndpoint * obj)
{
  KmsPlayerEndpoint *self = KMS_PLAYER_ENDPOINT (obj);

  /* Set internal pipeline to NULL */
  gst_element_set_state (self->priv->pipeline, GST_STATE_NULL);
  KMS_ELEMENT_LOCK (self);
  g_object_set_data (G_OBJECT (self), BASE_TIME_DATA, NULL);
  KMS_ELEMENT_UNLOCK (self);

  KMS_URI_ENDPOINT_GET_CLASS (self)->change_state (KMS_URI_ENDPOINT (self),
      KMS_URI_ENDPOINT_STATE_STOP);
}

static void
kms_player_endpoint_started (KmsUriEndpoint * obj)
{
  KmsPlayerEndpoint *self = KMS_PLAYER_ENDPOINT (obj);

  /* Set uri property in uridecodebin */
  g_object_set (G_OBJECT (self->priv->uridecodebin), "uri",
      KMS_URI_ENDPOINT (self)->uri, NULL);

  /* Set internal pipeline to playing */
  gst_element_set_state (self->priv->pipeline, GST_STATE_PLAYING);

  KMS_URI_ENDPOINT_GET_CLASS (self)->change_state (KMS_URI_ENDPOINT (self),
      KMS_URI_ENDPOINT_STATE_START);
}

static void
kms_player_endpoint_paused (KmsUriEndpoint * obj)
{
  KmsPlayerEndpoint *self = KMS_PLAYER_ENDPOINT (obj);

  /* Set internal pipeline to paused */
  gst_element_set_state (self->priv->pipeline, GST_STATE_PAUSED);

  KMS_URI_ENDPOINT_GET_CLASS (self)->change_state (KMS_URI_ENDPOINT (self),
      KMS_URI_ENDPOINT_STATE_PAUSE);
}

static void
kms_player_endpoint_class_init (KmsPlayerEndpointClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  KmsUriEndpointClass *urienpoint_class = KMS_URI_ENDPOINT_CLASS (klass);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "PlayerEndpoint", "Sink/Generic", "Kurento plugin player end point",
      "Joaquin Mengual García <kini.mengual@gmail.com>");

  gobject_class->dispose = kms_player_endpoint_dispose;
  gobject_class->set_property = kms_player_endpoint_set_property;
  gobject_class->get_property = kms_player_endpoint_get_property;

  urienpoint_class->stopped = kms_player_endpoint_stopped;
  urienpoint_class->started = kms_player_endpoint_started;
  urienpoint_class->paused = kms_player_endpoint_paused;

  g_object_class_install_property (gobject_class, PROP_USE_ENCODED_MEDIA,
      g_param_spec_boolean ("use-encoded-media", "use encoded media",
          "The element uses encoded media instead of raw media. This mode "
          "could have an unexpected behaviour if key frames are lost",
          FALSE, G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY));

  kms_player_endpoint_signals[SIGNAL_EOS] =
      g_signal_new ("eos",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (KmsPlayerEndpointClass, eos_signal), NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  kms_player_endpoint_signals[SIGNAL_INVALID_URI] =
      g_signal_new ("invalid-uri",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (KmsPlayerEndpointClass, invalid_uri_signal), NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  kms_player_endpoint_signals[SIGNAL_INVALID_MEDIA] =
      g_signal_new ("invalid-media",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (KmsPlayerEndpointClass, invalid_media_signal), NULL,
      NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  /* Registers a private structure for the instantiatable type */
  g_type_class_add_private (klass, sizeof (KmsPlayerEndpointPrivate));
}

static gboolean
kms_player_endpoint_emit_EOS_signal (gpointer data)
{
  GST_DEBUG ("Emit EOS Signal");
  kms_player_endpoint_stopped (KMS_URI_ENDPOINT (data));
  g_signal_emit (G_OBJECT (data), kms_player_endpoint_signals[SIGNAL_EOS], 0);

  return G_SOURCE_REMOVE;
}

static gboolean
kms_player_endpoint_emit_invalid_uri_signal (gpointer data)
{
  KmsPlayerEndpoint *self = KMS_PLAYER_ENDPOINT (data);

  GST_DEBUG ("Emit invalid uri signal");
  g_signal_emit (G_OBJECT (self),
      kms_player_endpoint_signals[SIGNAL_INVALID_URI], 0);

  return G_SOURCE_REMOVE;
}

static gboolean
kms_player_endpoint_emit_invalid_media_signal (gpointer data)
{
  KmsPlayerEndpoint *self = KMS_PLAYER_ENDPOINT (data);

  GST_DEBUG ("Emit invalid media signal");
  g_signal_emit (G_OBJECT (self),
      kms_player_endpoint_signals[SIGNAL_INVALID_MEDIA], 0);

  return G_SOURCE_REMOVE;
}

static gboolean
kms_player_endpoint_post_media_error (gpointer data)
{
  KmsPlayerEndpoint *self = KMS_PLAYER_ENDPOINT (data);

  GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("Wrong video format"), (NULL));

  return G_SOURCE_REMOVE;
}

static GstBusSyncReply
bus_sync_signal_handler (GstBus * bus, GstMessage * msg, gpointer data)
{
  KmsPlayerEndpoint *self = KMS_PLAYER_ENDPOINT (data);

  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS) {
    kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_HIGH_IDLE,
        kms_player_endpoint_emit_EOS_signal, g_object_ref (self),
        g_object_unref);
  } else if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {

    if (g_str_has_prefix (GST_OBJECT_NAME (msg->src), "decodebin")) {
      kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_HIGH_IDLE,
          kms_player_endpoint_emit_invalid_media_signal, g_object_ref (self),
          g_object_unref);
    } else if (g_strcmp0 (GST_OBJECT_NAME (msg->src), "source") == 0) {
      kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_HIGH_IDLE,
          kms_player_endpoint_emit_invalid_uri_signal, g_object_ref (self),
          g_object_unref);
    } else {
      kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_HIGH_IDLE,
          kms_player_endpoint_post_media_error, g_object_ref (self),
          g_object_unref);
    }
  }
  return GST_BUS_PASS;
}

static void
kms_player_endpoint_init (KmsPlayerEndpoint * self)
{
  GstBus *bus;

  self->priv = KMS_PLAYER_ENDPOINT_GET_PRIVATE (self);

  self->priv->loop = kms_loop_new ();
  self->priv->pipeline = gst_pipeline_new ("pipeline");
  self->priv->uridecodebin =
      gst_element_factory_make ("uridecodebin", URIDECODEBIN);

  gst_bin_add (GST_BIN (self->priv->pipeline), self->priv->uridecodebin);

  bus = gst_pipeline_get_bus (GST_PIPELINE (self->priv->pipeline));
  gst_bus_set_sync_handler (bus, bus_sync_signal_handler, self, NULL);
  g_object_unref (bus);

  /* Connect to signals */
  g_signal_connect (self->priv->uridecodebin, "pad-added",
      G_CALLBACK (pad_added), self);
  g_signal_connect (self->priv->uridecodebin, "pad-removed",
      G_CALLBACK (pad_removed), self);
}

gboolean
kms_player_endpoint_plugin_init (GstPlugin * plugin)
{

  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_PLAYER_ENDPOINT);
}
