/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included by test-preview-page.c; all instrumentation exists only in tests. */
static char *html_report;
static guint unexpected_resources;

static void
html_message (WebKitUserContentManager *manager, JSCValue *value, gpointer data)
{
  g_free (html_report);
  html_report = jsc_value_to_string (value);
}

static void
html_resource (WebKitWebView *web, WebKitWebResource *resource,
               WebKitURIRequest *request, gpointer data)
{
  const char *uri = webkit_uri_request_get_uri (request);
  if (g_str_has_prefix (uri, "http:") || g_str_has_prefix (uri, "https:") ||
      g_str_has_prefix (uri, "file:"))
    { unexpected_resources++; g_print ("Unexpected resource: %s\n", uri); }
}

static void
install_html_probe (EditorPage *page)
{
  WebKitWebView *web = WEBKIT_WEB_VIEW (gtk_widget_get_first_child (GTK_WIDGET (page->preview_box)));
  WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager (web);
  WebKitUserScript *script = webkit_user_script_new (
    "if(window.top!==window){setTimeout(async()=>{const p={};"
    "p.visibility=document.visibilityState;await new Promise(r=>setTimeout(r,300));p.errors=window.__htmlErrors;p.title=document.title;p.ready=document.body.dataset.ready==='yes';"
    "p.tail=!!document.getElementById('tail');"
    "const b=document.getElementById('go');if(b)b.click();"
    "p.clicked=document.body.dataset.clicked==='yes';"
    "p.live=!!document.getElementById('live-update');"
    "try{void parent.document.body;p.parentBlocked=false}catch(e){p.parentBlocked=true}"
    "try{localStorage.setItem('preview','x');p.storageBlocked=false}catch(e){p.storageBlocked=true}"
    "try{await fetch('https://example.invalid/preview-must-stay-offline');p.networkBlocked=false}catch(e){p.networkBlocked=true}"
    "try{await fetch('file:///tmp/editor-preview-must-not-read');p.fileBlocked=false}catch(e){p.fileBlocked=true}"
    "p.three=typeof THREE!=='undefined';"
    "p.renderCalls=typeof renderer!=='undefined'&&renderer?renderer.info.render.calls:0;"
    "p.canvases=document.querySelectorAll('canvas').length;p.dimensions=[innerWidth,innerHeight];"
    "p.fallback=!!document.querySelector('.fallback');"
    "const c=document.getElementById('gl');if(c){const gl=c.getContext('webgl2')||c.getContext('webgl');p.webgl=!!gl;p.drawn=!!gl&&!!gl.getParameter(gl.CURRENT_PROGRAM)}"
    "const top=document.getElementById('vTop');if(top){top.click();for(let i=0;i<80&&top.getAttribute('aria-pressed')!=='true';i++)await new Promise(r=>setTimeout(r,100));p.cameraChanged=top.getAttribute('aria-pressed')==='true'}"
    "const play=document.getElementById('btnPlay');if(play){play.click();p.playing=play.getAttribute('aria-pressed')==='true'}"
    "p.text=document.body.innerText.slice(0,1500);"
    "window.webkit.messageHandlers.htmlTest.postMessage(JSON.stringify(p));"
    "},400)}",
    WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
  g_signal_connect (manager, "script-message-received::htmlTest", G_CALLBACK (html_message), NULL);
  g_assert_true (webkit_user_content_manager_register_script_message_handler (manager, "htmlTest", NULL));
  WebKitUserScript *errors = webkit_user_script_new (
    "window.__htmlErrors=[];addEventListener('error',e=>window.__htmlErrors.push(e.message));addEventListener('unhandledrejection',e=>window.__htmlErrors.push(String(e.reason)));",
    WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
  webkit_user_content_manager_add_script (manager, errors);
  webkit_user_script_unref (errors);
  webkit_user_content_manager_add_script (manager, script);
  webkit_user_script_unref (script);
  g_signal_connect (web, "resource-load-started", G_CALLBACK (html_resource), NULL);
}

static void
wait_html_report (void)
{
  gint64 deadline = g_get_monotonic_time () + 12 * G_USEC_PER_SEC;
  while (!html_report && g_get_monotonic_time () < deadline) spin (30);
  g_assert_nonnull (html_report);
  g_print ("HTML runtime: %s\n", html_report);
}

static void
assert_html_isolated (void)
{
  g_assert_nonnull (strstr (html_report, "\"parentBlocked\":true"));
  g_assert_nonnull (strstr (html_report, "\"storageBlocked\":true"));
  g_assert_nonnull (strstr (html_report, "\"networkBlocked\":true"));
  g_assert_nonnull (strstr (html_report, "\"fileBlocked\":true"));
  g_assert_cmpuint (unexpected_resources, ==, 0);
}

static void
test_html_preview (EditorWindow *window)
{
  g_autoptr(GString) source = g_string_new (
    "<!doctype html><html><head><title>Interactive HTML</title>"
    "<style>body{background:rgb(10,20,30);color:white}</style></head><body>"
    "<h1>HTML preview</h1><button id='go' onclick=\"document.body.dataset.clicked='yes'\">Run</button>"
    "<canvas width='80' height='50'></canvas>"
    "<script>document.body.dataset.ready='yes';"
    "document.querySelector('canvas').getContext('2d').fillRect(0,0,40,30)</script>"
    "<img src='https://example.invalid/blocked-image.png'>");
  EditorPage *page;
  WebKitWebView *web;
  GtkWidget *line;
  for (guint i = 0; i < 2000; i++) g_string_append (source, "<!-- full-document content and line count -->\n");
  g_string_append (source, "<p id='tail'>Entire document loaded</p></body></html>");
  page = new_page (window, "preview-interactive", source->str);
  gtk_source_buffer_set_language (GTK_SOURCE_BUFFER (page->document),
      gtk_source_language_manager_get_language (gtk_source_language_manager_get_default (), "html"));
  spin (350);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (page->preview_toolbar)));
  web = WEBKIT_WEB_VIEW (gtk_widget_get_first_child (GTK_WIDGET (page->preview_box)));
  install_html_probe (page);
  g_clear_pointer (&html_report, g_free);
  unexpected_resources = 0;
  _editor_page_preview_set_mode (page, "split");
  _editor_page_raise (page);
  gtk_window_present (GTK_WINDOW (window));
  spin (150);
  wait_html_report ();
  g_assert_nonnull (strstr (html_report, "\"ready\":true"));
  g_assert_nonnull (strstr (html_report, "\"clicked\":true"));
  g_assert_nonnull (strstr (html_report, "\"tail\":true"));
  assert_html_isolated ();
  g_assert_true (webkit_settings_get_enable_javascript (webkit_web_view_get_settings (web)));
  line = find_tooltip (GTK_WIDGET (page->preview_box), "Go to line in preview");
  g_assert_false (gtk_widget_is_visible (line));
  { g_autofree char *after = source_text (page); g_assert_cmpstr (after, ==, source->str); }
  g_assert_false (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)));
  g_clear_pointer (&html_report, g_free);
  g_string_prepend (source, "<div id='live-update'>Live preview</div>");
  gtk_text_buffer_set_text (GTK_TEXT_BUFFER (page->document), source->str, -1);
  wait_html_report ();
  g_assert_nonnull (strstr (html_report, "\"live\":true"));
  _editor_page_preview_set_mode (page, "source");
  spin (250);
  g_assert_false (webkit_settings_get_enable_javascript (webkit_web_view_get_settings (web)));
  { g_autofree char *html = body_text (page); g_assert_null (strstr (html, "HTML document preview")); }
  g_clear_pointer (&html_report, g_free);
  _editor_page_preview_set_mode (page, "preview");
  wait_html_report ();
  g_assert_false (gtk_widget_get_visible (page->source_box));
  assert_html_isolated ();
  /* Changing the same document back to JSON restores its normal controls and
   * disables HTML execution, not merely the visibility of its toolbar. */
  gtk_source_buffer_set_language (GTK_SOURCE_BUFFER (page->document),
      gtk_source_language_manager_get_language (gtk_source_language_manager_get_default (), "json"));
  gtk_text_buffer_set_text (GTK_TEXT_BUFFER (page->document), "{\"afterHtml\":true}", -1);
  spin (700);
  g_assert_false (webkit_settings_get_enable_javascript (webkit_web_view_get_settings (web)));
  g_assert_true (gtk_widget_is_visible (line));
  { g_autofree char *html = body_text (page); g_assert_nonnull (strstr (html, "afterHtml")); }
  g_print ("PASS HTML: full 90 KB/2000-line document, scripts/canvas/button, live updates, opaque origin, blocked network/files/storage, Source/Preview/Split, JSON switch, source untouched\n");
  g_clear_pointer (&html_report, g_free);
}

static void
test_html_external_fixture (EditorWindow *window)
{
  const char *path = g_getenv ("EDITOR_TEST_HTML_FILE");
  g_autofree char *text = NULL;
  g_autofree char *after = NULL;
  g_autoptr(GError) error = NULL;
  EditorPage *page;
  if (!path) return;
  g_assert_true (g_file_get_contents (path, &text, NULL, &error));
  g_assert_no_error (error);
  page = new_page (window, "preview-offline-orrery.html", text);
  install_html_probe (page);
  g_clear_pointer (&html_report, g_free);
  unexpected_resources = 0;
  _editor_page_preview_set_mode (page, "split");
  _editor_page_raise (page);
  gtk_window_present (GTK_WINDOW (window));
  spin (150);
  wait_html_report ();
  g_assert_nonnull (strstr (html_report, "Earth–Moon Orrery"));
  g_assert_nonnull (strstr (html_report, "\"three\":true"));
  g_assert_nonnull (strstr (html_report, "\"canvases\":1"));
  g_assert_nonnull (strstr (html_report, "\"fallback\":false"));
  g_assert_nonnull (strstr (html_report, "\"webgl\":true"));
  g_assert_nonnull (strstr (html_report, "\"drawn\":true"));
  g_assert_nonnull (strstr (html_report, "\"cameraChanged\":true"));
  g_assert_nonnull (strstr (html_report, "\"playing\":true"));
  g_assert_nonnull (strstr (html_report, "\"visibility\":\"visible\""));
  g_assert_nonnull (strstr (html_report, "\"errors\":[]"));
  assert_html_isolated ();
  after = source_text (page);
  g_assert_cmpstr (after, ==, text);
  g_assert_false (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (page->document)));
  g_print ("PASS actual Earth–Moon Orrery: bundled Three.js, canvas scene, no network/file requests, exact original source\n");
  g_clear_pointer (&html_report, g_free);
}
