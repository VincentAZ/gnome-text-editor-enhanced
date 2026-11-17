/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "editor-application-private.h"
#include "editor-document.h"
#include "editor-page-private.h"
#include "editor-session.h"
#include "editor-window.h"
#include <webkit/webkit.h>
#include <string.h>

static char *dom_text;
static gboolean evaluating;

static void spin (guint milliseconds)
{
  gint64 until = g_get_monotonic_time () + milliseconds * 1000;
  do
    {
      while (g_main_context_iteration (NULL, FALSE));
      g_usleep (1000);
    }
  while (g_get_monotonic_time () < until);
}

static void resource_received (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  gsize length = 0;
  guchar *bytes = webkit_web_resource_get_data_finish (WEBKIT_WEB_RESOURCE (object), result, &length, &error);
  if (bytes) { dom_text = g_strndup ((char *)bytes, length); g_free (bytes); }
  else dom_text = g_strdup (error->message);
  evaluating = FALSE;
}

static char *body_text (EditorPage *page)
{
  WebKitWebView *web = WEBKIT_WEB_VIEW (gtk_widget_get_first_child (GTK_WIDGET (page->preview_box)));
  gint64 deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  g_assert_true (WEBKIT_IS_WEB_VIEW (web));
  spin (400);
  while (webkit_web_view_is_loading (web) && g_get_monotonic_time () < deadline) spin (10);
  g_assert_false (webkit_web_view_is_loading (web));
  evaluating = TRUE;
  g_assert_nonnull (webkit_web_view_get_main_resource (web));
  webkit_web_resource_get_data (webkit_web_view_get_main_resource (web), NULL, resource_received, NULL);
  while (evaluating && g_get_monotonic_time () < deadline) spin (10);
  g_assert_false (evaluating);
  return g_steal_pointer (&dom_text);
}

static char *source_text (EditorPage *page)
{
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (page->document), &start, &end);
  return gtk_text_buffer_get_text (GTK_TEXT_BUFFER (page->document), &start, &end, FALSE);
}

static EditorPage *new_page (EditorWindow *window, const char *name, const char *contents)
{
  g_autofree char *path = g_build_filename (g_get_tmp_dir (), name, NULL);
  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(EditorDocument) document = editor_document_new_for_file (file);
  EditorPage *page;
  gtk_text_buffer_set_text (GTK_TEXT_BUFFER (document), contents, -1);
  gtk_text_buffer_set_modified (GTK_TEXT_BUFFER (document), FALSE);
  page = editor_session_add_document (EDITOR_SESSION_DEFAULT, window, document);
  gtk_window_present (GTK_WINDOW (window));
  spin (100);
  return page;
}

int main (int argc, char **argv)
{
  g_autoptr(EditorApplication) app = NULL;
  g_autoptr(GError) error = NULL;
  EditorWindow *window;
  EditorPage *page;
  GtkTextIter end;
  const char *json = "{\"id\":123456789012345678901234567890,\"ok\":true,\"a\":[1,2]}";
  g_test_init (&argc, &argv, NULL);
  /* Portals may be absent under the disposable test bus. Criticals stay fatal. */
  g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
  app = _editor_application_new (TRUE);
  g_assert_true (g_application_register (G_APPLICATION (app), NULL, &error));
  g_assert_no_error (error);
  window = editor_session_create_window (EDITOR_SESSION_DEFAULT);
  gtk_window_set_default_size (GTK_WINDOW (window), 1100, 750);

  page = new_page (window, "preview-integration.md", "# First heading\n\n**Strong text**\n");
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (page->preview_toolbar)));
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (page->preview_box)));
  g_assert_true (gtk_widget_get_visible (page->source_box));
  {
    g_autofree char *text = body_text (page);
    g_assert_nonnull (strstr (text, "First heading"));
    g_assert_nonnull (strstr (text, "Strong text"));
    g_assert_false (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)));
  }
  gtk_text_buffer_get_end_iter (GTK_TEXT_BUFFER (page->document), &end);
  gtk_text_buffer_insert (GTK_TEXT_BUFFER (page->document), &end, "\n## Live update\n", -1);
  {
    g_autofree char *text = body_text (page);
    g_assert_nonnull (strstr (text, "Live update"));
  }
  g_assert_true (gtk_widget_activate_action (GTK_WIDGET (window), "page.preview-mode", "s", "preview"));
  g_assert_false (gtk_widget_get_visible (page->source_box));
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (page->preview_box)));
  g_assert_true (gtk_widget_activate_action (GTK_WIDGET (window), "page.preview-mode", "s", "source"));
  g_assert_true (gtk_widget_get_visible (page->source_box));
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (page->preview_box)));
  g_assert_true (gtk_widget_activate_action (GTK_WIDGET (window), "page.toggle-preview", NULL));
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (page->preview_box)));
  g_print ("PASS Markdown auto-preview, live update, read-only preview, Source/Preview/Split actions\n");

  page = new_page (window, "preview-integration.json", json);
  {
    g_autofree char *text = body_text (page);
    g_assert_nonnull (strstr (text, "3 members"));
    g_assert_nonnull (strstr (text, "123456789012345678901234567890"));
    g_assert_false (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)));
  }
  g_assert_true (gtk_widget_activate_action (GTK_WIDGET (window), "page.format-json", NULL));
  {
    g_autofree char *text = source_text (page);
    g_assert_true (g_str_has_prefix (text, "{\n  \"id\": 123456789012345678901234567890,"));
    g_assert_true (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)));
    g_assert_true (gtk_text_buffer_get_can_undo (GTK_TEXT_BUFFER (page->document)));
  }
  gtk_text_buffer_undo (GTK_TEXT_BUFFER (page->document));
  {
    g_autofree char *text = source_text (page);
    g_assert_cmpstr (text, ==, json);
  }
  g_print ("PASS JSON tree, exact large numbers, Format JSON action and single-step Undo\n");

  gtk_text_buffer_set_text (GTK_TEXT_BUFFER (page->document), "{\"broken\":}", -1);
  gtk_text_buffer_set_modified (GTK_TEXT_BUFFER (page->document), FALSE);
  g_assert_true (gtk_widget_activate_action (GTK_WIDGET (window), "page.format-json", NULL));
  {
    g_autofree char *text = body_text (page);
    g_autofree char *source = source_text (page);
    g_assert_nonnull (strstr (text, "Line 1"));
    g_assert_cmpstr (source, ==, "{\"broken\":}");
    g_assert_false (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)));
  }
  g_print ("PASS Invalid JSON is explained and remains untouched\n");

  page = new_page (window, "preview-integration.jsonl", "{\"role\":\"user\"}\n\n{\"role\":\"assistant\"}\n");
  {
    g_autofree char *text = body_text (page);
    g_assert_nonnull (strstr (text, "Record 1 · Line 1"));
    g_assert_nonnull (strstr (text, "Record 2 · Line 3"));
  }
  g_assert_true (gtk_widget_activate_action (GTK_WIDGET (window), "page.format-json", NULL));
  {
    g_autofree char *text = source_text (page);
    g_assert_cmpstr (text, ==, "{\"role\":\"user\"}\n\n{\"role\":\"assistant\"}\n");
  }
  g_print ("PASS JSON Lines records, blank lines and no destructive whole-file formatting\n");

  page = new_page (window, "preview-integration.txt", "Plain text stays plain.\n");
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (page->preview_toolbar)));
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (page->preview_box)));
  {
    GtkSourceLanguageManager *manager = gtk_source_language_manager_get_default ();
    g_autofree char *text = NULL;
    gtk_source_buffer_set_language (GTK_SOURCE_BUFFER (page->document), gtk_source_language_manager_get_language (manager, "markdown"));
    g_assert_true (gtk_widget_get_visible (GTK_WIDGET (page->preview_toolbar)));
    _editor_page_preview_set_mode (page, "split");
    text = body_text (page);
    g_assert_nonnull (strstr (text, "Plain text stays plain."));
  }
  g_print ("PASS Plain files stay normal; changing Document Type enables the matching preview\n");
  gtk_window_destroy (GTK_WINDOW (window));
  spin (200);
  g_print ("PASS Window/page teardown without critical errors\n");
  return 0;
}
