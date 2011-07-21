/*
 * Virt Viewer: A virtual machine console viewer
 *
 * Copyright (C) 2007-2009 Red Hat,
 * Copyright (C) 2009 Daniel P. Berrange
 * Copyright (C) 2010 Marc-André Lureau
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <glib/gprintf.h>
#include <glib/gi18n.h>

#include <libxml/xpath.h>
#include <libxml/uri.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#include "virt-viewer-app.h"
#include "virt-viewer-auth.h"
#include "virt-viewer-window.h"
#include "virt-viewer-session.h"
#include "virt-viewer-session-vnc.h"
#ifdef HAVE_SPICE_GTK
#include "virt-viewer-session-spice.h"
#endif

gboolean doDebug = FALSE;

/* Signal handlers for about dialog */
void virt_viewer_app_about_close(GtkWidget *dialog, VirtViewerApp *self);
void virt_viewer_app_about_delete(GtkWidget *dialog, void *dummy, VirtViewerApp *self);


/* Internal methods */
static void virt_viewer_app_connected(VirtViewerSession *session,
				      VirtViewerApp *self);
static void virt_viewer_app_initialized(VirtViewerSession *session,
					VirtViewerApp *self);
static void virt_viewer_app_disconnected(VirtViewerSession *session,
					 VirtViewerApp *self);
static void virt_viewer_app_auth_refused(VirtViewerSession *session,
					 const char *msg,
					 VirtViewerApp *self);
static void virt_viewer_app_auth_failed(VirtViewerSession *session,
					const char *msg,
					VirtViewerApp *self);

static void virt_viewer_app_server_cut_text(VirtViewerSession *session,
					    const gchar *text,
					    VirtViewerApp *self);
static void virt_viewer_app_bell(VirtViewerSession *session,
				 VirtViewerApp *self);

static void virt_viewer_app_channel_open(VirtViewerSession *session,
					 VirtViewerSessionChannel *channel,
					 VirtViewerApp *self);
static void virt_viewer_app_update_pretty_address(VirtViewerApp *self);
static void virt_viewer_app_set_fullscreen(VirtViewerApp *self, gboolean fullscreen);


struct _VirtViewerAppPrivate {
	VirtViewerWindow *main_window;
	GtkWidget *main_notebook;
	GtkWidget *container;
	GHashTable *windows;
	gchar *clipboard;

	gboolean direct;
	gboolean verbose;
	gboolean authretry;
	gboolean started;
	gboolean fullscreen;

	VirtViewerSession *session;
	gboolean active;
	gboolean connected;
	guint reconnect_poll; /* source id */
	char *unixsock;
	char *ghost;
	char *gport;
	char *host; /* ssh */
        int port;/* ssh */
	char *user; /* ssh */
	char *transport;
	char *pretty_address;
	gchar *guest_name;
	gboolean grabbed;
};


G_DEFINE_ABSTRACT_TYPE(VirtViewerApp, virt_viewer_app, G_TYPE_OBJECT)
#define GET_PRIVATE(o)							\
        (G_TYPE_INSTANCE_GET_PRIVATE ((o), VIRT_VIEWER_TYPE_APP, VirtViewerAppPrivate))

enum {
	PROP_0,
	PROP_VERBOSE,
	PROP_CONTAINER,
	PROP_SESSION,
	PROP_GUEST_NAME,
	PROP_FULLSCREEN,
};

void
virt_viewer_app_set_debug(gboolean debug)
{
	doDebug = debug;
}

void
virt_viewer_app_simple_message_dialog(VirtViewerApp *self,
				      const char *fmt, ...)
{
	g_return_if_fail(VIRT_VIEWER_IS_APP(self));
	GtkWindow *window = GTK_WINDOW(virt_viewer_window_get_window(self->priv->main_window));
	GtkWidget *dialog;
	char *msg;
	va_list vargs;

	va_start(vargs, fmt);

	msg = g_strdup_vprintf(fmt, vargs);

	va_end(vargs);

	dialog = gtk_message_dialog_new(window,
					GTK_DIALOG_MODAL |
					GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_ERROR,
					GTK_BUTTONS_OK,
					"%s",
					msg);

	gtk_dialog_run(GTK_DIALOG(dialog));

	gtk_widget_destroy(dialog);

	g_free(msg);
}

static VirtViewerWindow*
virt_viewer_app_window_new(VirtViewerApp *self, GtkWidget *container)
{
	return g_object_new(VIRT_VIEWER_TYPE_WINDOW,
			    "app", self,
			    "container", container,
			    NULL);
}

void
virt_viewer_app_quit(VirtViewerApp *self)
{
	g_return_if_fail(self != NULL);
	VirtViewerAppPrivate *priv = self->priv;

	if (priv->session)
		virt_viewer_session_close(VIRT_VIEWER_SESSION(priv->session));
	gtk_main_quit();
}

void
virt_viewer_app_about_close(GtkWidget *dialog,
			    VirtViewerApp *self G_GNUC_UNUSED)
{
	gtk_widget_hide(dialog);
	gtk_widget_destroy(dialog);
}

void
virt_viewer_app_about_delete(GtkWidget *dialog,
			     void *dummy G_GNUC_UNUSED,
			     VirtViewerApp *self G_GNUC_UNUSED)
{
	gtk_widget_hide(dialog);
	gtk_widget_destroy(dialog);
}

#if defined(HAVE_SOCKETPAIR) && defined(HAVE_FORK)

static int
virt_viewer_app_open_tunnel(const char **cmd)
{
	int fd[2];
	pid_t pid;

	if (socketpair(PF_UNIX, SOCK_STREAM, 0, fd) < 0)
		return -1;

	pid = fork();
	if (pid == -1) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}

	if (pid == 0) { /* child */
		close(fd[0]);
		close(0);
		close(1);
		if (dup(fd[1]) < 0)
			_exit(1);
		if (dup(fd[1]) < 0)
			_exit(1);
		close(fd[1]);
		execvp("ssh", (char *const*)cmd);
		_exit(1);
	}
	close(fd[1]);
	return fd[0];
}


static int
virt_viewer_app_open_tunnel_ssh(const char *sshhost,
				int sshport,
				const char *sshuser,
				const char *host,
				const char *port,
				const char *unixsock)
{
	const char *cmd[10];
	char portstr[50];
	int n = 0;

	if (!sshport)
		sshport = 22;

	sprintf(portstr, "%d", sshport);

	cmd[n++] = "ssh";
	cmd[n++] = "-p";
	cmd[n++] = portstr;
	if (sshuser) {
		cmd[n++] = "-l";
		cmd[n++] = sshuser;
	}
	cmd[n++] = sshhost;
	cmd[n++] = "nc";
	if (port) {
		cmd[n++] = host;
		cmd[n++] = port;
	} else {
		cmd[n++] = "-U";
		cmd[n++] = unixsock;
	}
	cmd[n++] = NULL;

	return virt_viewer_app_open_tunnel(cmd);
}

static int
virt_viewer_app_open_unix_sock(const char *unixsock)
{
	struct sockaddr_un addr;
	int fd;

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, unixsock);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;

	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

#endif /* defined(HAVE_SOCKETPAIR) && defined(HAVE_FORK) */

void
virt_viewer_app_trace(VirtViewerApp *self,
		      const char *fmt, ...)
{
	g_return_if_fail(VIRT_VIEWER_IS_APP(self));
	va_list ap;
	VirtViewerAppPrivate *priv = self->priv;

	if (doDebug) {
		va_start(ap, fmt);
		g_logv(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, fmt, ap);
		va_end(ap);
	}

	if (priv->verbose) {
		va_start(ap, fmt);
		g_vprintf(fmt, ap);
		va_end(ap);
	}
}

void
virt_viewer_app_set_status(VirtViewerApp *self,
			   const char *text)
{
	g_return_if_fail(VIRT_VIEWER_IS_APP(self));

	virt_viewer_notebook_show_status(VIRT_VIEWER_NOTEBOOK(self->priv->main_notebook), text);
}

static void update_title(gpointer key G_GNUC_UNUSED,
			 gpointer value,
			 gpointer user_data G_GNUC_UNUSED)
{
	virt_viewer_window_update_title(VIRT_VIEWER_WINDOW(value));
}

static void
virt_viewer_app_update_title(VirtViewerApp *self)
{
	g_hash_table_foreach(self->priv->windows, update_title, NULL);
}

static VirtViewerWindow *
virt_viewer_app_get_nth_window(VirtViewerApp *self, gint nth)
{
	return g_hash_table_lookup(self->priv->windows, &nth);
}

static gboolean
virt_viewer_app_remove_nth_window(VirtViewerApp *self, gint nth)
{
	gboolean removed;

	g_return_val_if_fail(nth != 0, FALSE);
	removed = g_hash_table_remove(self->priv->windows, &nth);
	g_warn_if_fail(removed);

	return removed;
}

static void
virt_viewer_app_set_nth_window(VirtViewerApp *self, gint nth, VirtViewerWindow *win)
{
	gint *key;

	g_return_if_fail(virt_viewer_app_get_nth_window(self, nth) == NULL);
	key = g_malloc(sizeof(gint));
	*key = nth;
	g_hash_table_insert(self->priv->windows, key, win);
}

static void
virt_viewer_app_display_added(VirtViewerSession *session G_GNUC_UNUSED,
			      VirtViewerDisplay *display,
			      VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv = self->priv;
	VirtViewerWindow *window;
	gint nth;

	g_object_get(display, "nth-display", &nth, NULL);
	if (nth == 0) {
		window = priv->main_window;
	} else {
		if (priv->container) {
			g_warning("multi-head not yet supported within container");
			return;
		}

		g_return_if_fail(virt_viewer_app_get_nth_window(self, nth) == NULL);
		window = virt_viewer_app_window_new(self, NULL);
		/* TODO: track fullscreen state */
		gtk_widget_show_all(GTK_WIDGET(virt_viewer_window_get_window(window)));
		virt_viewer_app_set_nth_window(self, nth, window);
	}

	virt_viewer_window_set_display(window, display);
}


static void
virt_viewer_app_display_removed(VirtViewerSession *session G_GNUC_UNUSED,
				VirtViewerDisplay *display,
				VirtViewerApp *self)
{
	VirtViewerWindow *win = NULL;
	gint nth;

	gtk_widget_hide(GTK_WIDGET(display));
	g_object_get(display, "nth-display", &nth, NULL);
	win = virt_viewer_app_get_nth_window(self, nth);
	virt_viewer_window_set_display(win, NULL);

	if (nth != 0)
		virt_viewer_app_remove_nth_window(self, nth);
}

int
virt_viewer_app_create_session(VirtViewerApp *self, const gchar *type)
{
	g_return_val_if_fail(VIRT_VIEWER_IS_APP(self), -1);
	VirtViewerAppPrivate *priv = self->priv;
	g_return_val_if_fail(priv->session == NULL, -1);

	if (g_strcasecmp(type, "vnc") == 0) {
		virt_viewer_app_trace(self, "Guest %s has a %s display\n",
				      priv->guest_name, type);
		priv->session = virt_viewer_session_vnc_new();
	} else
#ifdef HAVE_SPICE_GTK
	if (g_strcasecmp(type, "spice") == 0) {
		virt_viewer_app_trace(self, "Guest %s has a %s display\n",
				      priv->guest_name, type);
		priv->session = virt_viewer_session_spice_new();
	} else
#endif
	{
		virt_viewer_app_trace(self, "Guest %s has unsupported %s display type\n",
				      priv->guest_name, type);
		virt_viewer_app_simple_message_dialog(self, _("Unknown graphic type for the guest %s"),
						      priv->guest_name);
		return -1;
	}

	g_signal_connect(priv->session, "session-initialized",
			 G_CALLBACK(virt_viewer_app_initialized), self);
	g_signal_connect(priv->session, "session-connected",
			 G_CALLBACK(virt_viewer_app_connected), self);
	g_signal_connect(priv->session, "session-disconnected",
			 G_CALLBACK(virt_viewer_app_disconnected), self);
	g_signal_connect(priv->session, "session-channel-open",
			 G_CALLBACK(virt_viewer_app_channel_open), self);
	g_signal_connect(priv->session, "session-auth-refused",
			 G_CALLBACK(virt_viewer_app_auth_refused), self);
	g_signal_connect(priv->session, "session-auth-failed",
			 G_CALLBACK(virt_viewer_app_auth_failed), self);
	g_signal_connect(priv->session, "session-display-added",
			 G_CALLBACK(virt_viewer_app_display_added), self);
	g_signal_connect(priv->session, "session-display-removed",
			 G_CALLBACK(virt_viewer_app_display_removed), self);

	g_signal_connect(priv->session, "session-cut-text",
			 G_CALLBACK(virt_viewer_app_server_cut_text), self);
	g_signal_connect(priv->session, "session-bell",
			 G_CALLBACK(virt_viewer_app_bell), self);

	return 0;
}

#if defined(HAVE_SOCKETPAIR) && defined(HAVE_FORK)
static void
virt_viewer_app_channel_open(VirtViewerSession *session,
			     VirtViewerSessionChannel *channel,
			     VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv;
	int fd = -1;

	g_return_if_fail(self != NULL);

	priv = self->priv;
	if (priv->transport && g_strcasecmp(priv->transport, "ssh") == 0 &&
	    !priv->direct) {
		if ((fd = virt_viewer_app_open_tunnel_ssh(priv->host, priv->port, priv->user,
							  priv->ghost, priv->gport, NULL)) < 0)
			virt_viewer_app_simple_message_dialog(self, _("Connect to ssh failed."));
	} else {
		virt_viewer_app_simple_message_dialog(self, _("Can't connect to channel, SSH only supported."));
	}

	if (fd >= 0)
		virt_viewer_session_channel_open_fd(session, channel, fd);
}
#else
static void
virt_viewer_app_channel_open(VirtViewerSession *session G_GNUC_UNUSED,
			     VirtViewerSessionChannel *channel G_GNUC_UNUSED,
			     VirtViewerApp *self)
{
	virt_viewer_app_simple_message_dialog(self, _("Connect to channel unsupported."));
}
#endif

int
virt_viewer_app_activate(VirtViewerApp *self)
{
	g_return_val_if_fail(VIRT_VIEWER_IS_APP(self), -1);
	VirtViewerAppPrivate *priv = self->priv;
	int fd = -1;
	int ret = -1;

	if (priv->active)
		goto cleanup;

#if defined(HAVE_SOCKETPAIR) && defined(HAVE_FORK)
	if (priv->transport &&
	    g_strcasecmp(priv->transport, "ssh") == 0 &&
	    !priv->direct) {
		if (priv->gport) {
			virt_viewer_app_trace(self, "Opening indirect TCP connection to display at %s:%s\n",
					      priv->ghost, priv->gport);
		} else {
			virt_viewer_app_trace(self, "Opening indirect UNIX connection to display at %s\n",
					      priv->unixsock);
		}
		virt_viewer_app_trace(self, "Setting up SSH tunnel via %s@%s:%d\n",
				      priv->user, priv->host, priv->port ? priv->port : 22);

		if ((fd = virt_viewer_app_open_tunnel_ssh(priv->host, priv->port,
							  priv->user, priv->ghost,
							  priv->gport, priv->unixsock)) < 0)
			return -1;
	} else if (priv->unixsock) {
		virt_viewer_app_trace(self, "Opening direct UNIX connection to display at %s",
				      priv->unixsock);
		if ((fd = virt_viewer_app_open_unix_sock(priv->unixsock)) < 0)
			return -1;
	}
#endif

	if (fd >= 0) {
		ret = virt_viewer_session_open_fd(VIRT_VIEWER_SESSION(priv->session), fd);
	} else {
		virt_viewer_app_trace(self, "Opening direct TCP connection to display at %s:%s\n",
				      priv->ghost, priv->gport);
		ret = virt_viewer_session_open_host(VIRT_VIEWER_SESSION(priv->session),
						    priv->ghost, priv->gport);
	}

	virt_viewer_app_set_status(self, _("Connecting to graphic server"));

	priv->connected = FALSE;
	priv->active = TRUE;
	priv->grabbed = FALSE;
	virt_viewer_app_update_title(self);

 cleanup:
	return ret;
}

/* text was actually requested */
static void
virt_viewer_app_clipboard_copy(GtkClipboard *clipboard G_GNUC_UNUSED,
			       GtkSelectionData *data,
			       guint info G_GNUC_UNUSED,
			       VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv = self->priv;

	gtk_selection_data_set_text(data, priv->clipboard, -1);
}

static void
virt_viewer_app_server_cut_text(VirtViewerSession *session G_GNUC_UNUSED,
				const gchar *text,
				VirtViewerApp *self)
{
	GtkClipboard *cb;
	gsize a, b;
	VirtViewerAppPrivate *priv = self->priv;
	GtkTargetEntry targets[] = {
		{g_strdup("UTF8_STRING"), 0, 0},
		{g_strdup("COMPOUND_TEXT"), 0, 0},
		{g_strdup("TEXT"), 0, 0},
		{g_strdup("STRING"), 0, 0},
	};

	if (!text)
		return;

	g_free (priv->clipboard);
	priv->clipboard = g_convert (text, -1, "utf-8", "iso8859-1", &a, &b, NULL);

	if (priv->clipboard) {
		cb = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);

		gtk_clipboard_set_with_owner (cb,
					      targets,
					      G_N_ELEMENTS(targets),
					      (GtkClipboardGetFunc)virt_viewer_app_clipboard_copy,
					      NULL,
					      G_OBJECT (self));
	}
}


static void virt_viewer_app_bell(VirtViewerSession *session G_GNUC_UNUSED,
				 VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv = self->priv;

	gdk_window_beep(gtk_widget_get_window(GTK_WIDGET(virt_viewer_window_get_window(priv->main_window))));
}


static int
virt_viewer_app_default_initial_connect(VirtViewerApp *self)
{
	return virt_viewer_app_activate(self);
}

int
virt_viewer_app_initial_connect(VirtViewerApp *self)
{
	VirtViewerAppClass *klass;

	g_return_val_if_fail(VIRT_VIEWER_IS_APP(self), -1);
	klass = VIRT_VIEWER_APP_GET_CLASS(self);

	return klass->initial_connect(self);
}

static gboolean
virt_viewer_app_retryauth(gpointer opaque)
{
	VirtViewerApp *self = opaque;

	virt_viewer_app_initial_connect(self);

	return FALSE;
}

static gboolean
virt_viewer_app_connect_timer(void *opaque)
{
	VirtViewerApp *self = opaque;
	VirtViewerAppPrivate *priv = self->priv;

	DEBUG_LOG("Connect timer fired");

	if (!priv->active &&
	    virt_viewer_app_initial_connect(self) < 0)
		gtk_main_quit();

	if (priv->active) {
		priv->reconnect_poll = 0;
		return FALSE;
	}

	return TRUE;
}

void
virt_viewer_app_start_reconnect_poll(VirtViewerApp *self)
{
	g_return_if_fail(VIRT_VIEWER_IS_APP(self));
	VirtViewerAppPrivate *priv = self->priv;

	if (priv->reconnect_poll != 0)
		return;

	priv->reconnect_poll = g_timeout_add(500, virt_viewer_app_connect_timer, self);
}

static void
virt_viewer_app_default_deactivated(VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv = self->priv;

	virt_viewer_app_set_status(self, _("Guest domain has shutdown"));
	virt_viewer_app_trace(self, "Guest %s display has disconnected, shutting down",
			      priv->guest_name);
	gtk_main_quit();
}

static void
virt_viewer_app_deactivated(VirtViewerApp *self)
{
	VirtViewerAppClass *klass;
	klass = VIRT_VIEWER_APP_GET_CLASS(self);

	klass->deactivated(self);
}

static void
virt_viewer_app_deactivate(VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv = self->priv;

	if (!priv->active)
		return;

	if (priv->session)
		virt_viewer_session_close(VIRT_VIEWER_SESSION(priv->session));

	priv->connected = FALSE;
	priv->active = FALSE;
#if 0
	g_free(priv->pretty_address);
	priv->pretty_address = NULL;
#endif
	priv->grabbed = FALSE;
	virt_viewer_app_update_title(self);

	if (priv->authretry) {
		priv->authretry = FALSE;
		g_idle_add(virt_viewer_app_retryauth, self);
	} else
		virt_viewer_app_deactivated(self);

}

static void
virt_viewer_app_connected(VirtViewerSession *session G_GNUC_UNUSED,
			  VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv = self->priv;

	priv->connected = TRUE;
	virt_viewer_app_set_status(self, _("Connected to graphic server"));
}



static void
virt_viewer_app_initialized(VirtViewerSession *session G_GNUC_UNUSED,
			    VirtViewerApp *self)
{
	virt_viewer_notebook_show_display(VIRT_VIEWER_NOTEBOOK(self->priv->main_notebook));
	virt_viewer_app_update_title(self);
}

static void
virt_viewer_app_disconnected(VirtViewerSession *session G_GNUC_UNUSED,
			     VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv = self->priv;

	if (!priv->connected) {
		virt_viewer_app_simple_message_dialog(self,
						      _("Unable to connect to the graphic server %s"),
						      priv->pretty_address);
	}
	virt_viewer_app_deactivate(self);
}


static void virt_viewer_app_auth_refused(VirtViewerSession *session G_GNUC_UNUSED,
					 const char *msg,
					 VirtViewerApp *self)
{
	GtkWidget *dialog;
	int ret;
	VirtViewerAppPrivate *priv = self->priv;

	dialog = gtk_message_dialog_new(virt_viewer_window_get_window(priv->main_window),
					GTK_DIALOG_MODAL |
					GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_ERROR,
					GTK_BUTTONS_YES_NO,
					_("Unable to authenticate with remote desktop server at %s: %s\n"
					  "Retry connection again?"),
					priv->pretty_address, msg);

	ret = gtk_dialog_run(GTK_DIALOG(dialog));

	gtk_widget_destroy(dialog);

	if (ret == GTK_RESPONSE_YES)
		priv->authretry = TRUE;
	else
		priv->authretry = FALSE;
}


static void virt_viewer_app_auth_failed(VirtViewerSession *session G_GNUC_UNUSED,
					const char *msg,
					VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv = self->priv;

	virt_viewer_app_simple_message_dialog(self,
					      _("Unable to authenticate with remote desktop server at %s"),
					      priv->pretty_address, msg);
}

static void
virt_viewer_app_get_property (GObject *object, guint property_id,
			      GValue *value G_GNUC_UNUSED, GParamSpec *pspec)
{
	g_return_if_fail(VIRT_VIEWER_IS_APP(object));
	VirtViewerApp *self = VIRT_VIEWER_APP(object);
	VirtViewerAppPrivate *priv = self->priv;

        switch (property_id) {
	case PROP_VERBOSE:
		g_value_set_boolean(value, priv->verbose);
		break;

	case PROP_CONTAINER:
		g_value_set_object(value, priv->container);
		break;

	case PROP_SESSION:
		g_value_set_object(value, priv->session);
		break;

	case PROP_GUEST_NAME:
		g_value_set_string(value, priv->guest_name);
		break;

	case PROP_FULLSCREEN:
		g_value_set_boolean(value, priv->fullscreen);
		break;

        default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        }
}

static void
virt_viewer_app_set_property (GObject *object, guint property_id,
			      const GValue *value G_GNUC_UNUSED, GParamSpec *pspec)
{
	g_return_if_fail(VIRT_VIEWER_IS_APP(object));
	VirtViewerApp *self = VIRT_VIEWER_APP(object);
	VirtViewerAppPrivate *priv = self->priv;

        switch (property_id) {
	case PROP_VERBOSE:
		priv->verbose = g_value_get_boolean(value);
		break;

	case PROP_CONTAINER:
		g_return_if_fail(priv->container == NULL);
		priv->container = g_value_dup_object(value);
		break;

	case PROP_GUEST_NAME:
		g_free(priv->guest_name);
		priv->guest_name = g_value_dup_string(value);
		break;

	case PROP_FULLSCREEN:
		virt_viewer_app_set_fullscreen(self, g_value_get_boolean(value));
		break;

        default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        }
}

static void
virt_viewer_app_dispose (GObject *object)
{
	VirtViewerApp *self = VIRT_VIEWER_APP(object);
	VirtViewerAppPrivate *priv = self->priv;

	if (priv->windows) {
		g_hash_table_unref(priv->windows);
		priv->windows = NULL;
	}

	if (priv->main_window) {
		g_object_unref(priv->main_window);
		priv->main_window = NULL;
	}

	if (priv->container) {
		g_object_unref(priv->container);
		priv->container = NULL;
	}

	virt_viewer_app_free_connect_info(self);

        G_OBJECT_CLASS (virt_viewer_app_parent_class)->dispose (object);
}

static gboolean
virt_viewer_app_default_start(VirtViewerApp *self, gboolean fullscreen)
{
	VirtViewerAppPrivate *priv;
	GtkWindow *win;
	priv = self->priv;

	win = virt_viewer_window_get_window(priv->main_window);
	if (win) {
		if (fullscreen)
			gtk_window_fullscreen(win);
		gtk_widget_show_all(GTK_WIDGET(win));
	} else {
		gtk_box_pack_end(GTK_BOX(priv->container), priv->main_notebook, TRUE, TRUE, 0);
		gtk_widget_show_all(GTK_WIDGET(priv->main_notebook));
	}

	return TRUE;
}

gboolean virt_viewer_app_start(VirtViewerApp *self, gboolean fullscreen)
{
	VirtViewerAppClass *klass;

	g_return_val_if_fail(VIRT_VIEWER_IS_APP(self), FALSE);
	klass = VIRT_VIEWER_APP_GET_CLASS(self);

	g_return_val_if_fail(!self->priv->started, TRUE);

	self->priv->started = klass->start(self, fullscreen);
	return self->priv->started;
}

static void
virt_viewer_app_init (VirtViewerApp *self)
{
	self->priv = GET_PRIVATE(self);
	self->priv->windows = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, g_object_unref);
}

static GObject *
virt_viewer_app_constructor (GType gtype,
			     guint n_properties,
			     GObjectConstructParam *properties)
{
	GObject *obj;
	VirtViewerApp *self;
	VirtViewerAppPrivate *priv;

	obj = G_OBJECT_CLASS (virt_viewer_app_parent_class)->constructor (gtype, n_properties, properties);
	self = VIRT_VIEWER_APP(obj);
	priv = self->priv;

	priv->main_window = virt_viewer_app_window_new(self, priv->container);
	priv->main_notebook = GTK_WIDGET(virt_viewer_window_get_notebook(priv->main_window));
	virt_viewer_app_set_nth_window(self, 0, priv->main_window);

	return obj;
}

static void
virt_viewer_app_class_init (VirtViewerAppClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        g_type_class_add_private (klass, sizeof (VirtViewerAppPrivate));

        object_class->constructor = virt_viewer_app_constructor;
        object_class->get_property = virt_viewer_app_get_property;
        object_class->set_property = virt_viewer_app_set_property;
        object_class->dispose = virt_viewer_app_dispose;

	klass->start = virt_viewer_app_default_start;
	klass->initial_connect = virt_viewer_app_default_initial_connect;
	klass->deactivated = virt_viewer_app_default_deactivated;

	g_object_class_install_property(object_class,
					PROP_VERBOSE,
					g_param_spec_boolean("verbose",
							     "Verbose",
							     "Verbose trace",
							     FALSE,
							     G_PARAM_READABLE |
							     G_PARAM_WRITABLE |
							     G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(object_class,
					PROP_CONTAINER,
					g_param_spec_object("container",
							    "Container",
							    "Widget container",
							    GTK_TYPE_WIDGET,
							    G_PARAM_READABLE |
							    G_PARAM_WRITABLE |
							    G_PARAM_CONSTRUCT_ONLY |
							    G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(object_class,
					PROP_SESSION,
					g_param_spec_object("session",
							    "Session",
							    "ViewerSession",
							    VIRT_VIEWER_TYPE_SESSION,
							    G_PARAM_READABLE |
							    G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(object_class,
					PROP_GUEST_NAME,
					g_param_spec_string("guest-name",
							    "Guest name",
							    "Guest name",
							    "",
							    G_PARAM_READABLE |
							    G_PARAM_WRITABLE |
							    G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(object_class,
					PROP_FULLSCREEN,
					g_param_spec_boolean("fullscreen",
							     "Fullscreen",
							     "Fullscreen",
							     FALSE,
							     G_PARAM_READABLE |
							     G_PARAM_WRITABLE |
							     G_PARAM_STATIC_STRINGS));
}

void
virt_viewer_app_set_direct(VirtViewerApp *self, gboolean direct)
{
	g_return_if_fail(VIRT_VIEWER_IS_APP(self));

	self->priv->direct = direct;
}

gboolean
virt_viewer_app_is_active(VirtViewerApp *self)
{
	g_return_val_if_fail(VIRT_VIEWER_IS_APP(self), FALSE);

	return self->priv->active;
}

gboolean
virt_viewer_app_has_session(VirtViewerApp *self)
{
	g_return_val_if_fail(VIRT_VIEWER_IS_APP(self), FALSE);

	return self->priv->session != NULL;
}

static void
virt_viewer_app_update_pretty_address(VirtViewerApp *self)
{
	VirtViewerAppPrivate *priv;

	priv = self->priv;
	g_free(priv->pretty_address);
	if (priv->gport)
		priv->pretty_address = g_strdup_printf("%s:%s", priv->ghost, priv->gport);
	else
		priv->pretty_address = g_strdup_printf("%s:%s", priv->host, priv->unixsock);
}

static void
virt_viewer_app_set_fullscreen(VirtViewerApp *self, gboolean fullscreen)
{
	VirtViewerAppPrivate *priv = self->priv;

	priv->fullscreen = fullscreen;
}

void
virt_viewer_app_set_connect_info(VirtViewerApp *self,
				 const gchar *host,
				 const gchar *ghost,
				 const gchar *gport,
				 const gchar *transport,
				 const gchar *unixsock,
				 const gchar *user,
				 gint port)
{
	g_return_if_fail(VIRT_VIEWER_IS_APP(self));
	VirtViewerAppPrivate *priv = self->priv;

	DEBUG_LOG("Set connect info: %s,%s,%s,%s,%s,%s,%d",
		  host, ghost, gport, transport, unixsock, user, port);

	g_free(priv->host);
	g_free(priv->ghost);
	g_free(priv->gport);
	g_free(priv->transport);
	g_free(priv->unixsock);
	g_free(priv->user);

	priv->host = g_strdup(host);
	priv->ghost = g_strdup(ghost);
	priv->gport = g_strdup(gport);
	priv->transport = g_strdup(transport);
	priv->unixsock = g_strdup(unixsock);
	priv->user = g_strdup(user);
	priv->port = 0;

	virt_viewer_app_update_pretty_address(self);
}

void
virt_viewer_app_free_connect_info(VirtViewerApp *self)
{
	virt_viewer_app_set_connect_info(self, NULL, NULL, NULL, NULL, NULL, NULL, 0);
}

VirtViewerWindow*
virt_viewer_app_get_main_window(VirtViewerApp *self)
{
	return self->priv->main_window;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 *  indent-tabs-mode: t
 * End:
 */