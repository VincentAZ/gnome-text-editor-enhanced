/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "editor-page-private.h"
#include "editor-document.h"
#include "editor-preview-render.h"
#include <webkit/webkit.h>

struct _EditorPagePreview {
  GtkToggleButton *source;
  GtkToggleButton *preview;
  GtkToggleButton *split;
  GtkWidget *format;
  GtkLabel *status;
  WebKitWebView *web;
  EditorPreviewKind kind;
  char *mode;
  guint timeout;
  guint generation;
  GCancellable *cancel;
  gboolean changing;
  gboolean chosen;
};

typedef struct {
  GWeakRef page;
  char *text;
  EditorPreviewKind kind;
  gboolean dark;
  guint generation;
} RenderJob;

static void queue_render (EditorPage *self);

static gboolean
is_dark (void)
{
  return adw_style_manager_get_dark (adw_style_manager_get_default ());
}

static void
job_free (gpointer data)
{
  RenderJob *job = data;
  g_weak_ref_clear (&job->page);
  g_free (job->text);
  g_free (job);
}

static void
render_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  RenderJob *job = data;
  g_autoptr(GError) error = NULL;
  char *html;
  if (g_task_return_error_if_cancelled (task)) return;
  html = editor_preview_render (job->text, job->kind, job->dark, &error);
  if (!html) html = editor_preview_error_html (error->message, job->dark);
  g_task_return_pointer (task, html, g_free);
}

static void
render_done (GObject *source, GAsyncResult *result, gpointer unused)
{
  GTask *task = G_TASK (result);
  RenderJob *job = g_task_get_task_data (task);
  g_autoptr(EditorPage) self = g_weak_ref_get (&job->page);
  g_autofree char *html = g_task_propagate_pointer (task, NULL);
  if (self && self->preview_state && html &&
      self->preview_state->generation == job->generation && self->preview_state->web)
    {
      webkit_web_view_load_html (self->preview_state->web, html, "about:blank");
      gtk_label_set_text (self->preview_state->status,
                          job->kind == EDITOR_PREVIEW_MARKDOWN ? "Markdown · Read-only" :
                          job->kind == EDITOR_PREVIEW_JSON ? "JSON · Read-only" : "JSON Lines · Read-only");
    }
}

static gboolean
render_timeout (gpointer data)
{
  EditorPage *self = data;
  EditorPagePreview *p = self->preview_state;
  GtkTextIter start, end;
  RenderJob *job;
  g_autoptr(GTask) task = NULL;
  p->timeout = 0;
  if (p->kind == EDITOR_PREVIEW_NONE || !p->web || g_str_equal (p->mode, "source"))
    return G_SOURCE_REMOVE;
  if (gtk_text_buffer_get_char_count (GTK_TEXT_BUFFER (self->document)) > EDITOR_PREVIEW_MAX_BYTES)
    {
      g_autofree char *html = editor_preview_error_html (
        "Preview is limited to 2 MiB. Your complete source remains available in Source view.", is_dark ());
      webkit_web_view_load_html (p->web, html, "about:blank");
      gtk_label_set_text (p->status, "Preview size limit");
      return G_SOURCE_REMOVE;
    }
  job = g_new0 (RenderJob, 1);
  g_weak_ref_init (&job->page, self);
  gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (self->document), &start, &end);
  job->text = gtk_text_buffer_get_text (GTK_TEXT_BUFFER (self->document), &start, &end, TRUE);
  job->kind = p->kind;
  job->dark = is_dark ();
  job->generation = p->generation;
  task = g_task_new (NULL, p->cancel, render_done, NULL);
  g_task_set_task_data (task, job, job_free);
  g_task_run_in_thread (task, render_worker);
  return G_SOURCE_REMOVE;
}

static void
queue_render (EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  if (!p) return;
  p->generation++;
  if (p->timeout) { g_source_remove (p->timeout); p->timeout = 0; }
  if (p->cancel) g_cancellable_cancel (p->cancel);
  g_clear_object (&p->cancel);
  p->cancel = g_cancellable_new ();
  if (p->kind != EDITOR_PREVIEW_NONE && !g_str_equal (p->mode, "source"))
    {
      gtk_label_set_text (p->status, "Updating preview…");
      p->timeout = g_timeout_add (250, render_timeout, self);
    }
}

static gboolean
navigation_policy (WebKitWebView *web, WebKitPolicyDecision *decision,
                   WebKitPolicyDecisionType type, gpointer data)
{
  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION ||
      type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    {
      WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action (
        WEBKIT_NAVIGATION_POLICY_DECISION (decision));
      const char *uri = webkit_uri_request_get_uri (webkit_navigation_action_get_request (action));
      if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION ||
          g_strcmp0 (uri, "about:blank") != 0 || webkit_navigation_action_is_user_gesture (action))
        { webkit_policy_decision_ignore (decision); return TRUE; }
    }
  return FALSE;
}

static gboolean
no_context_menu (WebKitWebView *web, WebKitContextMenu *menu,
                 WebKitHitTestResult *hit, gpointer data)
{
  return TRUE;
}

static void
web_terminated (WebKitWebView *web, WebKitWebProcessTerminationReason reason, EditorPage *self)
{
  if (!self->preview_state) return;
  gtk_label_set_text (self->preview_state->status, "Preview stopped — switch to Source, then Preview to retry");
}

static void
ensure_web (EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  g_autoptr(WebKitNetworkSession) session = NULL;
  WebKitSettings *settings;
  if (p->web) return;
  session = webkit_network_session_new_ephemeral ();
  p->web = g_object_new (WEBKIT_TYPE_WEB_VIEW, "network-session", session, NULL);
  gtk_widget_set_name (GTK_WIDGET (p->web), "document-preview");
  gtk_widget_set_hexpand (GTK_WIDGET (p->web), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (p->web), TRUE);
  settings = webkit_web_view_get_settings (p->web);
  webkit_settings_set_enable_javascript (settings, FALSE);
  webkit_settings_set_enable_html5_database (settings, FALSE);
  webkit_settings_set_enable_html5_local_storage (settings, FALSE);
  webkit_settings_set_enable_developer_extras (settings, FALSE);
  webkit_settings_set_media_playback_requires_user_gesture (settings, TRUE);
  g_signal_connect (p->web, "decide-policy", G_CALLBACK (navigation_policy), NULL);
  g_signal_connect (p->web, "context-menu", G_CALLBACK (no_context_menu), NULL);
  g_signal_connect_object (p->web, "web-process-terminated", G_CALLBACK (web_terminated), self, 0);
  gtk_box_append (self->preview_box, GTK_WIDGET (p->web));
}

void
_editor_page_preview_set_mode (EditorPage *self, const char *mode)
{
  EditorPagePreview *p = self->preview_state;
  gboolean supported;
  if (!p || (!g_str_equal (mode, "source") && !g_str_equal (mode, "preview") && !g_str_equal (mode, "split"))) return;
  p->chosen = TRUE;
  g_free (p->mode);
  p->mode = g_strdup (mode);
  supported = p->kind != EDITOR_PREVIEW_NONE;
  p->changing = TRUE;
  gtk_toggle_button_set_active (p->source, g_str_equal (mode, "source"));
  gtk_toggle_button_set_active (p->preview, g_str_equal (mode, "preview"));
  gtk_toggle_button_set_active (p->split, g_str_equal (mode, "split"));
  p->changing = FALSE;
  gtk_widget_set_visible (self->source_box, !supported || !g_str_equal (mode, "preview"));
  gtk_widget_set_visible (GTK_WIDGET (self->preview_box), supported && !g_str_equal (mode, "source"));
  if (supported && !g_str_equal (mode, "source"))
    {
      ensure_web (self);
      if (g_str_equal (mode, "split") && gtk_paned_get_position (self->preview_paned) < 100)
        gtk_paned_set_position (self->preview_paned, MAX (300, gtk_widget_get_width (GTK_WIDGET (self)) / 2));
    }
  else if (gtk_widget_get_root (GTK_WIDGET (self)))
    gtk_widget_grab_focus (GTK_WIDGET (self->view));
  queue_render (self);
}

void
_editor_page_preview_toggle (EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  if (p && p->kind != EDITOR_PREVIEW_NONE)
    _editor_page_preview_set_mode (self, g_str_equal (p->mode, "source") ? "split" : "source");
}

static void
mode_toggled (GtkToggleButton *button, EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  if (!p->changing && gtk_toggle_button_get_active (button))
    _editor_page_preview_set_mode (self, button == p->source ? "source" : button == p->preview ? "preview" : "split");
}

static void
update_kind (EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  GFile *file = editor_document_get_file (self->document);
  GtkSourceLanguage *language = gtk_source_buffer_get_language (GTK_SOURCE_BUFFER (self->document));
  g_autofree char *basename = file ? g_file_get_basename (file) : NULL;
  EditorPreviewKind previous;
  gboolean chosen;
  g_autofree char *mode = NULL;
  if (!p) return;
  previous = p->kind;
  chosen = p->chosen;
  mode = g_strdup (p->mode);
  p->kind = editor_preview_detect (basename, language ? gtk_source_language_get_id (language) : NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->preview_toolbar), p->kind != EDITOR_PREVIEW_NONE);
  gtk_widget_set_visible (p->format, p->kind == EDITOR_PREVIEW_JSON);
  if (p->kind != previous)
    {
      _editor_page_preview_set_mode (self, !chosen && p->kind != EDITOR_PREVIEW_NONE ? "split" : mode);
      p->chosen = chosen;
    }
  else queue_render (self);
}

static void
show_format_error (EditorPage *self, const char *message)
{
  AdwDialog *dialog = adw_alert_dialog_new ("Cannot Format JSON", message);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", "Close");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "close");
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

void
_editor_page_preview_format_json (EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER (self->document);
  GtkTextIter start, end, cursor;
  g_autofree char *text = NULL;
  g_autofree char *formatted = NULL;
  g_autoptr(GError) error = NULL;
  int offset;
  if (!p || p->kind != EDITOR_PREVIEW_JSON)
    { show_format_error (self, "Format JSON works on JSON documents only. JSON Lines records must stay on separate lines."); return; }
  if (!gtk_text_view_get_editable (GTK_TEXT_VIEW (self->view)) || editor_document_get_busy (self->document))
    { show_format_error (self, "This document is currently read-only or busy."); return; }
  if (gtk_text_buffer_get_char_count (buffer) > EDITOR_PREVIEW_MAX_BYTES)
    { show_format_error (self, "Formatting is limited to 2 MiB. Your source has not been changed."); return; }
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  text = gtk_text_buffer_get_text (buffer, &start, &end, TRUE);
  formatted = editor_preview_format_json (text, &error);
  if (!formatted) { show_format_error (self, error->message); return; }
  if (g_str_equal (formatted, text)) return;
  gtk_text_buffer_get_iter_at_mark (buffer, &cursor, gtk_text_buffer_get_insert (buffer));
  offset = gtk_text_iter_get_offset (&cursor);
  gtk_text_buffer_begin_user_action (buffer);
  gtk_text_buffer_delete (buffer, &start, &end);
  gtk_text_buffer_insert (buffer, &start, formatted, -1);
  gtk_text_buffer_end_user_action (buffer);
  gtk_text_buffer_get_iter_at_offset (buffer, &cursor, MIN (offset, gtk_text_buffer_get_char_count (buffer)));
  gtk_text_buffer_place_cursor (buffer, &cursor);
}

static void
format_clicked (GtkButton *button, EditorPage *self)
{
  _editor_page_preview_format_json (self);
}

void
_editor_page_preview_init (EditorPage *self)
{
  EditorPagePreview *p = g_new0 (EditorPagePreview, 1);
  GtkWidget *group = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  self->preview_state = p;
  p->mode = g_strdup ("source");
  p->source = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("Source"));
  p->preview = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("Preview"));
  p->split = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("Split View"));
  gtk_toggle_button_set_group (p->preview, p->source);
  gtk_toggle_button_set_group (p->split, p->source);
  gtk_toggle_button_set_active (p->source, TRUE);
  gtk_widget_add_css_class (group, "linked");
  gtk_box_append (GTK_BOX (group), GTK_WIDGET (p->source));
  gtk_box_append (GTK_BOX (group), GTK_WIDGET (p->preview));
  gtk_box_append (GTK_BOX (group), GTK_WIDGET (p->split));
  gtk_box_append (self->preview_toolbar, group);
  p->status = GTK_LABEL (gtk_label_new ("Read-only preview"));
  gtk_label_set_ellipsize (p->status, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (GTK_WIDGET (p->status), TRUE);
  gtk_label_set_xalign (p->status, 1.0);
  gtk_widget_add_css_class (GTK_WIDGET (p->status), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (p->status), "caption");
  gtk_box_append (self->preview_toolbar, GTK_WIDGET (p->status));
  p->format = gtk_button_new_with_label ("Format JSON");
  gtk_widget_set_tooltip_text (p->format, "Indent JSON with two spaces. Changes the source; undo with Ctrl+Z. Does not save automatically. (Ctrl+Shift+J)");
  gtk_box_append (self->preview_toolbar, p->format);
  g_signal_connect_object (p->source, "toggled", G_CALLBACK (mode_toggled), self, 0);
  g_signal_connect_object (p->preview, "toggled", G_CALLBACK (mode_toggled), self, 0);
  g_signal_connect_object (p->split, "toggled", G_CALLBACK (mode_toggled), self, 0);
  g_signal_connect_object (p->format, "clicked", G_CALLBACK (format_clicked), self, 0);
  g_signal_connect_object (self->document, "changed", G_CALLBACK (queue_render), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->document, "notify::file", G_CALLBACK (update_kind), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->document, "notify::language", G_CALLBACK (update_kind), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (adw_style_manager_get_default (), "notify::dark", G_CALLBACK (queue_render), self, G_CONNECT_SWAPPED);
  update_kind (self);
}

void
_editor_page_preview_shutdown (EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  if (!p) return;
  self->preview_state = NULL;
  if (p->timeout) g_source_remove (p->timeout);
  if (p->cancel) g_cancellable_cancel (p->cancel);
  g_clear_object (&p->cancel);
  g_free (p->mode);
  g_free (p);
}
