project('tb-tray', 'c',
  version: '1.0.0',
  meson_version: '>= 0.50.0',
  default_options: ['warning_level=2', 'c_std=c11']
)

gtk3 = dependency('gtk+-3.0')
gio_unix = dependency('gio-unix-2.0')

i18n = import('i18n')
localedir = get_option('prefix') / get_option('localedir')

asus_led = get_option('asus-led')
tb_cargs = ['-DTB_CMD_DEFAULT="/opt/thunderbird/thunderbird"']
if asus_led
  tb_cargs += ['-DASUS_LED', '-DASUS_LED_URL_DEFAULT="http://192.168.137.115:8077/mail"']
endif

executable('tb-tray',
  files('tray/tb-tray.c'),
  dependencies: [gtk3, gio_unix],
  c_args: tb_cargs + [
    '-DVERSION="' + meson.project_version() + '"',
    '-DGETTEXT_PACKAGE="tb-tray"',
    '-DLOCALEDIR="' + localedir + '"',
  ],
  install: true
)

subdir('po')

# MailExtension: устанавливается в профиль Thunderbird из pkg_postinst (USE=extension)
install_subdir('extension',
  install_dir: get_option('prefix') / get_option('datadir') / 'tb-tray'
)

install_data('data/tb-tray.desktop',
  install_dir: get_option('prefix') / get_option('datadir') / 'applications'
)
