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
#include "config.h"
#endif

#include <gst/check/gstcheck.h>
#include <gst/gst.h>
#include <glib.h>
#include "kmsuriendpointstate.h"

#include <kmstestutils.h>

#define COSTUME_PATH "costume-images-path"
#define FILTER_VERSION "filter-version"
#define VIDEO_PATH BINARY_LOCATION "/video/pointerDetector.mp4"

GMainLoop *loop;

GST_START_TEST (set_properties)
{
  GstElement *jackvader;

  jackvader = gst_element_factory_make ("jackvader", NULL);

  g_object_set (G_OBJECT (jackvader), COSTUME_PATH, ".", NULL);
  g_object_set (G_OBJECT (jackvader), COSTUME_PATH, ".", NULL);

  g_object_set (G_OBJECT (jackvader), FILTER_VERSION, FALSE, NULL);
  g_object_set (G_OBJECT (jackvader), FILTER_VERSION, TRUE, NULL);

  g_object_unref (jackvader);
}

GST_END_TEST static void
bus_msg (GstBus * bus, GstMessage * msg, gpointer pipe)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:{
      GST_ERROR ("Error: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, "error");
      fail ("Error received on bus");
      break;
    }
    case GST_MESSAGE_EOS:{
      g_main_loop_quit (loop);
      break;
    }
    case GST_MESSAGE_WARNING:{
      GST_ERROR ("Warning: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, "error");
      fail ("Warning received on bus");
      break;
    }
    default:
      break;
  }
}

GST_START_TEST (player_with_filter_v1)
{
  GstElement *player, *pipeline, *filter, *fakesink_video;
  guint bus_watch_id;
  GstBus *bus;

  loop = g_main_loop_new (NULL, FALSE);
  pipeline = gst_pipeline_new ("pipeline_live_stream");
  g_object_set (G_OBJECT (pipeline), "async-handling", TRUE, NULL);
  player = gst_element_factory_make ("playerendpoint", NULL);
  filter = gst_element_factory_make ("jackvader", NULL);
  fakesink_video = gst_element_factory_make ("fakesink", NULL);
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));

  bus_watch_id = gst_bus_add_watch (bus, gst_bus_async_signal_func, NULL);
  g_signal_connect (bus, "message", G_CALLBACK (bus_msg), pipeline);
  g_object_unref (bus);

  g_object_set (G_OBJECT (player), "uri", VIDEO_PATH, NULL);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  gst_bin_add_many (GST_BIN (pipeline), filter, fakesink_video, player, NULL);
  gst_element_set_state (filter, GST_STATE_PLAYING);
  gst_element_set_state (fakesink_video, GST_STATE_PLAYING);
  gst_element_set_state (player, GST_STATE_PLAYING);

  kms_element_link_pads (player, "video_src_%u", filter, "sink");
  gst_element_link (filter, fakesink_video);

  /* Set player to start state */
  g_object_set (G_OBJECT (player), "state", KMS_URI_ENDPOINT_STATE_START, NULL);

  g_main_loop_run (loop);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (GST_OBJECT (pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (loop);
}

GST_END_TEST
GST_START_TEST (player_with_filter_v2)
{
  GstElement *player, *pipeline, *filter, *fakesink_video;
  guint bus_watch_id;
  GstBus *bus;

  loop = g_main_loop_new (NULL, FALSE);
  pipeline = gst_pipeline_new ("pipeline_live_stream");
  g_object_set (G_OBJECT (pipeline), "async-handling", TRUE, NULL);
  player = gst_element_factory_make ("playerendpoint", NULL);
  filter = gst_element_factory_make ("jackvader", NULL);
  fakesink_video = gst_element_factory_make ("fakesink", NULL);
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));

  bus_watch_id = gst_bus_add_watch (bus, gst_bus_async_signal_func, NULL);
  g_signal_connect (bus, "message", G_CALLBACK (bus_msg), pipeline);
  g_object_unref (bus);

  g_object_set (G_OBJECT (player), "uri", VIDEO_PATH, NULL);

  g_object_set (G_OBJECT (filter), FILTER_VERSION, FALSE, NULL);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  gst_bin_add_many (GST_BIN (pipeline), filter, fakesink_video, player, NULL);
  gst_element_set_state (filter, GST_STATE_PLAYING);
  gst_element_set_state (fakesink_video, GST_STATE_PLAYING);
  gst_element_set_state (player, GST_STATE_PLAYING);

  kms_element_link_pads (player, "video_src_%u", filter, "sink");
  gst_element_link (filter, fakesink_video);

  /* Set player to start state */
  g_object_set (G_OBJECT (player), "state", KMS_URI_ENDPOINT_STATE_START, NULL);

  g_main_loop_run (loop);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (GST_OBJECT (pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (loop);
}

GST_END_TEST static Suite *
jackvader_suite (void)
{
  Suite *s = suite_create ("jackvader");
  TCase *tc_chain = tcase_create ("element");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, set_properties);
  tcase_add_test (tc_chain, player_with_filter_v1);
  tcase_add_test (tc_chain, player_with_filter_v2);

  return s;
}

GST_CHECK_MAIN (jackvader);
