/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* gcr-viewer-window.c: Window for viewer

   Copyright (C) 2011 Collabora Ltd.

   The Gnome Keyring Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Keyring Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Author: Stef Walter <stefw@collabora.co.uk>
*/

#include "config.h"

#include "gcr-failure-renderer.h"
#include "gcr-parser.h"
#include "gcr-renderer.h"
#include "gcr-unlock-renderer.h"
#include "gcr-viewer-window.h"
#include "gcr-viewer.h"

#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#include <locale.h>
#include <string.h>

/**
 * SECTION:gcr-viewer-window
 * @title: GcrViewerWindow
 * @short_description: A window which shows certificates or keys
 *
 * A viewer window which can display certificates and keys that are
 * located in files.
 */

/**
 * GcrViewerWindow:
 *
 * A viewer window object.
 */

/**
 * GcrViewerWindowClass:
 *
 * Class for #GcrViewerWindow
 */

struct _GcrViewerWindowPrivate {
	GQueue *files_to_load;
	GcrParser *parser;
	GCancellable *cancellable;
	GcrViewer *viewer;
	gboolean loading;
	gchar *display_name;
};

static void viewer_load_next_file (GcrViewerWindow *self);
static void viewer_stop_loading_files (GcrViewerWindow *self);

static void gcr_viewer_iface_init (GcrViewerIface *iface);

G_DEFINE_TYPE_WITH_CODE (GcrViewerWindow, gcr_viewer_window, GTK_TYPE_WINDOW,
                         G_IMPLEMENT_INTERFACE (GCR_TYPE_VIEWER, gcr_viewer_iface_init);
);

static const gchar *
get_parsed_label_or_display_name (GcrViewerWindow *self,
                                  GcrParser *parser)
{
	const gchar *label;

	label = gcr_parser_get_parsed_label (parser);
	if (label == NULL)
		label = self->pv->display_name;

	return label;
}

static void
on_parser_parsed (GcrParser *parser, gpointer user_data)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (user_data);
	GcrRenderer *renderer;

	renderer = gcr_renderer_create (get_parsed_label_or_display_name (self, parser),
	                                gcr_parser_get_parsed_attributes (parser));

	if (renderer == NULL)
		renderer = _gcr_failure_renderer_new_unsupported (get_parsed_label_or_display_name (self, parser));

	gcr_viewer_add_renderer (self->pv->viewer, renderer);
	g_object_unref (renderer);
}

static gboolean
on_parser_authenticate (GcrParser *parser,
                        guint count,
                        gpointer user_data)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (user_data);
	GcrUnlockRenderer *renderer;

	renderer = _gcr_unlock_renderer_new_for_parsed (parser);
	if (renderer) {
		g_object_set (renderer, "label", get_parsed_label_or_display_name (self, parser), NULL);
		gcr_viewer_add_renderer (self->pv->viewer, GCR_RENDERER (renderer));
		g_object_unref (renderer);
	}

	return TRUE;
}

static void
gcr_viewer_window_init (GcrViewerWindow *self)
{
	self->pv = G_TYPE_INSTANCE_GET_PRIVATE (self, GCR_TYPE_VIEWER_WINDOW,
	                                        GcrViewerWindowPrivate);

	self->pv->files_to_load = g_queue_new ();
	self->pv->parser = gcr_parser_new ();
	self->pv->cancellable = g_cancellable_new ();

	g_signal_connect (self->pv->parser, "parsed", G_CALLBACK (on_parser_parsed), self);
	g_signal_connect (self->pv->parser, "authenticate", G_CALLBACK (on_parser_authenticate), self);
}

static void
gcr_viewer_window_constructed (GObject *obj)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (obj);

	if (G_OBJECT_CLASS (gcr_viewer_window_parent_class)->constructed)
		G_OBJECT_CLASS (gcr_viewer_window_parent_class)->constructed (obj);

	self->pv->viewer = gcr_viewer_new_scrolled ();

	gtk_widget_show (GTK_WIDGET (self->pv->viewer));
	gtk_container_add (GTK_CONTAINER (self), GTK_WIDGET (self->pv->viewer));

	gtk_window_set_default_size (GTK_WINDOW (self), 250, 400);
}

static void
gcr_viewer_window_dispose (GObject *obj)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (obj);

	g_signal_handlers_disconnect_by_func (self->pv->parser, on_parser_parsed, self);

	while (!g_queue_is_empty (self->pv->files_to_load))
		g_object_unref (g_queue_pop_head (self->pv->files_to_load));

	g_cancellable_cancel (self->pv->cancellable);

	G_OBJECT_CLASS (gcr_viewer_window_parent_class)->dispose (obj);
}

static void
gcr_viewer_window_finalize (GObject *obj)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (obj);

	/* self->pv->viewer is owned by container */

	g_assert (g_queue_is_empty (self->pv->files_to_load));
	g_queue_free (self->pv->files_to_load);

	g_free (self->pv->display_name);
	g_object_unref (self->pv->cancellable);
	g_object_unref (self->pv->parser);

	G_OBJECT_CLASS (gcr_viewer_window_parent_class)->finalize (obj);
}

static void
gcr_viewer_window_class_init (GcrViewerWindowClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gcr_viewer_window_parent_class = g_type_class_peek_parent (klass);

	gobject_class->dispose = gcr_viewer_window_dispose;
	gobject_class->finalize = gcr_viewer_window_finalize;
	gobject_class->constructed = gcr_viewer_window_constructed;

	g_type_class_add_private (klass, sizeof (GcrViewerWindow));
}

static void
gcr_viewer_window_add_renderer (GcrViewer *viewer,
                                GcrRenderer *renderer)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (viewer);
	gcr_viewer_add_renderer (self->pv->viewer, renderer);
}

static void
gcr_viewer_window_insert_renderer (GcrViewer *viewer,
                                   GcrRenderer *renderer,
                                   GcrRenderer *before)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (viewer);
	gcr_viewer_insert_renderer (self->pv->viewer, renderer, before);
}

static void
gcr_viewer_window_remove_renderer (GcrViewer *viewer,
                                   GcrRenderer *renderer)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (viewer);
	gcr_viewer_remove_renderer (self->pv->viewer, renderer);
}

static guint
gcr_viewer_window_count_renderers (GcrViewer *viewer)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (viewer);
	return gcr_viewer_count_renderers (self->pv->viewer);
}

static GcrRenderer *
gcr_viewer_window_get_renderer (GcrViewer *viewer,
                                guint index_)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (viewer);
	return gcr_viewer_get_renderer (self->pv->viewer, index_);
}

static void
gcr_viewer_iface_init (GcrViewerIface *iface)
{
	iface->add_renderer = gcr_viewer_window_add_renderer;
	iface->insert_renderer = gcr_viewer_window_insert_renderer;
	iface->count_renderers = gcr_viewer_window_count_renderers;
	iface->get_renderer = gcr_viewer_window_get_renderer;
	iface->remove_renderer = gcr_viewer_window_remove_renderer;
}

static void
on_parser_parse_stream_returned (GObject *source, GAsyncResult *result,
                                 gpointer user_data)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (user_data);
	GError *error = NULL;
	GcrRenderer *renderer;

	gcr_parser_parse_stream_finish (self->pv->parser, result, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
	    g_error_matches (error, GCR_DATA_ERROR, GCR_ERROR_CANCELLED)) {
		viewer_stop_loading_files (self);

	} else if (g_error_matches (error, GCR_DATA_ERROR, GCR_ERROR_LOCKED)) {
		/* Just skip this one, an unlock renderer was added */

	} else if (error) {
		renderer = _gcr_failure_renderer_new (self->pv->display_name, error);
		gcr_viewer_add_renderer (self->pv->viewer, renderer);
		g_object_unref (renderer);
		g_error_free (error);
	}

	viewer_load_next_file (self);
}

static void
update_display_name (GcrViewerWindow *self,
                     GFile *file)
{
	gchar *basename;

	basename = g_file_get_basename (file);

	g_free (self->pv->display_name);
	self->pv->display_name = g_filename_display_name (basename);

	g_free (basename);
}

static void
on_file_read_returned (GObject *source, GAsyncResult *result, gpointer user_data)
{
	GcrViewerWindow *self = GCR_VIEWER_WINDOW (user_data);
	GFile *file = G_FILE (source);
	GError *error = NULL;
	GFileInputStream *fis;
	GcrRenderer *renderer;

	fis = g_file_read_finish (file, result, &error);
	update_display_name (self, file);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		viewer_stop_loading_files (self);

	} else if (error) {
		renderer = _gcr_failure_renderer_new (self->pv->display_name, error);
		gcr_viewer_add_renderer (self->pv->viewer, renderer);
		g_object_unref (renderer);
		g_error_free (error);

		viewer_load_next_file (self);

	} else {
		gcr_parser_parse_stream_async (self->pv->parser, G_INPUT_STREAM (fis),
		                               self->pv->cancellable, on_parser_parse_stream_returned,
		                               self);
		g_object_unref (fis);
	}
}

static void
viewer_stop_loading_files (GcrViewerWindow *self)
{
	self->pv->loading = FALSE;
}

static void
viewer_load_next_file (GcrViewerWindow *self)
{
	GFile* file;

	file = g_queue_pop_head (self->pv->files_to_load);
	if (file == NULL) {
		viewer_stop_loading_files (self);
		return;
	}

	g_file_read_async (file, G_PRIORITY_DEFAULT, self->pv->cancellable,
	                   on_file_read_returned, self);

	g_object_unref (file);
}

/**
 * gcr_viewer_window_new:
 *
 * Create a new viewer window.
 *
 * Returns: (transfer full): A new #GcrViewerWindow object
 */
GcrViewerWindow *
gcr_viewer_window_new (void)
{
	return g_object_new (GCR_TYPE_VIEWER_WINDOW, NULL);
}

/**
 * gcr_viewer_window_load:
 * @self: a viewer window
 * @file: a file to load
 *
 * Display contents of a file in the viewer window. Multiple files can
 * be loaded.
 */
void
gcr_viewer_window_load (GcrViewerWindow *self, GFile *file)
{
	g_return_if_fail (GCR_IS_VIEWER_WINDOW (self));
	g_return_if_fail (G_IS_FILE (file));

	g_queue_push_tail (self->pv->files_to_load, g_object_ref (file));

	if (!self->pv->loading)
		viewer_load_next_file (self);
}