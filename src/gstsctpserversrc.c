/*
 * (C) Copyright 2014 Kurento (http://kurento.org/)
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
#  include <config.h>
#endif

#include <gio/gio.h>
#include <string.h>
#include <netinet/sctp.h>

#include "gstsctp.h"
#include "gstsctpserversrc.h"
#include "kmssctpserverrpc.h"

#define SCTP_BACKLOG 1          /* client connection queue */
#define MAX_BUFFER_SIZE (1024 * 16)

#define PLUGIN_NAME "sctpserversrc"

GST_DEBUG_CATEGORY_STATIC (gst_sctp_server_src_debug_category);
#define GST_CAT_DEFAULT gst_sctp_server_src_debug_category

G_DEFINE_TYPE_WITH_CODE (GstSCTPServerSrc, gst_sctp_server_src,
    GST_TYPE_PUSH_SRC,
    GST_DEBUG_CATEGORY_INIT (gst_sctp_server_src_debug_category, PLUGIN_NAME,
        0, "debug category for sctp server source"));

#define GST_SCTP_SERVER_SRC_GET_PRIVATE(obj) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_SCTP_SERVER_SRC, GstSCTPServerSrcPrivate))

struct _GstSCTPServerSrcPrivate
{
  /* socket */
  GSocket *server_socket;
  GSocket *client_socket;
  GCancellable *cancellable;

  /* server information */
  int current_port;             /* currently bound-to port, or 0 *//* ATOMIC */
  int server_port;              /* port property */
  gchar *host;
  guint16 num_ostreams;
  guint16 max_istreams;

  KmsSCTPServerRPC *serverrpc;
};

enum
{
  PROP_0,
  PROP_HOST,
  PROP_PORT,
  PROP_CURRENT_PORT,
  PROP_NUM_OSTREAMS,
  PROP_MAX_INSTREAMS
};

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static void
gst_sctp_server_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSCTPServerSrc *self;

  g_return_if_fail (GST_IS_SCTP_SERVER_SRC (object));
  self = GST_SCTP_SERVER_SRC (object);

  GST_OBJECT_LOCK (self);

  switch (prop_id) {
    case PROP_HOST:
      if (!g_value_get_string (value)) {
        GST_WARNING ("host property cannot be NULL");
        break;
      }
      g_free (self->priv->host);
      self->priv->host = g_strdup (g_value_get_string (value));
      break;
    case PROP_PORT:
      self->priv->server_port = g_value_get_int (value);
      break;
    case PROP_NUM_OSTREAMS:
      self->priv->num_ostreams = g_value_get_int (value);
      break;
    case PROP_MAX_INSTREAMS:
      self->priv->max_istreams = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  GST_OBJECT_UNLOCK (self);
}

static void
gst_sctp_server_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSCTPServerSrc *self;

  g_return_if_fail (GST_IS_SCTP_SERVER_SRC (object));
  self = GST_SCTP_SERVER_SRC (object);

  GST_OBJECT_LOCK (self);

  switch (prop_id) {
    case PROP_HOST:
      g_value_set_string (value, self->priv->host);
      break;
    case PROP_PORT:
      g_value_set_int (value, self->priv->server_port);
      break;
    case PROP_CURRENT_PORT:
      g_value_set_int (value, g_atomic_int_get (&self->priv->current_port));
      break;
    case PROP_NUM_OSTREAMS:
      g_value_set_int (value, self->priv->num_ostreams);
      break;
    case PROP_MAX_INSTREAMS:
      g_value_set_int (value, self->priv->max_istreams);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  GST_OBJECT_UNLOCK (self);
}

static void
gst_sctp_server_src_dispose (GObject * gobject)
{
  GstSCTPServerSrc *self = GST_SCTP_SERVER_SRC (gobject);

  g_clear_object (&self->priv->cancellable);
  g_clear_object (&self->priv->serverrpc);

  G_OBJECT_CLASS (gst_sctp_server_src_parent_class)->dispose (gobject);
}

static void
gst_sctp_server_src_finalize (GObject * gobject)
{
  GstSCTPServerSrc *self = GST_SCTP_SERVER_SRC (gobject);

  g_free (self->priv->host);

  G_OBJECT_CLASS (gst_sctp_server_src_parent_class)->finalize (gobject);
}

static gboolean
gst_sctp_server_src_stop (GstBaseSrc * bsrc)
{
  GstSCTPServerSrc *self = GST_SCTP_SERVER_SRC (bsrc);

  GST_DEBUG ("stopping");

  g_cancellable_cancel (self->priv->cancellable);
  kms_sctp_server_rpc_stop (self->priv->serverrpc);

  return TRUE;
}

/* set up server */
static gboolean
gst_sctp_server_src_start (GstBaseSrc * bsrc)
{
  GstSCTPServerSrc *self = GST_SCTP_SERVER_SRC (bsrc);
  GError *err = NULL;

  GST_DEBUG ("starting");

  if (kms_sctp_server_rpc_start (self->priv->serverrpc, self->priv->host,
          self->priv->server_port, self->priv->cancellable, &err)) {
    return TRUE;
  }

  GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ, (NULL),
      ("Error: %s", err->message));

  g_error_free (err);

  return FALSE;
}

/* will be called only between calls to start() and stop() */
static gboolean
gst_sctp_server_src_unlock (GstBaseSrc * bsrc)
{
  GstSCTPServerSrc *self = GST_SCTP_SERVER_SRC (bsrc);

  GST_DEBUG ("unlock");
  g_cancellable_cancel (self->priv->cancellable);

  return TRUE;
}

static gboolean
gst_sctp_server_src_unlock_stop (GstBaseSrc * bsrc)
{
  GstSCTPServerSrc *self = GST_SCTP_SERVER_SRC (bsrc);

  GST_DEBUG ("unlock_stop");
  g_cancellable_reset (self->priv->cancellable);

  return TRUE;
}

static GstFlowReturn
gst_sctp_server_src_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
  GstSCTPServerSrc *self = GST_SCTP_SERVER_SRC (psrc);
  GstFlowReturn ret = GST_FLOW_OK;
  GError *err = NULL;
  GstMapInfo map;
  gssize rret;

  GST_DEBUG ("create");

  *outbuf = gst_buffer_new_and_alloc (MAX_BUFFER_SIZE);
  gst_buffer_map (*outbuf, &map, GST_MAP_READWRITE);

  rret =
      kms_sctp_server_rpc_get_buffer (self->priv->serverrpc, (gchar *) map.data,
      MAX_BUFFER_SIZE, &err);

  if (rret == 0) {
    GST_DEBUG_OBJECT (self, "Connection closed");
    ret = GST_FLOW_EOS;
    if (*outbuf != NULL) {
      gst_buffer_unmap (*outbuf, &map);
      gst_buffer_unref (*outbuf);
    }
    *outbuf = NULL;
  } else if (rret < 0) {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      ret = GST_FLOW_FLUSHING;
      GST_DEBUG_OBJECT (self, "Cancelled reading from socket");
    } else {
      ret = GST_FLOW_ERROR;
      GST_ELEMENT_ERROR (self, RESOURCE, READ, (NULL),
          ("Failed to read from socket: %s", err->message));
    }
    gst_buffer_unmap (*outbuf, &map);
    gst_buffer_unref (*outbuf);
    *outbuf = NULL;
  } else {
    ret = GST_FLOW_OK;
    gst_buffer_unmap (*outbuf, &map);
    gst_buffer_resize (*outbuf, 0, rret);

    GST_LOG_OBJECT (self, "Got buffer %" GST_PTR_FORMAT, *outbuf);
  }
  g_clear_error (&err);

  return ret;
}

static void
gst_sctp_server_src_class_init (GstSCTPServerSrcClass * klass)
{
  GstPushSrcClass *gstpush_src_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->set_property = gst_sctp_server_src_set_property;
  gobject_class->get_property = gst_sctp_server_src_get_property;
  gobject_class->finalize = gst_sctp_server_src_finalize;
  gobject_class->dispose = gst_sctp_server_src_dispose;

  g_object_class_install_property (gobject_class, PROP_HOST,
      g_param_spec_string ("bind-address", "Bind Address",
          "The address to bind the socket to",
          SCTP_DEFAULT_HOST,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PORT,
      g_param_spec_int ("port", "Port",
          "The port to listen to (0=random available port)",
          0, G_MAXUINT16, SCTP_DEFAULT_PORT,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CURRENT_PORT,
      g_param_spec_int ("current-port", "current-port",
          "The port number the socket is currently bound to", 0,
          G_MAXUINT16, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_NUM_OSTREAMS,
      g_param_spec_int ("num-ostreams", "Output streams",
          "This is the number of streams that the application wishes to be "
          "able to send to", 0, G_MAXUINT16, 1,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MAX_INSTREAMS,
      g_param_spec_int ("max-instreams", "Inputput streams",
          "This value represents the maximum number of inbound streams the "
          "application is prepared to support", 0, G_MAXUINT16, 1,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  gstelement_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_static_metadata (gstelement_class,
      "SCTP server source", "Source/Network",
      "Receive data as a server over the network via SCTP",
      "Santiago Carot-Nemesio <sancane at gmail dot com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&srctemplate));

  gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  gstbasesrc_class->start = gst_sctp_server_src_start;
  gstbasesrc_class->stop = gst_sctp_server_src_stop;
  gstbasesrc_class->unlock = gst_sctp_server_src_unlock;
  gstbasesrc_class->unlock_stop = gst_sctp_server_src_unlock_stop;

  gstpush_src_class = GST_PUSH_SRC_CLASS (klass);
  gstpush_src_class->create = gst_sctp_server_src_create;

  g_type_class_add_private (klass, sizeof (GstSCTPServerSrcPrivate));
}

static void
gst_sctp_server_src_remote_query (GstQuery * query, GstSCTPServerSrc * self)
{
  GST_DEBUG_OBJECT (self, ">> %" GST_PTR_FORMAT, query);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
      gst_pad_peer_query (GST_BASE_SRC_PAD (GST_BASE_SRC (self)), query);
      break;
    default:
      GST_WARNING ("Unsupported query %" GST_PTR_FORMAT, query);
      return;
  }

  GST_DEBUG_OBJECT (self, "<< %" GST_PTR_FORMAT, query);
}

static void
gst_sctp_server_src_init (GstSCTPServerSrc * self)
{
  self->priv = GST_SCTP_SERVER_SRC_GET_PRIVATE (self);
  self->priv->cancellable = g_cancellable_new ();
  self->priv->serverrpc = kms_sctp_server_rpc_new (KMS_SCTP_BASE_RPC_RULES,
      KURENTO_MARSHALL_BER, KMS_SCTP_BASE_RPC_BUFFER_SIZE, MAX_BUFFER_SIZE,
      NULL);
  kms_sctp_base_rpc_set_query_function (KMS_SCTP_BASE_RPC (self->priv->
          serverrpc), (KmsQueryFunction) gst_sctp_server_src_remote_query, self,
      NULL);
}

gboolean
gst_sctp_server_src_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      GST_TYPE_SCTP_SERVER_SRC);
}
