/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <functional>

#include <QApplication>
#include <QWidget>
#include <QVariant>
#include <QString>
#include <QAction>
#include <QShortcut>
#include <QKeySequence>
#include <QSettings>

#include "globalshortcutsmanager.h"
#include "globalshortcutsbackend.h"

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && defined(HAVE_DBUS)
#  include "globalshortcutsbackend-kde.h"
#  include "globalshortcutsbackend-gnome.h"
#  include "globalshortcutsbackend-mate.h"
#endif

#ifdef HAVE_X11_GLOBALSHORTCUTS
#  include "globalshortcutsbackend-x11.h"
#endif

#ifdef Q_OS_WIN
#  include "globalshortcutsbackend-win.h"
#endif

#ifdef Q_OS_MACOS
#  include "globalshortcutsbackend-macos.h"
#endif

#include "settings/globalshortcutssettingspage.h"

GlobalShortcutsManager::GlobalShortcutsManager(QWidget *parent) : QWidget(parent) {

  settings_.beginGroup(GlobalShortcutsSettingsPage::kSettingsGroup);

  // Create actions
  AddShortcut("play", "Play", std::bind(&GlobalShortcutsManager::Play, this));
  AddShortcut("pause", "Pause", std::bind(&GlobalShortcutsManager::Pause, this));
  AddShortcut("play_pause", "Play/Pause", std::bind(&GlobalShortcutsManager::PlayPause, this), QKeySequence(Qt::Key_MediaPlay));
  AddShortcut("stop", "Stop", std::bind(&GlobalShortcutsManager::Stop, this), QKeySequence(Qt::Key_MediaStop));
  AddShortcut("stop_after", "Stop playing after current track", std::bind(&GlobalShortcutsManager::StopAfter, this));
  AddShortcut("next_track", "Next track", std::bind(&GlobalShortcutsManager::Next, this), QKeySequence(Qt::Key_MediaNext));
  AddShortcut("prev_track", "Previous track", std::bind(&GlobalShortcutsManager::Previous, this), QKeySequence(Qt::Key_MediaPrevious));
  AddShortcut("inc_volume", "Increase volume", std::bind(&GlobalShortcutsManager::IncVolume, this));
  AddShortcut("dec_volume", "Decrease volume", std::bind(&GlobalShortcutsManager::DecVolume, this));
  AddShortcut("mute", tr("Mute"), std::bind(&GlobalShortcutsManager::Mute, this));
  AddShortcut("seek_forward", "Seek forward", std::bind(&GlobalShortcutsManager::SeekForward, this));
  AddShortcut("seek_backward", "Seek backward", std::bind(&GlobalShortcutsManager::SeekBackward, this));
  AddShortcut("show_hide", "Show/Hide", std::bind(&GlobalShortcutsManager::ShowHide, this));
  AddShortcut("show_osd", "Show OSD", std::bind(&GlobalShortcutsManager::ShowOSD, this));
  AddShortcut("toggle_pretty_osd", "Toggle Pretty OSD", std::bind(&GlobalShortcutsManager::TogglePrettyOSD, this));  // Toggling possible only for pretty OSD
  AddShortcut("shuffle_mode", "Change shuffle mode", std::bind(&GlobalShortcutsManager::CycleShuffleMode, this));
  AddShortcut("repeat_mode", "Change repeat mode", std::bind(&GlobalShortcutsManager::CycleRepeatMode, this));
  AddShortcut("toggle_scrobbling", "Enable/disable scrobbling", std::bind(&GlobalShortcutsManager::ToggleScrobbling, this));
  AddShortcut("love", "Love", std::bind(&GlobalShortcutsManager::Love, this));

  // Create backends - these do the actual shortcut registration

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && defined(HAVE_DBUS)
  backends_ << new GlobalShortcutsBackendKDE(this, this);
  backends_ << new GlobalShortcutsBackendGnome(this, this);
  backends_ << new GlobalShortcutsBackendMate(this, this);
#endif

#ifdef Q_OS_MACOS
  backends_ << new GlobalShortcutsBackendMacOS(this, this);
#endif

#ifdef Q_OS_WIN
  backends_ << new GlobalShortcutsBackendWin(this, this);
#endif

#ifdef HAVE_X11_GLOBALSHORTCUTS
  backends_ << new GlobalShortcutsBackendX11(this, this);
#endif

  ReloadSettings();

}

void GlobalShortcutsManager::ReloadSettings() {

  backends_enabled_.clear();

#ifdef Q_OS_MACOS
  backends_enabled_ << GlobalShortcutsBackend::Type_MacOS;
#endif

#ifdef Q_OS_WIN
  backends_enabled_ << GlobalShortcutsBackend::Type_Win;
#endif

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && defined(HAVE_DBUS)
  if (settings_.value("use_kde", true).toBool()) {
    backends_enabled_ << GlobalShortcutsBackend::Type_KDE;
  }
  if (settings_.value("use_gnome", true).toBool()) {
    backends_enabled_ << GlobalShortcutsBackend::Type_Gnome;
  }
  if (settings_.value("use_mate", true).toBool()) {
    backends_enabled_ << GlobalShortcutsBackend::Type_Mate;
  }
#endif

#ifdef HAVE_X11_GLOBALSHORTCUTS
  if (settings_.value("use_x11", false).toBool()) {
    backends_enabled_ << GlobalShortcutsBackend::Type_X11;
  }
#endif

  Unregister();
  Register();

}

void GlobalShortcutsManager::AddShortcut(const QString &id, const QString &name, std::function<void()> signal, const QKeySequence &default_key) {  // clazy:exclude=function-args-by-ref

  Shortcut shortcut = AddShortcut(id, name, default_key);
  QObject::connect(shortcut.action, &QAction::triggered, this, signal);

}

GlobalShortcutsManager::Shortcut GlobalShortcutsManager::AddShortcut(const QString &id, const QString &name, const QKeySequence &default_key) {

  Shortcut shortcut;
  shortcut.action = new QAction(name, this);
  QKeySequence key_sequence = QKeySequence::fromString(settings_.value(id, default_key.toString()).toString());
  shortcut.action->setShortcut(key_sequence);
  shortcut.id = id;
  shortcut.default_key = default_key;

  // Create application wide QShortcut to hide keyevents mapped to global shortcuts from widgets.
  shortcut.shortcut = new QShortcut(key_sequence, this);
  shortcut.shortcut->setContext(Qt::ApplicationShortcut);

  shortcuts_[id] = shortcut;

  return shortcut;

}

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && defined(HAVE_DBUS)

bool GlobalShortcutsManager::IsKdeAvailable() {

  return GlobalShortcutsBackendKDE::IsKDEAvailable();

}

bool GlobalShortcutsManager::IsGnomeAvailable() {

  return GlobalShortcutsBackendGnome::IsGnomeAvailable();

}

bool GlobalShortcutsManager::IsMateAvailable() {

  return GlobalShortcutsBackendMate::IsMateAvailable();

}

#  endif  // defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && defined(HAVE_DBUS)

#ifdef HAVE_X11_GLOBALSHORTCUTS

bool GlobalShortcutsManager::IsX11Available() {

  return GlobalShortcutsBackendX11::IsX11Available();

}

#endif  // HAVE_X11_GLOBALSHORTCUTS

bool GlobalShortcutsManager::Register() {

  for (GlobalShortcutsBackend *backend : backends_) {
    if (backend->IsAvailable() && backends_enabled_.contains(backend->type())) {
      return backend->Register();
    }
  }

  qLog(Warning) << "No global shortcuts enabled.";

  return false;

}

void GlobalShortcutsManager::Unregister() {

  for (GlobalShortcutsBackend *backend : backends_) {
    if (backend->is_active()) {
      backend->Unregister();
    }
  }

}

#ifdef Q_OS_MACOS

bool GlobalShortcutsManager::IsMacAccessibilityEnabled() {

  return GlobalShortcutsBackendMacOS::IsAccessibilityEnabled();

}

#endif  // Q_OS_MACOS

void GlobalShortcutsManager::ShowMacAccessibilityDialog() {

#ifdef Q_OS_MACOS
  GlobalShortcutsBackendMacOS::ShowAccessibilityDialog();
#endif

}
