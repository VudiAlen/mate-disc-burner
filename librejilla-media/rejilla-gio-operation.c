/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Rejilla
 * Copyright (C) Philippe Rouquier 2005-2009 <bonfire-app@wanadoo.fr>
 *
 * Librejilla-media is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Librejilla-media authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Librejilla-media. This permission is above and beyond the permissions granted
 * by the GPL license by which Librejilla-media is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Librejilla-media is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "rejilla-drive.h"
#include "rejilla-media-private.h"
#include "rejilla-gio-operation.h"

typedef struct _RejillaGioOperation RejillaGioOperation;
struct _RejillaGioOperation
{
	GMainLoop *loop;
	GCancellable *cancel;
	guint timeout_id;
	gboolean result;
	GError *error;
};

static void
rejilla_gio_operation_destroy (RejillaGioOperation *operation)
{
	if (operation->cancel) {
		g_cancellable_cancel (operation->cancel);
		operation->cancel = NULL;
	}

	if (operation->timeout_id) {
		g_source_remove (operation->timeout_id);
		operation->timeout_id = 0;
	}

	if (operation->loop && g_main_loop_is_running (operation->loop))
		g_main_loop_quit (operation->loop);
}

static void
rejilla_gio_operation_end (gpointer callback_data)
{
	RejillaGioOperation *operation = callback_data;

	if (!operation->loop)
		return;

	if (!g_main_loop_is_running (operation->loop))
		return;

	g_main_loop_quit (operation->loop);	
}

static gboolean
rejilla_gio_operation_timeout (gpointer callback_data)
{
	RejillaGioOperation *operation = callback_data;

	rejilla_gio_operation_end (operation);

	REJILLA_MEDIA_LOG ("Volume/Disc operation timed out");

	operation->timeout_id = 0;
	operation->result = FALSE;
	return FALSE;
}

static void
rejilla_gio_operation_cancelled (GCancellable *cancel,
				 RejillaGioOperation *operation)
{
	operation->result = FALSE;
	g_cancellable_cancel (operation->cancel);
	if (operation->loop && g_main_loop_is_running (operation->loop))
		g_main_loop_quit (operation->loop);
}

static gboolean
rejilla_gio_operation_wait_for_operation_end (RejillaGioOperation *operation,
					      GError **error)
{
	REJILLA_MEDIA_LOG ("Waiting for end of async operation");

	g_object_ref (operation->cancel);
	g_cancellable_reset (operation->cancel);
	g_signal_connect (operation->cancel,
			  "cancelled",
			  G_CALLBACK (rejilla_gio_operation_cancelled),
			  operation);

	/* put a timeout (30 sec) */
	operation->timeout_id = g_timeout_add_seconds (20,
						       rejilla_gio_operation_timeout,
						       operation);

	operation->loop = g_main_loop_new (NULL, FALSE);

	GDK_THREADS_LEAVE ();
	g_main_loop_run (operation->loop);
	GDK_THREADS_ENTER ();

	g_main_loop_unref (operation->loop);
	operation->loop = NULL;

	if (operation->timeout_id) {
		g_source_remove (operation->timeout_id);
		operation->timeout_id = 0;
	}

	if (operation->error) {
		REJILLA_MEDIA_LOG ("Medium operation finished with an error %s",
				   operation->error->message);

		if (operation->error->code == G_IO_ERROR_FAILED_HANDLED) {
			REJILLA_MEDIA_LOG ("Error already handled and displayed by GIO");

			/* means we shouldn't display any error message since 
			 * that was already done */
			g_error_free (operation->error);
			operation->error = NULL;
		}
		else if (error && (*error) == NULL)
			g_propagate_error (error, operation->error);
		else
			g_error_free (operation->error);

		operation->error = NULL;
	}

	g_object_unref (operation->cancel);
	return operation->result;
}

static void
rejilla_gio_operation_umounted_cb (GMount *mount,
				   RejillaGioOperation *operation)
{
	rejilla_gio_operation_end (operation);
}

static void
rejilla_gio_operation_umount_finish (GObject *source,
				     GAsyncResult *result,
				     gpointer user_data)
{
	RejillaGioOperation *op = user_data;

	if (!op->loop)
		return;

	op->result = g_mount_unmount_with_operation_finish (G_MOUNT (source),
					     		    result,
					     		    &op->error);

	REJILLA_MEDIA_LOG ("Umount operation completed (result = %d)", op->result);

	if (op->error) {
		if (op->error->code == G_IO_ERROR_NOT_MOUNTED) {
			/* That can happen sometimes */
			g_error_free (op->error);
			op->error = NULL;
			op->result = TRUE;
		}

		/* Since there was an error. The "unmounted" signal won't be 
		 * emitted by GVolumeMonitor and therefore we'd get stuck if
		 * we didn't get out of the loop. */
		rejilla_gio_operation_end (op);
	}
	else if (!op->result)
		rejilla_gio_operation_end (op);
}

gboolean
rejilla_gio_operation_umount (GVolume *gvolume,
			      GCancellable *cancel,
			      gboolean wait,
			      GError **error)
{
	GMount *mount;
	gboolean result;

	REJILLA_MEDIA_LOG ("Unmounting volume");

	if (!gvolume) {
		REJILLA_MEDIA_LOG ("No volume");
		return TRUE;
	}

	mount = g_volume_get_mount (gvolume);
	if (!mount) {
		REJILLA_MEDIA_LOG ("No mount");
		return TRUE;
	}

	if (!g_mount_can_unmount (mount)) {
		/* NOTE: that can happen when for example a blank medium is 
		 * mounted on burn:// */
		REJILLA_MEDIA_LOG ("Mount can't unmount");
		return FALSE;
	}

	if (wait) {
		gulong umount_sig;
		RejillaGioOperation *op;

		op = g_new0 (RejillaGioOperation, 1);
		op->cancel = cancel;

		umount_sig = g_signal_connect_after (mount,
						     "unmounted",
						     G_CALLBACK (rejilla_gio_operation_umounted_cb),
						     op);

		/* NOTE: we own a reference to mount
		 * object so no need to ref it even more */
		g_mount_unmount_with_operation (mount,
				 		G_MOUNT_UNMOUNT_NONE,
						NULL,
				 		cancel,
				 		rejilla_gio_operation_umount_finish,
				 		op);
		result = rejilla_gio_operation_wait_for_operation_end (op, error);
		rejilla_gio_operation_destroy (op);
		g_signal_handler_disconnect (mount, umount_sig);
	}
	else {
		g_mount_unmount_with_operation (mount,
				 		G_MOUNT_UNMOUNT_NONE,
						NULL,
				 		cancel,
				 		NULL,					/* callback */
				 		NULL);
		result = TRUE;
	}
	g_object_unref (mount);

	REJILLA_MEDIA_LOG ("Unmount result = %d", result);
	return result;
}

static void
rejilla_gio_operation_mount_finish (GObject *source,
				    GAsyncResult *result,
				    gpointer user_data)
{
	RejillaGioOperation *op = user_data;

	op->result = g_volume_mount_finish (G_VOLUME (source),
					    result,
					    &op->error);

	if (op->error) {
		if (op->error->code == G_IO_ERROR_ALREADY_MOUNTED) {
			g_error_free (op->error);
			op->error = NULL;
			op->result = TRUE;
		}
	}

	rejilla_gio_operation_end (op);
}

gboolean
rejilla_gio_operation_mount (GVolume *gvolume,
			     GtkWindow *parent_window,
			     GCancellable *cancel,
			     gboolean wait,
			     GError **error)
{
	GMount *mount;
	gboolean result;
	GMountOperation *operation = NULL;

	REJILLA_MEDIA_LOG ("Mounting");

	if (!gvolume) {
		REJILLA_MEDIA_LOG ("No volume");
		return FALSE;
	}

	if (!g_volume_can_mount (gvolume)) {
		REJILLA_MEDIA_LOG ("Volume can't be mounted");
		return FALSE;
	}

	mount = g_volume_get_mount (gvolume);
	if (mount) {
		REJILLA_MEDIA_LOG ("Existing mount");
		g_object_unref (mount);
		return TRUE;
	}

	if (parent_window && GTK_IS_WINDOW (parent_window))
		operation = gtk_mount_operation_new (parent_window);

	if (wait) {
		RejillaGioOperation *op;

		op = g_new0 (RejillaGioOperation, 1);
		op->cancel = cancel;

		/* Ref gvolume as it could be unreffed 
		 * while we are in the loop */
		g_object_ref (gvolume);

		g_volume_mount (gvolume,
				G_MOUNT_MOUNT_NONE,
				operation,				/* authentification */
				cancel,
				rejilla_gio_operation_mount_finish,
				op);
		result = rejilla_gio_operation_wait_for_operation_end (op, error);

		g_object_unref (gvolume);
	}
	else {
		g_volume_mount (gvolume,
				G_MOUNT_MOUNT_NONE,
				operation,				/* authentification */
				cancel,
				NULL,
				NULL);
		result = TRUE;
	}

	if (operation)
		g_object_unref (operation);

	REJILLA_MEDIA_LOG ("Mount result = %d", result);

	return result;
}

static void
rejilla_gio_operation_eject_finish (GObject *source,
				    GAsyncResult *result,
				    gpointer user_data)
{
	RejillaGioOperation *operation = user_data;

	if (G_IS_DRIVE (source))
		operation->result = g_drive_eject_with_operation_finish (G_DRIVE (source),
							  result,
							  &operation->error);
	else
		operation->result = g_volume_eject_with_operation_finish (G_VOLUME (source),
							   result,
							   &operation->error);

	if (operation->error)
		rejilla_gio_operation_end (operation);
	else if (!operation->result)
		rejilla_gio_operation_end (operation);
}

static void
rejilla_gio_operation_removed_cb (GVolume *volume,
				  gpointer callback_data)
{
	RejillaGioOperation *operation = callback_data;
	rejilla_gio_operation_end (operation);
}

gboolean
rejilla_gio_operation_eject_volume (GVolume *gvolume,
				    GCancellable *cancel,
				    gboolean wait,
				    GError **error)
{
	gboolean result;

	if (!g_volume_can_eject (gvolume)) {
		REJILLA_MEDIA_LOG ("GVolume cannot be ejected");
		return FALSE;
	}

	if (wait) {
		gulong eject_sig;
		RejillaGioOperation *op;

		op = g_new0 (RejillaGioOperation, 1);
		op->cancel = cancel;

		eject_sig = g_signal_connect (gvolume,
					      "removed",
					      G_CALLBACK (rejilla_gio_operation_removed_cb),
					      op);

		/* Ref gvolume as it could be unreffed 
		 * while we are in the loop */
		g_object_ref (gvolume);

		g_volume_eject_with_operation (gvolume,
					       G_MOUNT_UNMOUNT_NONE,
					       NULL,
					       cancel,
				  	       rejilla_gio_operation_eject_finish,
					       op);

		result = rejilla_gio_operation_wait_for_operation_end (op, error);
		g_signal_handler_disconnect (gvolume, eject_sig);

		rejilla_gio_operation_destroy (op);

		g_object_unref (gvolume);
	}
	else {
		g_volume_eject_with_operation (gvolume,
					       G_MOUNT_UNMOUNT_NONE,
				               NULL,
					       cancel,
					       NULL,
					       NULL);
		result = TRUE;
	}

	return result;
}

static void
rejilla_gio_operation_ejected_cb (GDrive *gdrive,
				  gpointer callback_data)
{
	RejillaGioOperation *operation = callback_data;

	if (!g_drive_has_media (gdrive))
		rejilla_gio_operation_end (operation);
}

static void
rejilla_gio_operation_disconnected_cb (GDrive *gdrive,
				       gpointer callback_data)
{
	RejillaGioOperation *operation = callback_data;
	rejilla_gio_operation_end (operation);
}

gboolean
rejilla_gio_operation_eject_drive (GDrive *gdrive,
				   GCancellable *cancel,
				   gboolean wait,
				   GError **error)
{
	gboolean result;

	if (!gdrive) {
		REJILLA_MEDIA_LOG ("No GDrive");
		return FALSE;
	}

	if (!g_drive_can_eject (gdrive)) {
		REJILLA_MEDIA_LOG ("GDrive can't eject");
		return FALSE;
	}

	if (wait) {
		gulong eject_sig;
		gulong disconnect_sig;
		RejillaGioOperation *op;

		op = g_new0 (RejillaGioOperation, 1);
		op->cancel = cancel;

		eject_sig = g_signal_connect (gdrive,
					      "changed",
					      G_CALLBACK (rejilla_gio_operation_ejected_cb),
					      op);

		disconnect_sig = g_signal_connect (gdrive,
						   "disconnected",
						   G_CALLBACK (rejilla_gio_operation_disconnected_cb),
						   op);

		g_drive_eject_with_operation (gdrive,
			       		      G_MOUNT_UNMOUNT_NONE,
			                      NULL,
					      cancel,
			                      rejilla_gio_operation_eject_finish,
			                      op);

		/* Ref gdrive as it could be unreffed 
		 * while we are in the loop */
		g_object_ref (gdrive);

		result = rejilla_gio_operation_wait_for_operation_end (op, error);
		rejilla_gio_operation_destroy (op);
		g_signal_handler_disconnect (gdrive, eject_sig);
		g_signal_handler_disconnect (gdrive, disconnect_sig);

		g_object_unref (gdrive);
	}
	else {
		g_drive_eject_with_operation (gdrive,
			       		      G_MOUNT_UNMOUNT_NONE,
					      NULL,
			         	      cancel,
			       		      NULL,
			       		      NULL);
		result = TRUE;
	}

	return result;
}
