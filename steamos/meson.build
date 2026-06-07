project(
  'deck-screen-share-agent',
  'cpp',
  version: '0.2.0',
  default_options: ['cpp_std=c++17', 'warning_level=2'],
)

gio = dependency('gio-2.0')
json_glib = dependency('json-glib-1.0')

executable(
  'deck-screen-share-agent',
  'src/main.cpp',
  dependencies: [gio, json_glib],
  install: true,
)

install_data(
  'io.github.deckscreenshare.Agent.desktop',
  install_dir: get_option('datadir') / 'applications',
)

install_data(
  'io.github.deckscreenshare.Agent.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)

install_data(
  'io.github.deckscreenshare.Agent.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)
