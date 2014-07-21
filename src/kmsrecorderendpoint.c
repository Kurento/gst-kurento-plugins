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
#include <gst/pbutils/encoding-profile.h>
#include <libsoup/soup.h>

#include "kmsagnosticcaps.h"
#include "kmsrecorderendpoint.h"
#include "kmsuriendpointstate.h"
#include "kmsutils.h"
#include "kmsloop.h"
#include "kmsrecordingprofile.h"
#include "kms-enumtypes.h"
#include "kmsconfcontroller.h"

#define PLUGIN_NAME "recorderendpoint"

#define AUDIO_APPSINK "audio_appsink"
#define AUDIO_APPSRC "audio_appsrc"
#define VIDEO_APPSINK "video_appsink"
#define VIDEO_APPSRC "video_appsrc"

#define KEY_RECORDER_PAD_PROBE_ID "kms-recorder-pad-key-probe-id"

#define BASE_TIME_DATA "base_time_data"

#define HTTP_PROTO "http"
#define HTTPS_PROTO "https"

#define DEFAULT_RECORDING_PROFILE KMS_RECORDING_PROFILE_WEBM

#define HTTP_TIMEOUT 10
#define MEGA_BYTES(n) ((n) * 1000000)

GST_DEBUG_CATEGORY_STATIC (kms_recorder_endpoint_debug_category);
#define GST_CAT_DEFAULT kms_recorder_endpoint_debug_category

#define KMS_RECORDER_ENDPOINT_GET_PRIVATE(obj) (  \
  G_TYPE_INSTANCE_GET_PRIVATE (                   \
    (obj),                                        \
    KMS_TYPE_RECORDER_ENDPOINT,                   \
    KmsRecorderEndpointPrivate                    \
  )                                               \
)

enum
{
  PROP_0,
  PROP_DVR,
  PROP_PROFILE,
  N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

struct state_controller
{
  GCond cond;
  GMutex mutex;
  guint locked;
  gboolean changing;
};

struct _KmsRecorderEndpointPrivate
{
  GstElement *pipeline;
  GstClockTime paused_time;
  GstClockTime paused_start;
  gboolean use_dvr;
  struct state_controller state_manager;
  KmsLoop *loop;
  KmsConfController *controller;
};

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsRecorderEndpoint, kms_recorder_endpoint,
    KMS_TYPE_URI_ENDPOINT,
    GST_DEBUG_CATEGORY_INIT (kms_recorder_endpoint_debug_category, PLUGIN_NAME,
        0, "debug category for recorderendpoint element"));

static void
destroy_ulong (gpointer data)
{
  g_slice_free (gulong, data);
}

static void
send_eos (GstElement * appsrc)
{
  GstFlowReturn ret;

  GST_DEBUG ("Send EOS to %s", GST_ELEMENT_NAME (appsrc));

  g_signal_emit_by_name (appsrc, "end-of-stream", &ret);
  if (ret != GST_FLOW_OK) {
    /* something wrong */
    GST_ERROR ("Could not send EOS to appsrc  %s. Ret code %d",
        GST_ELEMENT_NAME (appsrc), ret);
  }
}

typedef struct _BaseTimeType
{
  GstClockTime pts;
  GstClockTime dts;
} BaseTimeType;

static void
release_base_time_type (gpointer data)
{
  g_slice_free (BaseTimeType, data);
}

static GstFlowReturn
recv_sample (GstElement * appsink, gpointer user_data)
{
  KmsRecorderEndpoint *self =
      KMS_RECORDER_ENDPOINT (GST_OBJECT_PARENT (appsink));
  GstElement *appsrc = GST_ELEMENT (user_data);
  KmsUriEndpointState state;
  GstFlowReturn ret;
  GstSample *sample;
  GstBuffer *buffer;
  GstCaps *caps;
  BaseTimeType *base_time;

  g_signal_emit_by_name (appsink, "pull-sample", &sample);
  if (sample == NULL)
    return GST_FLOW_OK;

  g_object_get (G_OBJECT (appsrc), "caps", &caps, NULL);
  if (caps == NULL) {
    /* Appsrc has not yet caps defined */
    GstPad *sink_pad = gst_element_get_static_pad (appsink, "sink");

    if (sink_pad != NULL) {
      caps = gst_pad_get_current_caps (sink_pad);
      g_object_unref (sink_pad);
    }

    if (caps == NULL) {
      GST_ELEMENT_ERROR (self, CORE, CAPS, ("No caps found for %s",
              GST_ELEMENT_NAME (appsrc)), GST_ERROR_SYSTEM);
      ret = GST_FLOW_ERROR;
      goto end;
    }

    g_object_set (appsrc, "caps", caps, NULL);
  }

  gst_caps_unref (caps);

  buffer = gst_sample_get_buffer (sample);
  if (buffer == NULL) {
    ret = GST_FLOW_OK;
    goto end;
  }

  g_object_get (G_OBJECT (self), "state", &state, NULL);
  if (state != KMS_URI_ENDPOINT_STATE_START) {
    GST_DEBUG ("Dropping buffer %" GST_PTR_FORMAT, buffer);
    ret = GST_FLOW_OK;
    goto end;
  }

  gst_buffer_ref (buffer);
  buffer = gst_buffer_make_writable (buffer);

  KMS_ELEMENT_LOCK (GST_OBJECT_PARENT (appsink));

  base_time = g_object_get_data (G_OBJECT (appsrc), BASE_TIME_DATA);

  if (base_time == NULL) {
    base_time = g_slice_new0 (BaseTimeType);
    base_time->pts = GST_CLOCK_TIME_NONE;
    base_time->dts = GST_CLOCK_TIME_NONE;
    g_object_set_data_full (G_OBJECT (appsrc), BASE_TIME_DATA, base_time,
        release_base_time_type);
  }

  if (!GST_CLOCK_TIME_IS_VALID (base_time->pts)
      && GST_BUFFER_PTS_IS_VALID (buffer)) {
    base_time->pts = buffer->pts;
    GST_DEBUG_OBJECT (appsrc, "Setting pts base time to: %" G_GUINT64_FORMAT,
        base_time->pts);
  }

  if (!GST_CLOCK_TIME_IS_VALID (base_time->dts)
      && GST_BUFFER_DTS_IS_VALID (buffer)) {
    base_time->dts = buffer->dts;
    GST_DEBUG_OBJECT (appsrc, "Setting dts base time to: %" G_GUINT64_FORMAT,
        base_time->dts);
  }

  if (GST_CLOCK_TIME_IS_VALID (base_time->pts)
      && GST_BUFFER_PTS_IS_VALID (buffer)) {
    buffer->pts -= base_time->pts + self->priv->paused_time;
    buffer->dts = buffer->pts;
  } else if (GST_CLOCK_TIME_IS_VALID (base_time->dts)
      && GST_BUFFER_DTS_IS_VALID (buffer)) {
    buffer->dts -= base_time->dts + self->priv->paused_time;
    buffer->pts = buffer->dts;
  }

  g_object_set (self->priv->controller, "has_data", TRUE, NULL);

  KMS_ELEMENT_UNLOCK (GST_OBJECT_PARENT (appsink));

  GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_LIVE);

  if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_HEADER))
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);

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
recv_eos (GstElement * appsink, gpointer user_data)
{
  GstElement *appsrc = GST_ELEMENT (user_data);

  send_eos (appsrc);
}

static void
kms_recorder_endpoint_change_state (KmsRecorderEndpoint * self)
{
  KMS_ELEMENT_UNLOCK (KMS_ELEMENT (self));

  g_mutex_lock (&self->priv->state_manager.mutex);
  while (self->priv->state_manager.changing) {
    GST_WARNING ("Change of state is taking place");
    self->priv->state_manager.locked++;
    g_cond_wait (&self->priv->state_manager.cond,
        &self->priv->state_manager.mutex);
    self->priv->state_manager.locked--;
  }

  self->priv->state_manager.changing = TRUE;
  g_mutex_unlock (&self->priv->state_manager.mutex);

  KMS_ELEMENT_LOCK (KMS_ELEMENT (self));
}

static void
kms_recorder_endpoint_state_changed (KmsRecorderEndpoint * self,
    KmsUriEndpointState state)
{
  KMS_URI_ENDPOINT_GET_CLASS (self)->change_state (KMS_URI_ENDPOINT (self),
      state);
  KMS_ELEMENT_UNLOCK (KMS_ELEMENT (self));

  g_mutex_lock (&self->priv->state_manager.mutex);
  self->priv->state_manager.changing = FALSE;
  if (self->priv->state_manager.locked > 0)
    g_cond_signal (&self->priv->state_manager.cond);
  g_mutex_unlock (&self->priv->state_manager.mutex);

  KMS_ELEMENT_LOCK (KMS_ELEMENT (self));
}

static void
kms_recorder_endpoint_send_eos_to_appsrcs (KmsRecorderEndpoint * self)
{
  GstElement *audiosrc =
      gst_bin_get_by_name (GST_BIN (self->priv->pipeline), AUDIO_APPSRC);
  GstElement *videosrc =
      gst_bin_get_by_name (GST_BIN (self->priv->pipeline), VIDEO_APPSRC);

  if (audiosrc == NULL && videosrc == NULL) {
    gst_element_set_state (self->priv->pipeline, GST_STATE_NULL);
    kms_recorder_endpoint_state_changed (self, KMS_URI_ENDPOINT_STATE_STOP);
    return;
  }

  gst_element_set_state (self->priv->pipeline, GST_STATE_PLAYING);

  if (audiosrc != NULL) {
    send_eos (audiosrc);
    g_object_unref (audiosrc);
  }

  if (videosrc != NULL) {
    send_eos (videosrc);
    g_object_unref (videosrc);
  }
}

static gboolean
set_to_null_state_on_EOS (gpointer data)
{
  KmsRecorderEndpoint *recorder = KMS_RECORDER_ENDPOINT (data);

  GST_DEBUG ("Received EOS in pipeline, setting NULL state");

  KMS_ELEMENT_LOCK (KMS_ELEMENT (recorder));

  gst_element_set_state (recorder->priv->pipeline, GST_STATE_NULL);

  kms_recorder_endpoint_state_changed (recorder, KMS_URI_ENDPOINT_STATE_STOP);

  KMS_ELEMENT_UNLOCK (KMS_ELEMENT (recorder));

  return G_SOURCE_REMOVE;
}

static void
kms_recorder_endpoint_dispose (GObject * object)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (object);

  GST_DEBUG_OBJECT (self, "dispose");

  g_clear_object (&self->priv->loop);
  g_clear_object (&self->priv->controller);

  if (self->priv->pipeline != NULL) {
    if (GST_STATE (self->priv->pipeline) != GST_STATE_NULL) {
      GST_ELEMENT_WARNING (self, RESOURCE, BUSY,
          ("Recorder may have buffers to save"),
          ("Disposing recorder when it isn't stopped."));
    }
    gst_element_set_state (self->priv->pipeline, GST_STATE_NULL);
    g_object_unref (self->priv->pipeline);
    self->priv->pipeline = NULL;
  }

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (kms_recorder_endpoint_parent_class)->dispose (object);
}

static void
kms_recorder_endpoint_release_pending_requests (KmsRecorderEndpoint * self)
{
  g_mutex_lock (&self->priv->state_manager.mutex);
  while (self->priv->state_manager.changing ||
      self->priv->state_manager.locked > 0) {
    GST_WARNING ("Waiting to all process blocked");
    self->priv->state_manager.locked++;
    g_cond_wait (&self->priv->state_manager.cond,
        &self->priv->state_manager.mutex);
    self->priv->state_manager.locked--;
  }
  g_mutex_unlock (&self->priv->state_manager.mutex);

  g_cond_clear (&self->priv->state_manager.cond);
  g_mutex_clear (&self->priv->state_manager.mutex);
}

static void
kms_recorder_endpoint_finalize (GObject * object)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (object);

  GST_DEBUG_OBJECT (self, "finalize");

  kms_recorder_endpoint_release_pending_requests (self);

  G_OBJECT_CLASS (kms_recorder_endpoint_parent_class)->finalize (object);
}

static GstElement *
kms_recorder_endpoint_get_sink_fallback (KmsRecorderEndpoint * self)
{
  GstElement *sink = NULL;
  gchar *prot;

  prot = gst_uri_get_protocol (KMS_URI_ENDPOINT (self)->uri);

  if ((g_strcmp0 (prot, HTTP_PROTO) == 0)
      || (g_strcmp0 (prot, HTTPS_PROTO) == 0)) {
    SoupSession *ss;

    if (kms_is_valid_uri (KMS_URI_ENDPOINT (self)->uri)) {
      /* We use souphttpclientsink */
      sink = gst_element_factory_make ("souphttpclientsink", NULL);
      g_object_set (sink, "blocksize", MEGA_BYTES (1), NULL);
      ss = soup_session_new_with_options ("timeout", HTTP_TIMEOUT,
          "ssl-strict", FALSE, NULL);
      g_object_set (G_OBJECT (sink), "session", ss, NULL);
    } else {
      GST_ERROR ("URL not valid");
    }

  }
  /* Add more if required */
  return sink;
}

static GstElement *
kms_recorder_endpoint_get_sink (KmsRecorderEndpoint * self)
{
  GObjectClass *sink_class;
  GstElement *sink = NULL;
  GParamSpec *pspec;
  GError *err = NULL;

  KMS_ELEMENT_LOCK (KMS_ELEMENT (self));

  if (KMS_URI_ENDPOINT (self)->uri == NULL)
    goto no_uri;

  if (!gst_uri_is_valid (KMS_URI_ENDPOINT (self)->uri))
    goto invalid_uri;

  sink = gst_element_make_from_uri (GST_URI_SINK, KMS_URI_ENDPOINT (self)->uri,
      NULL, &err);
  if (sink == NULL) {
    /* Some elements have no URI handling capabilities though they can */
    /* handle them. We try to find such element before failing to attend */
    /* this request */
    sink = kms_recorder_endpoint_get_sink_fallback (self);
    if (sink == NULL)
      goto no_sink;
  }

  /* Try to configure the sink element */
  sink_class = G_OBJECT_GET_CLASS (sink);

  pspec = g_object_class_find_property (sink_class, "location");
  if (pspec != NULL && G_PARAM_SPEC_VALUE_TYPE (pspec) == G_TYPE_STRING) {
    if (g_strcmp0 (GST_OBJECT_NAME (gst_element_get_factory (sink)),
            "filesink") == 0) {
      /* Work around for filesink elements */
      gchar *location = gst_uri_get_location (KMS_URI_ENDPOINT (self)->uri);

      GST_DEBUG_OBJECT (sink, "filesink location=%s", location);
      g_object_set (sink, "location", location, NULL);
      g_free (location);
    } else {
      GST_DEBUG_OBJECT (sink, "configuring location=%s",
          KMS_URI_ENDPOINT (self)->uri);
      g_object_set (sink, "location", KMS_URI_ENDPOINT (self)->uri, NULL);
    }
  }

  goto end;

no_uri:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("No URI specified to record to."), GST_ERROR_SYSTEM);
    goto end;
  }
invalid_uri:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("Invalid URI \"%s\".",
            KMS_URI_ENDPOINT (self)->uri), GST_ERROR_SYSTEM);
    g_clear_error (&err);
    goto end;
  }
no_sink:
  {
    /* whoops, could not create the sink element, dig a little deeper to
     * figure out what might be wrong. */
    if (err != NULL && err->code == GST_URI_ERROR_UNSUPPORTED_PROTOCOL) {
      gchar *prot;

      prot = gst_uri_get_protocol (KMS_URI_ENDPOINT (self)->uri);
      if (prot == NULL)
        goto invalid_uri;

      GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
          ("No URI handler implemented for \"%s\".", prot), GST_ERROR_SYSTEM);

      g_free (prot);
    } else {
      GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("%s",
              (err) ? err->message : "URI was not accepted by any element"),
          GST_ERROR_SYSTEM);
    }

    g_clear_error (&err);
    goto end;
  }
end:
  KMS_ELEMENT_UNLOCK (KMS_ELEMENT (self));
  return sink;
}

static GstPadProbeReturn
stop_notification_cb (GstPad * srcpad, GstPadProbeInfo * info,
    gpointer user_data)
{
  KmsRecorderEndpoint *recorder = KMS_RECORDER_ENDPOINT (user_data);

  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_DATA (info)) != GST_EVENT_EOS)
    return GST_PAD_PROBE_OK;

  kms_loop_idle_add_full (recorder->priv->loop, G_PRIORITY_HIGH_IDLE,
      set_to_null_state_on_EOS, g_object_ref (recorder), g_object_unref);

  return GST_PAD_PROBE_OK;
}

static void
kms_recorder_endpoint_send_force_key_unit_event (GstElement * valve)
{
  GstStructure *s;
  GstEvent *force_key_unit_event;

  GST_DEBUG ("Sending key ");
  s = gst_structure_new ("GstForceKeyUnit",
      "all-headers", G_TYPE_BOOLEAN, TRUE, NULL);
  force_key_unit_event = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM, s);
  gst_element_send_event (valve, force_key_unit_event);
}

static void
kms_recorder_endpoint_open_valves (KmsRecorderEndpoint * self)
{
  GstElement *valve;

  valve = kms_element_get_audio_valve (KMS_ELEMENT (self));
  if (valve != NULL) {
    kms_utils_set_valve_drop (valve, FALSE);
    kms_recorder_endpoint_send_force_key_unit_event (valve);
  }

  valve = kms_element_get_video_valve (KMS_ELEMENT (self));
  if (valve != NULL) {
    kms_utils_set_valve_drop (valve, FALSE);
    kms_recorder_endpoint_send_force_key_unit_event (valve);
  }
}

static void
kms_recorder_endpoint_close_valves (KmsRecorderEndpoint * self)
{
  GstElement *valve;

  valve = kms_element_get_audio_valve (KMS_ELEMENT (self));
  if (valve != NULL)
    kms_utils_set_valve_drop (valve, TRUE);

  valve = kms_element_get_video_valve (KMS_ELEMENT (self));
  if (valve != NULL)
    kms_utils_set_valve_drop (valve, TRUE);
}

static void
kms_recorder_endpoint_stopped (KmsUriEndpoint * obj)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (obj);
  GstElement *audio_src, *video_src;

  kms_recorder_endpoint_change_state (self);

  /* Close valves */
  kms_recorder_endpoint_close_valves (self);

  // Reset base time data
  audio_src =
      gst_bin_get_by_name (GST_BIN (self->priv->pipeline), AUDIO_APPSRC);
  video_src =
      gst_bin_get_by_name (GST_BIN (self->priv->pipeline), VIDEO_APPSRC);

  KMS_ELEMENT_LOCK (self);

  if (audio_src != NULL) {
    g_object_set_data_full (G_OBJECT (audio_src), BASE_TIME_DATA, NULL, NULL);
    g_object_unref (audio_src);
  }

  if (video_src != NULL) {
    g_object_set_data_full (G_OBJECT (video_src), BASE_TIME_DATA, NULL, NULL);
    g_object_unref (video_src);
  }

  self->priv->paused_time = G_GUINT64_CONSTANT (0);
  self->priv->paused_start = GST_CLOCK_TIME_NONE;

  KMS_ELEMENT_UNLOCK (self);

  if (GST_STATE (self->priv->pipeline) >= GST_STATE_PAUSED) {
    kms_recorder_endpoint_send_eos_to_appsrcs (self);
  } else {
    gst_element_set_state (self->priv->pipeline, GST_STATE_NULL);
    kms_recorder_endpoint_state_changed (self, KMS_URI_ENDPOINT_STATE_STOP);
  }
}

static void
kms_recorder_endpoint_started (KmsUriEndpoint * obj)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (obj);

  kms_recorder_endpoint_change_state (self);

  /* Set internal pipeline to playing */
  gst_element_set_state (self->priv->pipeline, GST_STATE_PLAYING);

  KMS_ELEMENT_LOCK (self);

  if (GST_CLOCK_TIME_IS_VALID (self->priv->paused_start)) {
    self->priv->paused_time +=
        gst_clock_get_time (GST_ELEMENT (self->priv->pipeline)->clock) -
        self->priv->paused_start;
    self->priv->paused_start = GST_CLOCK_TIME_NONE;
  }

  KMS_ELEMENT_UNLOCK (self);

  /* Open valves */
  kms_recorder_endpoint_open_valves (self);

  kms_recorder_endpoint_state_changed (self, KMS_URI_ENDPOINT_STATE_START);
}

static void
kms_recorder_endpoint_paused (KmsUriEndpoint * obj)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (obj);

  kms_recorder_endpoint_change_state (self);

  /* Close valves */
  kms_recorder_endpoint_close_valves (self);

  /* Set internal pipeline to GST_STATE_PAUSED */
  gst_element_set_state (self->priv->pipeline, GST_STATE_PAUSED);

  KMS_ELEMENT_LOCK (self);

  self->priv->paused_start =
      gst_clock_get_time (GST_ELEMENT (self->priv->pipeline)->clock);

  KMS_ELEMENT_UNLOCK (self);

  kms_recorder_endpoint_state_changed (self, KMS_URI_ENDPOINT_STATE_PAUSE);
}

static void
kms_recorder_endpoint_valve_added (KmsRecorderEndpoint * self,
    GstElement * valve, const gchar * sinkname,
    const gchar * srcname, const gchar * destpadname)
{
  KmsUriEndpointState state;

  kms_conf_controller_link_valve (self->priv->controller, valve, sinkname,
      srcname, destpadname);

  g_object_get (self, "state", &state, NULL);
  if (state == KMS_URI_ENDPOINT_STATE_START) {
    GST_DEBUG ("Setting %" GST_PTR_FORMAT " drop to FALSE", valve);
    kms_utils_set_valve_drop (valve, FALSE);
  }
}

static void
kms_recorder_endpoint_audio_valve_added (KmsElement * self, GstElement * valve)
{
  // TODO: This caps should be set using the profile data
  kms_recorder_endpoint_valve_added (KMS_RECORDER_ENDPOINT (self), valve,
      AUDIO_APPSINK, AUDIO_APPSRC, "audio_%u");
}

static void
kms_recorder_endpoint_audio_valve_removed (KmsElement * self,
    GstElement * valve)
{
  GST_INFO ("TODO: Implement this");
}

static void
kms_recorder_endpoint_video_valve_added (KmsElement * self, GstElement * valve)
{
  KmsRecorderEndpoint *recorder = KMS_RECORDER_ENDPOINT (self);

  // TODO: This caps should be set using the profile data
  kms_recorder_endpoint_valve_added (recorder, valve, VIDEO_APPSINK,
      VIDEO_APPSRC, "video_%u");
}

static void
kms_recorder_endpoint_video_valve_removed (KmsElement * self,
    GstElement * valve)
{
  GST_INFO ("TODO: Implement this");
}

static void
kms_recorder_endpoint_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (object);

  KMS_ELEMENT_LOCK (KMS_ELEMENT (self));
  switch (property_id) {
    case PROP_DVR:
      self->priv->use_dvr = g_value_get_boolean (value);
      g_object_set (G_OBJECT (self->priv->controller), "live-DVR",
          self->priv->use_dvr, NULL);
      break;
    case PROP_PROFILE:{
      KmsRecordingProfile profile;

      profile = g_value_get_enum (value);
      g_object_set (G_OBJECT (self->priv->controller), "profile", profile,
          NULL);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  KMS_ELEMENT_UNLOCK (KMS_ELEMENT (self));
}

static void
kms_recorder_endpoint_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (object);

  KMS_ELEMENT_LOCK (KMS_ELEMENT (self));
  switch (property_id) {
    case PROP_DVR:
      g_value_set_boolean (value, self->priv->use_dvr);
      break;
    case PROP_PROFILE:{
      KmsRecordingProfile profile;

      g_object_get (G_OBJECT (self->priv->controller), "profile", &profile,
          NULL);
      g_value_set_enum (value, profile);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  KMS_ELEMENT_UNLOCK (KMS_ELEMENT (self));
}

static void
kms_recorder_endpoint_class_init (KmsRecorderEndpointClass * klass)
{
  KmsUriEndpointClass *urienpoint_class = KMS_URI_ENDPOINT_CLASS (klass);
  KmsElementClass *kms_element_class;
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "RecorderEndpoint", "Sink/Generic", "Kurento plugin recorder end point",
      "Santiago Carot-Nemesio <sancane.kurento@gmail.com>");

  gobject_class->dispose = kms_recorder_endpoint_dispose;
  gobject_class->finalize = kms_recorder_endpoint_finalize;

  urienpoint_class->stopped = kms_recorder_endpoint_stopped;
  urienpoint_class->started = kms_recorder_endpoint_started;
  urienpoint_class->paused = kms_recorder_endpoint_paused;

  kms_element_class = KMS_ELEMENT_CLASS (klass);

  kms_element_class->audio_valve_added =
      GST_DEBUG_FUNCPTR (kms_recorder_endpoint_audio_valve_added);
  kms_element_class->video_valve_added =
      GST_DEBUG_FUNCPTR (kms_recorder_endpoint_video_valve_added);
  kms_element_class->audio_valve_removed =
      GST_DEBUG_FUNCPTR (kms_recorder_endpoint_audio_valve_removed);
  kms_element_class->video_valve_removed =
      GST_DEBUG_FUNCPTR (kms_recorder_endpoint_video_valve_removed);

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (kms_recorder_endpoint_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (kms_recorder_endpoint_get_property);

  obj_properties[PROP_DVR] = g_param_spec_boolean ("live-DVR",
      "Live digital video recorder", "Enables or disbles DVR", FALSE,
      G_PARAM_READWRITE);

  obj_properties[PROP_PROFILE] = g_param_spec_enum ("profile",
      "Recording profile",
      "The profile used for encapsulating the media",
      KMS_TYPE_RECORDING_PROFILE, DEFAULT_RECORDING_PROFILE, G_PARAM_READWRITE);

  g_object_class_install_properties (gobject_class,
      N_PROPERTIES, obj_properties);

  /* Registers a private structure for the instantiatable type */
  g_type_class_add_private (klass, sizeof (KmsRecorderEndpointPrivate));
}

static gboolean
kms_recorder_endpoint_post_error (gpointer data)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (data);

  gchar *message = (gchar *) g_object_steal_data (G_OBJECT (self), "message");

  GST_ELEMENT_ERROR (self, STREAM, FAILED, ("%s", message), (NULL));
  g_free (message);

  return G_SOURCE_REMOVE;
}

static GstBusSyncReply
bus_sync_signal_handler (GstBus * bus, GstMessage * msg, gpointer data)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (data);

  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *err = NULL;

    gst_message_parse_error (msg, &err, NULL);
    GST_ERROR_OBJECT (self, "Message %" GST_PTR_FORMAT, msg);
    g_object_set_data_full (G_OBJECT (self), "message",
        g_strdup (err->message), (GDestroyNotify) g_free);

    kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_HIGH_IDLE,
        kms_recorder_endpoint_post_error, g_object_ref (self), g_object_unref);

    g_error_free (err);
  }
  return GST_BUS_PASS;
}

static void
matched_elements_cb (KmsConfController * controller, GstElement * appsink,
    GstElement * appsrc, gpointer recorder)
{
  g_signal_connect (appsink, "new-sample", G_CALLBACK (recv_sample), appsrc);
  g_signal_connect (appsink, "eos", G_CALLBACK (recv_eos), appsrc);
}

static void
sink_required_cb (KmsConfController * controller, gpointer recorder)
{
  KmsRecorderEndpoint *self = KMS_RECORDER_ENDPOINT (recorder);
  gulong *probe_id;
  GstElement *sink;
  GstPad *sinkpad;

  sink = kms_recorder_endpoint_get_sink (self);

  if (sink == NULL) {
    sink = gst_element_factory_make ("fakesink", NULL);
    GST_ELEMENT_ERROR (self, STREAM, WRONG_TYPE, ("No available sink"), (NULL));
    return;
  }

  g_object_set (self->priv->controller, "sink", sink, NULL);

  sinkpad = gst_element_get_static_pad (sink, "sink");
  if (sinkpad == NULL) {
    GST_WARNING ("No sink pad available for element %" GST_PTR_FORMAT, sink);
    return;
  }

  probe_id = g_slice_new0 (gulong);
  *probe_id = gst_pad_add_probe (sinkpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      stop_notification_cb, self, NULL);
  g_object_set_data_full (G_OBJECT (sinkpad), KEY_RECORDER_PAD_PROBE_ID,
      probe_id, destroy_ulong);
  g_object_unref (sinkpad);
}

static void
sink_unrequired_cb (KmsConfController * controller, GstElement * sink,
    gpointer recorder)
{
  gulong *probe_id;
  GstPad *sinkpad;

  sinkpad = gst_element_get_static_pad (sink, "sink");
  if (sinkpad == NULL) {
    GST_WARNING ("No recording settings were found in element %" GST_PTR_FORMAT,
        sink);
    return;
  }

  probe_id = g_object_get_data (G_OBJECT (sinkpad), KEY_RECORDER_PAD_PROBE_ID);
  if (probe_id != NULL) {
    gst_pad_remove_probe (sinkpad, *probe_id);
    g_object_set_data_full (G_OBJECT (sinkpad), KEY_RECORDER_PAD_PROBE_ID, NULL,
        NULL);
  }

  g_object_unref (sinkpad);
}

static void
kms_recorder_endpoint_init (KmsRecorderEndpoint * self)
{
  GstBus *bus;

  self->priv = KMS_RECORDER_ENDPOINT_GET_PRIVATE (self);

  self->priv->loop = kms_loop_new ();

  self->priv->paused_time = G_GUINT64_CONSTANT (0);
  self->priv->paused_start = GST_CLOCK_TIME_NONE;

  /* Create internal pipeline */
  self->priv->pipeline = gst_pipeline_new ("recorder-pipeline");
  g_object_set (self->priv->pipeline, "async-handling", TRUE, NULL);
  g_cond_init (&self->priv->state_manager.cond);

  bus = gst_pipeline_get_bus (GST_PIPELINE (self->priv->pipeline));
  gst_bus_set_sync_handler (bus, bus_sync_signal_handler, self, NULL);
  g_object_unref (bus);

  self->priv->controller =
      kms_conf_controller_new (KMS_CONF_CONTROLLER_KMS_ELEMENT, self,
      KMS_CONF_CONTROLLER_PIPELINE, self->priv->pipeline, NULL);
  g_signal_connect (self->priv->controller, "matched-elements",
      G_CALLBACK (matched_elements_cb), self);
  g_signal_connect (self->priv->controller, "sink-required",
      G_CALLBACK (sink_required_cb), self);
  g_signal_connect (self->priv->controller, "sink-unrequired",
      G_CALLBACK (sink_unrequired_cb), self);
}

gboolean
kms_recorder_endpoint_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_RECORDER_ENDPOINT);
}
