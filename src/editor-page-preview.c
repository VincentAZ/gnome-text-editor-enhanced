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
  GtkWidget *failure;
  gboolean web_failed;
  EditorPreviewKind kind;
  char *mode;
  guint timeout;
  guint generation;
  GCancellable *cancel;
  gboolean changing;
  gboolean chosen;
  int offset;
  int next_offset;
  int expand;
  guint revision;
  gboolean formatting;
  GArray *history;
  gboolean plain_long_line;
  gboolean guarding_layout;
  gboolean saved_highlight;
  GtkWrapMode saved_wrap;
  GtkSourceLanguage *saved_language;
  GtkWidget *first;
  GtkWidget *previous;
  GtkWidget *next;
  GtkWidget *last;
  GtkSpinButton *line;
  GtkLabel *range;
  GtkWidget *navigation;

};

typedef struct {
  GWeakRef page;
  char *text;
  EditorPreviewKind kind;
  gboolean dark;
  guint generation;
  guint first_line;
  gboolean partial;
  gboolean starts_midline;
  gboolean ends_midline;
  int expand;
  char *status;
} RenderJob;

static void queue_render (EditorPage *self);
static void protect_long_lines (EditorPage *self, gboolean restore);

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
  g_free (job->status);
  g_free (job);
}

static void
render_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  RenderJob *job = data;
  g_autoptr(GError) error = NULL;
  char *html;
  if (g_task_return_error_if_cancelled (task)) return;
  html = editor_preview_render_section (job->text, job->kind, job->dark, job->first_line,
                                                        job->partial, job->starts_midline,
                                                        job->ends_midline, job->expand, &error);
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
      self->preview_state->web_failed = FALSE;
      webkit_settings_set_enable_javascript (
          webkit_web_view_get_settings (self->preview_state->web), job->kind == EDITOR_PREVIEW_HTML);
      webkit_web_view_load_html (self->preview_state->web, html, "about:blank");
      gtk_label_set_text (self->preview_state->status, job->status);
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
  protect_long_lines (self, TRUE);
  if (p->kind == EDITOR_PREVIEW_NONE || !p->web || g_str_equal (p->mode, "source"))
    return G_SOURCE_REMOVE;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER (self->document);
  GtkTextIter line_end;
  int count = gtk_text_buffer_get_char_count (buffer);
  p->offset = CLAMP (p->offset, 0, MAX (0, count - 1));
  gtk_text_buffer_get_iter_at_offset (buffer, &start, p->offset);
  end = start;
  gtk_text_iter_forward_chars (&end, EDITOR_PREVIEW_SECTION_CHARS);
  line_end = start;
  gtk_text_iter_forward_lines (&line_end, EDITOR_PREVIEW_SECTION_LINES);
  if (gtk_text_iter_compare (&line_end, &end) < 0) end = line_end;
  if (p->kind == EDITOR_PREVIEW_HTML)
    { p->offset = 0; gtk_text_buffer_get_bounds (buffer, &start, &end); }
  p->next_offset = gtk_text_iter_get_offset (&end);
  job = g_new0 (RenderJob, 1);
  g_weak_ref_init (&job->page, self);
  job->text = gtk_text_buffer_get_text (buffer, &start, &end, TRUE);
  job->kind = p->kind;
  job->dark = is_dark ();
  job->generation = p->generation;
  job->first_line = gtk_text_iter_get_line (&start) + 1;
  job->partial = p->offset > 0 || p->next_offset < count;
  job->starts_midline = !gtk_text_iter_starts_line (&start);
  job->ends_midline = !gtk_text_iter_is_end (&end) && !gtk_text_iter_starts_line (&end);
  job->expand = p->expand;
  job->status = g_strdup_printf ("Lines %u–%u of %d · Read-only", job->first_line,
                                gtk_text_iter_get_line (&end) + (gtk_text_iter_starts_line (&end) && p->next_offset > 0 ? 0 : 1),
                                gtk_text_buffer_get_line_count (buffer));
  if (p->kind == EDITOR_PREVIEW_HTML)
    {
      g_free (job->status);
      job->status = g_strdup ("HTML · Offline, isolated preview · Source unchanged");
    }
  if (p->plain_long_line)
    {
      char *base = job->status;
      job->status = g_strconcat ("Long-line source mode · ", base, NULL);
      g_free (base);
    }
  gtk_label_set_text (p->range, job->status);
  g_autofree char *position = g_strdup_printf ("Characters %d–%d of %d. Sections are bounded for responsiveness; there is no whole-file preview cutoff.",
                                             count ? p->offset + 1 : 0, p->next_offset, count);
  gtk_widget_set_tooltip_text (GTK_WIDGET (p->range), position);
  gtk_widget_set_sensitive (p->first, p->offset > 0);
  gtk_widget_set_sensitive (p->previous, p->offset > 0);
  gtk_widget_set_sensitive (p->next, p->next_offset < count);
  gtk_widget_set_sensitive (p->last, p->next_offset < count);
  p->changing = TRUE;
  gtk_spin_button_set_range (p->line, 1, gtk_text_buffer_get_line_count (buffer));
  gtk_spin_button_set_value (p->line, job->first_line);
  p->changing = FALSE;
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

/* Pango shaping of tens of thousands of highlighted spans on one wrapped
 * logical line can monopolize the GTK thread. Keep the source editable, but
 * temporarily use a plain, unwrapped source view for such structured lines.
 * This is per-page state, never a change to the user's saved preferences. */
static void
protect_long_lines (EditorPage *self, gboolean restore)
{
  EditorPagePreview *p = self->preview_state;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER (self->document);
  GtkSourceBuffer *source = GTK_SOURCE_BUFFER (buffer);
  GtkTextView *view = GTK_TEXT_VIEW (self->view);
  GtkTextIter first, cursor, end;
  gboolean plain = FALSE;
  if (!p || p->guarding_layout) return;
  if (p->kind != EDITOR_PREVIEW_NONE && gtk_text_buffer_get_char_count (buffer) >= EDITOR_PREVIEW_SECTION_CHARS)
    {
      gtk_text_buffer_get_start_iter (buffer, &first);
      gtk_text_buffer_get_end_iter (buffer, &end);
      gtk_text_buffer_get_iter_at_mark (buffer, &cursor, gtk_text_buffer_get_insert (buffer));
      plain = gtk_text_iter_get_chars_in_line (&first) > 8192 ||
              gtk_text_iter_get_chars_in_line (&cursor) > 8192 ||
              gtk_text_iter_get_chars_in_line (&end) > 8192;
    }
  p->guarding_layout = TRUE;
  if (plain)
    {
      if (!p->plain_long_line)
        {
          p->saved_wrap = gtk_text_view_get_wrap_mode (view);
          p->saved_highlight = gtk_source_buffer_get_highlight_syntax (source);
          p->saved_language = gtk_source_buffer_get_language (source);
          if (p->saved_language) g_object_ref (p->saved_language);
          p->plain_long_line = TRUE;
        }
      gtk_text_view_set_wrap_mode (view, GTK_WRAP_NONE);
      gtk_source_buffer_set_highlight_syntax (source, FALSE);
      gtk_source_buffer_set_language (source, NULL);
    }
  else if (restore && p->plain_long_line)
    {
      g_autoptr(GtkSourceLanguage) language = g_steal_pointer (&p->saved_language);
      p->plain_long_line = FALSE;
      gtk_source_buffer_set_language (source, language);
      gtk_text_view_set_wrap_mode (view, p->saved_wrap);
      gtk_source_buffer_set_highlight_syntax (source, p->saved_highlight);
    }
  p->guarding_layout = FALSE;
}

static void
source_layout_changed (GObject *object, GParamSpec *pspec, EditorPage *self)
{
  protect_long_lines (self, FALSE);
}

static void
document_changed (EditorPage *self)
{
  protect_long_lines (self, FALSE);
  if (self->preview_state) self->preview_state->revision++;
  queue_render (self);
}

void
_editor_page_preview_go_to_offset (EditorPage *self, int offset)
{
  EditorPagePreview *p = self->preview_state;
  if (!p) return;
  p->offset = CLAMP (offset, 0, gtk_text_buffer_get_char_count (GTK_TEXT_BUFFER (self->document)));
  g_array_set_size (p->history, 0);
  queue_render (self);
}

static void
section_clicked (GtkButton *button, EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER (self->document);
  int count = gtk_text_buffer_get_char_count (buffer);
  if (GTK_WIDGET (button) == p->next)
    {
      if (p->next_offset >= count) return;
      g_array_append_val (p->history, p->offset);
      p->offset = p->next_offset;
    }
  else if (GTK_WIDGET (button) == p->previous)
    {
      if (p->history->len)
        { p->offset = g_array_index (p->history, int, p->history->len - 1); g_array_set_size (p->history, p->history->len - 1); }
      else
        {
          GtkTextIter previous;
          gtk_text_buffer_get_iter_at_offset (buffer, &previous, p->offset);
          gtk_text_iter_backward_lines (&previous, EDITOR_PREVIEW_SECTION_LINES);
          p->offset = MAX (MAX (0, p->offset - EDITOR_PREVIEW_SECTION_CHARS), gtk_text_iter_get_offset (&previous));
        }
    }
  else
    {
      GtkTextIter tail;
      gtk_text_buffer_get_iter_at_line (buffer, &tail, MAX (0, gtk_text_buffer_get_line_count (buffer) - EDITOR_PREVIEW_SECTION_LINES));
      p->offset = GTK_WIDGET (button) == p->first ? 0 : MAX (MAX (0, count - EDITOR_PREVIEW_SECTION_CHARS), gtk_text_iter_get_offset (&tail));
      g_array_set_size (p->history, 0);
    }
  queue_render (self);
}

static void
line_changed (GtkSpinButton *spin, EditorPage *self)
{
  GtkTextIter iter;
  if (self->preview_state->changing) return;
  gtk_text_buffer_get_iter_at_line (GTK_TEXT_BUFFER (self->document), &iter,
                                   gtk_spin_button_get_value_as_int (spin) - 1);
  _editor_page_preview_go_to_offset (self, gtk_text_iter_get_offset (&iter));
}

static void
at_cursor (GtkButton *button, EditorPage *self)
{
  GtkTextIter iter;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER (self->document);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, gtk_text_buffer_get_insert (buffer));
  _editor_page_preview_go_to_offset (self, gtk_text_iter_get_offset (&iter));
}

static void
view_option (GtkButton *button, EditorPage *self)
{
  const char *label = gtk_button_get_label (button);
  EditorPagePreview *p = self->preview_state;
  if (g_str_equal (label, "Expand All")) p->expand = 1;
  else if (g_str_equal (label, "Collapse All")) p->expand = -1;
  else
    {
      GtkOrientation orientation = g_str_equal (label, "Stacked") ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
      gtk_orientable_set_orientation (GTK_ORIENTABLE (self->preview_paned), orientation);
      gtk_paned_set_position (self->preview_paned, MAX (1, (orientation == GTK_ORIENTATION_VERTICAL ?
          gtk_widget_get_height (GTK_WIDGET (self->preview_paned)) : gtk_widget_get_width (GTK_WIDGET (self->preview_paned))) / 2));
      _editor_page_preview_set_mode (self, "split");
    }
  queue_render (self);
}

static GtkWidget *
nav_button (GtkBox *row, const char *icon, const char *name, GCallback callback, EditorPage *self)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon);
  gtk_widget_set_tooltip_text (button, name);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_signal_connect_object (button, "clicked", callback, self, 0);
  gtk_box_append (row, button);
  return button;
}

static void
setup_navigation (EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
  GtkBox *row = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3));
  gtk_widget_set_margin_start (box, 6); gtk_widget_set_margin_end (box, 6);
  gtk_widget_set_margin_top (box, 3); gtk_widget_set_margin_bottom (box, 3);
  p->range = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_ellipsize (p->range, PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (p->range));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (row));
  p->first = nav_button (row, "go-first-symbolic", "First Section", G_CALLBACK (section_clicked), self);
  p->previous = nav_button (row, "go-previous-symbolic", "Previous Section", G_CALLBACK (section_clicked), self);
  p->next = nav_button (row, "go-next-symbolic", "Next Section", G_CALLBACK (section_clicked), self);
  p->last = nav_button (row, "go-last-symbolic", "Last Section", G_CALLBACK (section_clicked), self);
  gtk_box_append (row, gtk_label_new ("Line"));
  p->line = GTK_SPIN_BUTTON (gtk_spin_button_new_with_range (1, 1, 1));
  gtk_widget_set_hexpand (GTK_WIDGET (p->line), TRUE);
  gtk_editable_set_width_chars (GTK_EDITABLE (p->line), 5);
  gtk_widget_set_tooltip_text (GTK_WIDGET (p->line), "Go to line in preview");
  gtk_accessible_update_property (GTK_ACCESSIBLE (p->line), GTK_ACCESSIBLE_PROPERTY_LABEL, "Go to line in preview", -1);
  g_signal_connect_object (p->line, "value-changed", G_CALLBACK (line_changed), self, 0);
  gtk_box_append (row, GTK_WIDGET (p->line));
  nav_button (row, "find-location-symbolic", "Preview at Cursor", G_CALLBACK (at_cursor), self);
  p->navigation = box;
  gtk_box_append (self->preview_box, box);
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
          (g_strcmp0 (uri, "about:blank") != 0 && g_strcmp0 (uri, "about:srcdoc") != 0) ||
          webkit_navigation_action_is_user_gesture (action))
        { webkit_policy_decision_ignore (decision); return TRUE; }
    }
  return FALSE;
}

static gboolean
deny_permission (WebKitWebView *web, WebKitPermissionRequest *request, gpointer data)
{
  webkit_permission_request_deny (request);
  return TRUE;
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
  EditorPagePreview *p = self->preview_state;
  p->web_failed = TRUE;
  gtk_widget_set_visible (GTK_WIDGET (p->web), FALSE);
  gtk_widget_set_visible (p->failure, TRUE);
  gtk_label_set_text (p->status, "Preview unavailable · Source unchanged");
}

static gboolean
web_load_failed (WebKitWebView *web, WebKitLoadEvent event,
                 const char *uri, GError *error, EditorPage *self)
{
  if (g_error_matches (error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
    return FALSE;
  web_terminated (web, WEBKIT_WEB_PROCESS_CRASHED, self);
  return TRUE; /* Never replace the document with WebKit's generic error page. */
}

static void
web_load_changed (WebKitWebView *web, WebKitLoadEvent event, EditorPage *self)
{
  EditorPagePreview *p = self->preview_state;
  if (!p || event != WEBKIT_LOAD_FINISHED || p->web_failed) return;
  gtk_widget_set_visible (p->failure, FALSE);
  gtk_widget_set_visible (GTK_WIDGET (web), TRUE);
}

static void
retry_preview (GtkButton *button, EditorPage *self)
{
  if (!self->preview_state) return;
  queue_render (self);
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
  g_signal_connect (p->web, "permission-request", G_CALLBACK (deny_permission), NULL);
  g_signal_connect_object (p->web, "web-process-terminated", G_CALLBACK (web_terminated), self, 0);
  gtk_box_append (self->preview_box, GTK_WIDGET (p->web));
  p->failure = adw_status_page_new ();
  gtk_widget_set_name (p->failure, "preview-recovery");
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (p->failure), "dialog-warning-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (p->failure), "Preview Unavailable");
  adw_status_page_set_description (ADW_STATUS_PAGE (p->failure),
      "The preview process stopped. Your document is unchanged and you can still edit it in Source.");
  GtkWidget *retry = gtk_button_new_with_label ("Retry Preview");
  gtk_widget_set_halign (retry, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (retry, "suggested-action");
  gtk_widget_set_tooltip_text (retry, "Retry Preview");
  g_signal_connect_object (retry, "clicked", G_CALLBACK (retry_preview), self, 0);
  adw_status_page_set_child (ADW_STATUS_PAGE (p->failure), retry);
  gtk_widget_set_vexpand (p->failure, TRUE);
  gtk_widget_set_visible (p->failure, FALSE);
  gtk_box_append (self->preview_box, p->failure);
  g_signal_connect_object (p->web, "load-failed", G_CALLBACK (web_load_failed), self, 0);
  g_signal_connect_object (p->web, "load-changed", G_CALLBACK (web_load_changed), self, 0);
  setup_navigation (self);
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
      gtk_widget_set_visible (p->navigation, p->kind != EDITOR_PREVIEW_HTML);
      if (g_str_equal (mode, "split") && gtk_paned_get_position (self->preview_paned) < 100)
        gtk_paned_set_position (self->preview_paned, MAX (300, gtk_widget_get_width (GTK_WIDGET (self)) / 2));
    }
  else if (gtk_widget_get_root (GTK_WIDGET (self)))
    gtk_widget_grab_focus (GTK_WIDGET (self->view));
  /* Stop hidden interactive documents instead of leaving their timers running. */
  if (p->web && (!supported || g_str_equal (mode, "source")) &&
      webkit_settings_get_enable_javascript (webkit_web_view_get_settings (p->web)))
    {
      webkit_settings_set_enable_javascript (webkit_web_view_get_settings (p->web), FALSE);
      webkit_web_view_load_html (p->web, "", "about:blank");
    }
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
  if (!language && p->plain_long_line) language = p->saved_language;
  previous = p->kind;
  chosen = p->chosen;
  mode = g_strdup (p->mode);
  p->kind = editor_preview_detect (basename, language ? gtk_source_language_get_id (language) : NULL);
  protect_long_lines (self, FALSE);
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

typedef struct {
  GWeakRef page;
  char *text;
  guint revision;
  int cursor;
} FormatJob;

static void
format_job_free (gpointer data)
{
  FormatJob *job = data;
  g_weak_ref_clear (&job->page); g_free (job->text); g_free (job);
}

static void
apply_format (EditorPage *self, const char *formatted, int offset)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER (self->document);
  GtkTextIter start, end, cursor;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  gtk_text_buffer_begin_user_action (buffer);
  gtk_text_buffer_delete (buffer, &start, &end);
  gtk_text_buffer_insert (buffer, &start, formatted, -1);
  gtk_text_buffer_end_user_action (buffer);
  gtk_text_buffer_get_iter_at_offset (buffer, &cursor, MIN (offset, gtk_text_buffer_get_char_count (buffer)));
  gtk_text_buffer_place_cursor (buffer, &cursor);
}

static void
format_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  FormatJob *job = data;
  g_autoptr(GError) error = NULL;
  char *formatted = editor_preview_format_json (job->text, &error);
  if (formatted) g_task_return_pointer (task, formatted, g_free);
  else g_task_return_error (task, g_steal_pointer (&error));
}

static void
format_done (GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = G_TASK (result);
  FormatJob *job = g_task_get_task_data (task);
  g_autoptr(EditorPage) self = g_weak_ref_get (&job->page);
  g_autoptr(GError) error = NULL;
  g_autofree char *formatted = g_task_propagate_pointer (task, &error);
  if (!self || !self->preview_state) return;
  EditorPagePreview *p = self->preview_state;
  p->formatting = FALSE;
  gtk_widget_set_sensitive (p->format, TRUE);
  if (p->revision != job->revision)
    { show_format_error (self, "The document changed while formatting. No changes were applied; try Format JSON again."); return; }
  if (error) { show_format_error (self, error->message); return; }
  if (gtk_text_view_get_editable (GTK_TEXT_VIEW (self->view)) && !editor_document_get_busy (self->document))
    apply_format (self, formatted, job->cursor);
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
  if (p->formatting) return;
  if (!gtk_text_view_get_editable (GTK_TEXT_VIEW (self->view)) || editor_document_get_busy (self->document))
    { show_format_error (self, "This document is read-only or busy; its source has not been changed."); return; }
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  text = gtk_text_buffer_get_text (buffer, &start, &end, TRUE);
  gtk_text_buffer_get_iter_at_mark (buffer, &cursor, gtk_text_buffer_get_insert (buffer));
  offset = gtk_text_iter_get_offset (&cursor);
  if (gtk_text_buffer_get_char_count (buffer) > EDITOR_PREVIEW_SECTION_CHARS)
    {
      FormatJob *job = g_new0 (FormatJob, 1);
      g_autoptr(GTask) task = NULL;
      g_weak_ref_init (&job->page, self); job->text = g_steal_pointer (&text);
      job->cursor = offset; job->revision = p->revision;
      p->formatting = TRUE; gtk_widget_set_sensitive (p->format, FALSE);
      task = g_task_new (NULL, NULL, format_done, NULL);
      g_task_set_task_data (task, job, format_job_free);
      g_task_run_in_thread (task, format_worker);
      return;
    }
  formatted = editor_preview_format_json (text, &error);
  if (!formatted) { show_format_error (self, error->message); return; }
  if (strcmp (text, formatted) != 0) apply_format (self, formatted, offset);
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
  p->history = g_array_new (FALSE, FALSE, sizeof (int));
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
  {
    GtkWidget *menu = gtk_menu_button_new ();
    GtkWidget *popover = gtk_popover_new ();
    GtkWidget *options = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
    const char *labels[] = { "Side by Side", "Stacked", "Expand All", "Collapse All" };
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu), "view-more-symbolic");
    gtk_widget_set_tooltip_text (menu, "Preview View Options");
    gtk_popover_set_child (GTK_POPOVER (popover), options);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (menu), popover);
    for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
      {
        GtkWidget *button = gtk_button_new_with_label (labels[i]);
        g_signal_connect_object (button, "clicked", G_CALLBACK (view_option), self, 0);
        gtk_box_append (GTK_BOX (options), button);
      }
    gtk_box_append (self->preview_toolbar, menu);
  }
  gtk_box_append (self->preview_toolbar, GTK_WIDGET (p->status));
  p->format = gtk_button_new_with_label ("Format JSON");
  gtk_widget_set_tooltip_text (p->format, "Indent JSON with two spaces. Changes the source; undo with Ctrl+Z. Does not save automatically. (Ctrl+Shift+J)");
  gtk_box_append (self->preview_toolbar, p->format);
  g_signal_connect_object (p->source, "toggled", G_CALLBACK (mode_toggled), self, 0);
  g_signal_connect_object (p->preview, "toggled", G_CALLBACK (mode_toggled), self, 0);
  g_signal_connect_object (p->split, "toggled", G_CALLBACK (mode_toggled), self, 0);
  g_signal_connect_object (p->format, "clicked", G_CALLBACK (format_clicked), self, 0);
  g_signal_connect_object (self->document, "changed", G_CALLBACK (document_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->document, "notify::highlight-syntax", G_CALLBACK (source_layout_changed), self, 0);
  g_signal_connect_object (self->view, "notify::wrap-mode", G_CALLBACK (source_layout_changed), self, 0);
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
  g_array_unref (p->history);
  g_clear_object (&p->saved_language);
  g_free (p->mode);
  g_free (p);
}
