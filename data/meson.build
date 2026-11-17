if host_machine.system() not in ['windows', 'darwin']

desktop_file_in_config = configuration_data()
desktop_file_in_config.set('app_id', app_id)
editor_command = get_option('preview_build') ? 'gnome-text-editor-preview' : 'gnome-text-editor'
editor_exec = join_paths(get_option('prefix'), get_option('bindir'), editor_command)
desktop_file_in_config.set('editor_exec', editor_exec)
desktop_file_in_config.set('editor_name', get_option('preview_build') ? 'Text Editor Preview' : 'Text Editor')
desktop_file_in = configure_file(
          input: 'org.gnome.TextEditor.desktop.in.in',
         output: '@0@.desktop.in'.format(app_id),
  configuration: desktop_file_in_config,
)


desktop_file = i18n.merge_file(
        input: desktop_file_in,
       output: '@0@.desktop'.format(app_id),
         type: 'desktop',
       po_dir: '../po',
      install: true,
  install_dir: join_paths(get_option('datadir'), 'applications')
)

desktop_utils = find_program('desktop-file-validate', required: false)
if desktop_utils.found()
  test('Validate desktop file', desktop_utils,
    args: [desktop_file]
  )
endif

appconf = configuration_data()
appconf.set('app_id', app_id)
appstream_file_in = configure_file(
          input: 'org.gnome.TextEditor.metainfo.xml.in.in',
         output: '@0@.metainfo.xml.in'.format(app_id),
  configuration: appconf,
)
appstream_file = i18n.merge_file(
          input: appstream_file_in,
         output: '@0@.metainfo.xml'.format(app_id),
         po_dir: '../po',
        install: true,
    install_dir: join_paths(get_option('datadir'), 'metainfo')
)

appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('Validate appstream file', appstreamcli,
    args: ['validate', '--no-net', '--explain', appstream_file],
    workdir: meson.current_build_dir()
  )
endif

# D-Bus service file.
dbusconf = configuration_data()
dbusconf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))
dbusconf.set('app_id', app_id)
dbusconf.set('editor_command', editor_command)
configure_file(
          input: 'org.gnome.TextEditor.service.in',
         output: '@0@.service'.format(app_id),
  configuration: dbusconf,
   install_dir: join_paths(get_option('datadir'), 'dbus-1', 'services'),
)

endif

gnome.compile_schemas(build_by_default: true)
install_data('org.gnome.TextEditor.gschema.xml',
  install_dir: join_paths(get_option('datadir'), 'glib-2.0/schemas')
)

subdir('icons')
subdir('macos')

if get_option('preview_build')
  preview_launcher_conf = configuration_data()
  preview_launcher_conf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))
  preview_launcher_conf.set('datadir', join_paths(get_option('prefix'), get_option('datadir')))
  configure_file(input: 'gnome-text-editor-preview.in', output: 'gnome-text-editor-preview',
    configuration: preview_launcher_conf, install: true,
    install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
endif
