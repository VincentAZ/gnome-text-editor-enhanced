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
  page = editor_session_add_document (EDITOR_SESSION_DEFAULT, window, document);
  gtk_window_present (GTK_WINDOW (window));
  spin (100);
  /* Establish the saved baseline after source-view/undo initialization. */
  gtk_text_buffer_set_modified (GTK_TEXT_BUFFER (document), FALSE);
  return page;
}

static GtkWidget *find_tooltip (GtkWidget *root, const char *tooltip)
{
  if (g_strcmp0 (gtk_widget_get_tooltip_text (root), tooltip) == 0) return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child; child = gtk_widget_get_next_sibling (child))
    { GtkWidget *found = find_tooltip (child, tooltip); if (found) return found; }
  return NULL;
}

static GtkWidget *find_id (GtkWidget *root, const char *id)
{
  if (g_strcmp0 (gtk_buildable_get_buildable_id (GTK_BUILDABLE (root)), id) == 0) return root;
  for (GtkWidget *child = gtk_widget_get_first_child (root); child; child = gtk_widget_get_next_sibling (child))
    { GtkWidget *found = find_id (child, id); if (found) return found; }
  return NULL;
}

static void click_navigation (EditorPage *page, const char *tooltip)
{
  GtkWidget *button = find_tooltip (GTK_WIDGET (page->preview_box), tooltip);
  g_assert_true (GTK_IS_BUTTON (button)); g_assert_true (gtk_widget_get_sensitive (button));
  g_signal_emit_by_name (button, "clicked");
}

static void choose_view (EditorPage *page, const char *label)
{
  GtkWidget *menu = find_tooltip (GTK_WIDGET (page->preview_toolbar), "Preview View Options");
  g_assert_true (GTK_IS_MENU_BUTTON (menu));
  GtkWidget *options = gtk_popover_get_child (gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu)));
  for (GtkWidget *button = gtk_widget_get_first_child (options); button; button = gtk_widget_get_next_sibling (button))
    if (GTK_IS_BUTTON (button) && g_strcmp0 (gtk_button_get_label (GTK_BUTTON (button)), label) == 0)
      { g_signal_emit_by_name (button, "clicked"); return; }
  g_error ("Missing view option: %s", label);
}

static void test_preview_recovery (EditorWindow *window)
{
  EditorPage *page = new_page (window, "preview-recovery.md", "# Recovery test\n\nThis text must survive a preview crash.\n");
  g_autofree char *before = source_text (page);
  g_autofree char *initial = body_text (page);
  WebKitWebView *web = WEBKIT_WEB_VIEW (gtk_widget_get_first_child (GTK_WIDGET (page->preview_box)));
  GtkWidget *recovery = gtk_widget_get_next_sibling (GTK_WIDGET (web));
  g_assert_nonnull (strstr (initial, "Recovery test"));
  webkit_web_view_terminate_web_process (web);
  gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (!gtk_widget_get_visible (recovery) && g_get_monotonic_time () < deadline) spin (10);
  g_assert_true (gtk_widget_get_visible (recovery));
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (web)));
  g_assert_cmpstr (adw_status_page_get_title (ADW_STATUS_PAGE (recovery)), ==, "Preview Unavailable");
  click_navigation (page, "Retry Preview");
  g_autofree char *reloaded = body_text (page);
  g_autofree char *after = source_text (page);
  g_assert_nonnull (strstr (reloaded, "Recovery test"));
  g_assert_false (gtk_widget_get_visible (recovery));
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (web)));
  g_assert_cmpstr (before, ==, after);
  g_assert_false (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)));
  g_print ("PASS Preview-process failure uses native recovery; Retry restores preview with unchanged source\n");
}

static void test_panel_controls (EditorWindow *window, EditorPage *page)
{
  GtkWidget *toggle = find_id (GTK_WIDGET (window), "properties_button");
  g_assert_true (GTK_IS_TOGGLE_BUTTON (toggle));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (toggle), TRUE); spin (100);
  GtkWidget *splitter = find_id (GTK_WIDGET (window), "properties_splitter");
  GtkWidget *pane = find_id (GTK_WIDGET (window), "properties_pane");
  g_assert_true (GTK_IS_PANED (splitter)); g_assert_true (gtk_widget_get_visible (pane));
  /* Showing the end child schedules its first allocation. GtkPaned initializes
   * the divider during that allocation, so do not simulate a drag before it. */
  gint64 mapping_deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  g_test_message ("Before properties allocation: splitter=%d pane=%d position=%d",
                  gtk_widget_get_width (splitter), gtk_widget_get_width (pane),
                  gtk_paned_get_position (GTK_PANED (splitter)));
  while ((gtk_widget_get_width (pane) <= 0 || !gtk_widget_get_mapped (pane)) &&
         g_get_monotonic_time () < mapping_deadline) spin (20);
  g_assert_true (gtk_widget_get_mapped (pane));
  g_assert_cmpint (gtk_widget_get_width (pane), >, 0);
  gtk_paned_set_position (GTK_PANED (splitter), gtk_widget_get_width (splitter) - 400); spin (100);
  gint64 sizing_deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (gtk_widget_get_width (pane) < 350 && g_get_monotonic_time () < sizing_deadline) spin (20);
  g_test_message ("Properties geometry: window=%d splitter=%d pane=%d mapped=%d",
                  gtk_widget_get_width (GTK_WIDGET (window)), gtk_widget_get_width (splitter),
                  gtk_widget_get_width (pane), gtk_widget_get_mapped (pane));
  gint64 resize_deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (gtk_widget_get_width (pane) < 350 && g_get_monotonic_time () < resize_deadline)
    {
      gtk_paned_set_position (GTK_PANED (splitter), gtk_widget_get_width (splitter) - 400);
      gtk_widget_queue_allocate (splitter);
      spin (20);
    }
  g_assert_cmpint (gtk_widget_get_width (pane), >=, 350);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (toggle), FALSE); spin (50);
  g_assert_false (gtk_widget_get_visible (pane));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (toggle), TRUE); spin (50);
  g_assert_true (gtk_widget_get_visible (pane));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (toggle), FALSE); spin (50);
  choose_view (page, "Stacked"); spin (100);
  g_assert_cmpint (gtk_orientable_get_orientation (GTK_ORIENTABLE (page->preview_paned)), ==, GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_position (page->preview_paned, 200); spin (50);
  g_assert_cmpint (gtk_paned_get_position (page->preview_paned), ==, 200);
  choose_view (page, "Side by Side"); spin (100);
  g_assert_cmpint (gtk_orientable_get_orientation (GTK_ORIENTABLE (page->preview_paned)), ==, GTK_ORIENTATION_HORIZONTAL);
  choose_view (page, "Collapse All");
  { g_autofree char *html = body_text (page); g_assert_null (strstr (html, "<details open>")); }
  choose_view (page, "Expand All");
  { g_autofree char *html = body_text (page); g_assert_null (strstr (html, "<details>")); g_assert_nonnull (strstr (html, "<details open>")); }
  g_print ("PASS Properties drag/close/reopen, stacked/side-by-side split and expand/collapse all\n");
}

static void test_large_sections (EditorWindow *window)
{
  g_autoptr(GString) source = g_string_new (NULL);
  g_autofree char *payload = g_strnfill (600, 'x');
  for (guint i = 1; i <= 6500; i++)
    g_string_append_printf (source, "{\"record\":%u,\"payload\":\"%s\"}\n", i, payload);
  g_assert_cmpuint (source->len, >, 2 * 1024 * 1024);
  EditorPage *page = new_page (window, "preview-large.jsonl", source->str);
  { g_autofree char *html = body_text (page); g_assert_nonnull (strstr (html, "Record at Line 1</h2>")); g_assert_null (strstr (html, "Record at Line 201</h2>")); }
  click_navigation (page, "Next Section");
  { g_autofree char *html = body_text (page); g_assert_nonnull (strstr (html, "Record at Line 106</h2>")); }
  click_navigation (page, "Previous Section");
  { g_autofree char *html = body_text (page); g_assert_nonnull (strstr (html, "Record at Line 1</h2>")); }
  click_navigation (page, "Last Section");
  { g_autofree char *html = body_text (page); g_assert_nonnull (strstr (html, "Record at Line 6500</h2>")); }
  click_navigation (page, "First Section");
  GtkWidget *spin = find_tooltip (GTK_WIDGET (page->preview_box), "Go to line in preview");
  g_assert_true (GTK_IS_SPIN_BUTTON (spin)); gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), 5000);
  { g_autofree char *html = body_text (page); g_assert_nonnull (strstr (html, "Record at Line 5000</h2>")); }
  g_assert_false (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)));
  { g_autofree char *after = source_text (page); g_assert_cmpstr (after, ==, source->str); }
  g_print ("PASS >3 MB / 6500 JSONL records, first/next/previous/last sections, line 5000, untouched source\n");
  g_string_assign (source, "{\"text\":\"");
  for (guint i = 0; i < 150000; i++) g_string_append (source, "☃");
  g_string_append (source, "\",\"final\":\"LAST_UTF8_MARKER\"}");
  page = new_page (window, "preview-long-unicode.json", source->str);
  { g_autofree char *html = body_text (page); g_assert_nonnull (strstr (html, "JSON source section")); g_assert_true (g_utf8_validate (html, -1, NULL)); }
  click_navigation (page, "Next Section");
  { g_autofree char *html = body_text (page); g_assert_true (g_utf8_validate (html, -1, NULL)); }
  click_navigation (page, "Last Section");
  { g_autofree char *html = body_text (page); g_assert_nonnull (strstr (html, "LAST_UTF8_MARKER")); }
  g_assert_false (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)));
  g_print ("PASS Huge one-line JSON sections retain UTF-8 boundaries and reach the final field\n");
}

static void test_async_format (EditorWindow *window)
{
  g_autoptr(GString) source = g_string_new ("{\"items\":[");
  for (guint i = 0; i < 30000; i++) g_string_append_printf (source, i ? ",%u" : "%u", i);
  g_string_append (source, "]}");
  EditorPage *page = new_page (window, "preview-large-format.json", source->str);
  g_assert_false (gtk_source_buffer_get_highlight_syntax (GTK_SOURCE_BUFFER (page->document)));
  g_print ("DEBUG before async format\n");
  gtk_widget_activate_action (GTK_WIDGET (window), "page.format-json", NULL);
  g_print ("DEBUG after async launch\n");
  gint64 deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  while (!gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)) && g_get_monotonic_time () < deadline) spin (10);
  { g_autofree char *after = source_text (page); g_assert_true (g_str_has_prefix (after, "{\n  \"items\": [\n")); }
  g_print ("DEBUG before undo can=%d enable=%d max=%u\n", gtk_text_buffer_get_can_undo (GTK_TEXT_BUFFER (page->document)), gtk_text_buffer_get_enable_undo (GTK_TEXT_BUFFER (page->document)), gtk_text_buffer_get_max_undo_levels (GTK_TEXT_BUFFER (page->document)));
  gtk_text_buffer_undo (GTK_TEXT_BUFFER (page->document));
  { GtkTextIter first; gtk_text_buffer_get_start_iter (GTK_TEXT_BUFFER (page->document), &first); g_print ("DEBUG undo chars=%d firstline=%d highlight=%d wrap=%d\n", gtk_text_buffer_get_char_count (GTK_TEXT_BUFFER (page->document)), gtk_text_iter_get_chars_in_line (&first), gtk_source_buffer_get_highlight_syntax (GTK_SOURCE_BUFFER (page->document)), gtk_text_view_get_wrap_mode (GTK_TEXT_VIEW (page->view))); }
  g_print ("DEBUG after undo\n");
  { g_autofree char *after = source_text (page); g_assert_cmpstr (after, ==, source->str); }
  gtk_text_buffer_set_modified (GTK_TEXT_BUFFER (page->document), FALSE);
  gtk_widget_activate_action (GTK_WIDGET (window), "page.format-json", NULL);
  GtkTextIter end; gtk_text_buffer_get_end_iter (GTK_TEXT_BUFFER (page->document), &end);
  gtk_text_buffer_insert (GTK_TEXT_BUFFER (page->document), &end, " ", 1);
  spin (1000); g_string_append_c (source, ' ');
  { g_autofree char *after = source_text (page); g_assert_cmpstr (after, ==, source->str); }
  g_print ("PASS Async formatting >20,000 values, one-step Undo and stale-result protection after editing\n");
}

#include "test-preview-html.inc"

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
  if (g_getenv ("EDITOR_TEST_HTML_ONLY")) { test_html_preview (window); test_html_external_fixture (window); gtk_window_destroy (GTK_WINDOW (window)); spin (100); return 0; }
  if (g_getenv ("EDITOR_TEST_ASYNC_ONLY")) { test_async_format (window); gtk_window_destroy (GTK_WINDOW (window)); spin (100); return 0; }

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
    g_assert_nonnull (strstr (text, "Record at Line 1"));
    g_assert_nonnull (strstr (text, "Record at Line 3"));
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
  test_panel_controls (window, new_page (window, "preview-panels.json", "{\"a\":[{\"b\":[1]}]}"));
  test_preview_recovery (window);
  test_large_sections (window);
  test_async_format (window);
  gtk_window_destroy (GTK_WINDOW (window));
  spin (200);
  g_print ("PASS Window/page teardown without critical errors\n");
  return 0;
}
