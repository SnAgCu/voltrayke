/*
    VolTrayke - Volume tray widget.
    Copyright (C) 2021  Andrea Zanellato <redtid3@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; version 2.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

    SPDX-License-Identifier: GPL-2.0-only
*/
#include "application.hpp"
#include "dialogabout.hpp"
#include "dialogprefs.hpp"
#include "menuvolume.hpp"
#include "qtilities.hpp"

#include "audio/device.hpp"
#if USE_ALSA
#include "audio/device/alsa.hpp"
#include "audio/engine/alsa.hpp"
#endif
#if USE_PULSEAUDIO
#include "audio/engine/pulseaudio.hpp"
#endif

#include <QAction>
#include <QCursor>
#include <QDir>
#include <QLibraryInfo>
#include <QMenu>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QTextStream>

#include <QDebug>

Qtilities::Application::Application(int& argc, char* argv[])
    : QApplication(argc, argv)
    , engine_(nullptr)
    , channel_(nullptr)
{
    setOrganizationName(ORGANIZATION_NAME);
    setOrganizationDomain(ORGANIZATION_DOMAIN);
    setApplicationName(APPLICATION_NAME);
    setApplicationDisplayName(APPLICATION_DISPLAY_NAME);

    setQuitOnLastWindowClosed(false);

    initLocale();
    initUi();

    connect(this, &QApplication::aboutToQuit, mnuVolume_, &QObject::deleteLater);
    connect(this, &QApplication::aboutToQuit, dlgAbout_, &QObject::deleteLater);
    connect(this, &QApplication::aboutToQuit, dlgPrefs_, &QObject::deleteLater);
    connect(this, &QApplication::aboutToQuit, trayIcon_, &QObject::deleteLater);
    connect(this, &QApplication::aboutToQuit, this, &Application::onAboutToQuit);

    connect(dlgPrefs_, &DialogPrefs::sigEngineChanged,
            this, &Application::onAudioEngineChanged);

    connect(dlgPrefs_, &DialogPrefs::sigChannelChanged,
            this, &Application::onAudioDeviceChanged);

    connect(dlgPrefs_, &DialogPrefs::sigPrefsChanged,
            this, [=] {
#if USE_ALSA
                engine_->setNormalized(settings_.isNormalized());
                AlsaEngine* alsa = qobject_cast<AlsaEngine*>(engine_);
                AlsaDevice* dev = qobject_cast<AlsaDevice*>(channel_);
                if (alsa && dev)
                    alsa->updateDevice(dev);
#endif
                mnuVolume_->setPageStep(settings_.pageStep());
                mnuVolume_->setSingleStep(settings_.singleStep());
            });

    connect(mnuVolume_, &MenuVolume::sigRunMixer, this, &Application::runMixer);
    connect(mnuVolume_, &MenuVolume::sigMuteToggled, this, [=](bool muted) {
        if (!channel_)
            return;

        channel_->setMute(muted);
        updateTrayIcon();
    });
    connect(mnuVolume_, &MenuVolume::sigVolumeChanged, this, [=](int volume) {
        if (!channel_)
            return;

        channel_->setVolume(volume);
        updateTrayIcon();
    });
    connect(trayIcon_, &QSystemTrayIcon::activated, this, &Application::onTrayIconActivated);
}

Qtilities::Application::~Application()
{
    qDebug() << "Destroyed VolTrayke";
}

void Qtilities::Application::initLocale()
{
#if 1
    QLocale locale = QLocale::system();
#else
    QLocale locale(QLocale("it"));
    QLocale::setDefault(locale);
#endif
    // install the translations built-into Qt itself
    if (qtTranslator.load(QStringLiteral("qt_") + locale.name(),
#if QT_VERSION < 0x060000
                      QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
#else
                      QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
#endif
        installTranslator(&qtTranslator);

    // E.g. "<appname>_en"
    QString translationsFileName = QCoreApplication::applicationName().toLower() + '_' + locale.name();
    // Try first in the same binary directory, in case we are building,
    // otherwise read from system data
    QString translationsPath = QCoreApplication::applicationDirPath();

    bool isLoaded = translator.load(translationsFileName, translationsPath);
    if (!isLoaded) {
        // "/usr/share/<appname>/translations
        isLoaded = translator.load(translationsFileName,
                                    QStringLiteral(PROJECT_DATA_DIR) + QStringLiteral("/translations"));
    }
    if (isLoaded)
        installTranslator(&translator);
}

void Qtilities::Application::initUi()
{
    actAutoStart_ = new QAction(tr("Auto&start"), this);
    trayIcon_ = new QSystemTrayIcon(QIcon::fromTheme("audio-volume-medium"), this);
    dlgAbout_ = new DialogAbout;
    dlgPrefs_ = new DialogPrefs;
    mnuVolume_ = new MenuVolume;

    settings_.load();
    dlgPrefs_->loadSettings();
#if 1
    onAudioEngineChanged(settings_.engineId());
    onAudioDeviceChanged(settings_.channelId());
    onAudioDeviceListChanged();
    updateTrayIcon();

    if (channel_)
        mnuVolume_->setVolume(channel_->volume());
#endif
    centerOnScreen(dlgPrefs_);
    centerOnScreen(dlgAbout_);

    actAutoStart_->setCheckable(true);
    actAutoStart_->setChecked(settings_.useAutostart());

    QAction* actAbout = new QAction(QIcon::fromTheme("help-about", QIcon(":/help-about")), tr("&About"), this);
    QAction* actPrefs = new QAction(QIcon::fromTheme("preferences-system", QIcon(":/preferences-system")), tr("&Preferences"), this);
    QAction* actQuit = new QAction(QIcon::fromTheme("application-exit", QIcon(":/application-exit")), tr("&Quit"), this);

    QMenu* mnuActions = new QMenu();
    mnuActions->addAction(actAutoStart_);
    mnuActions->addAction(actPrefs);
    mnuActions->addAction(actAbout);
    mnuActions->addAction(actQuit);

    trayIcon_->setContextMenu(mnuActions);
    trayIcon_->show();

    connect(actAbout, &QAction::triggered,
            this, [=] { if (dlgAbout_->isHidden()) dlgAbout_->show(); });

    connect(actPrefs, &QAction::triggered,
            this, [=] { if (dlgPrefs_->isHidden()) dlgPrefs_->show(); });

    connect(actQuit, &QAction::triggered, qApp, &QCoreApplication::quit);
}

void Qtilities::Application::onAudioEngineChanged(int engineId)
{
    if (engine_) {
        if (engine_->id() == engineId)
            return;

        if (channel_) {
            disconnect(channel_, nullptr, this, nullptr);
            channel_ = nullptr;
        }
        disconnect(engine_, &AudioEngine::sinkListChanged,
                   this, &Application::onAudioDeviceListChanged);
    }
    switch (engineId) {
#if USE_ALSA
    case EngineId::Alsa:
        engine_ = new AlsaEngine(this);
        break;
#endif
#if USE_PULSEAUDIO
    case EngineId::PulseAudio:
        engine_ = new PulseAudioEngine(this);
        break;
#endif
    default:
        engine_ = nullptr;
        return;
    }
#if 0
    engine_->setIgnoreMaxVolume(settings_.ignoreMaxVolume());
#endif
    engine_->setNormalized(settings_.isNormalized());

    connect(engine_, &AudioEngine::sinkListChanged,
            this, &Application::onAudioDeviceListChanged);
}

void Qtilities::Application::onAudioDeviceChanged(int deviceId)
{
    if (!engine_ || engine_->sinks().count() <= 0)
        return;

    if (deviceId < 0)
        deviceId = 0;

    channel_ = engine_->sinks().at(deviceId);

    connect(channel_, &AudioDevice::muteChanged, this, [=](bool muted) {
        mnuVolume_->setMute(muted);
        updateTrayIcon();
    });
    connect(channel_, &AudioDevice::volumeChanged, this, [=](int volume) {
        mnuVolume_->setVolume(volume);
        updateTrayIcon();
    });
}

void Qtilities::Application::onAudioDeviceListChanged()
{
    if (engine_) {
        QStringList list;
        for (const AudioDevice* dev : engine_->sinks())
            list.append(dev->description());

        dlgPrefs_->setDeviceList(list);
    }
}

void Qtilities::Application::onAboutToQuit()
{
    dlgPrefs_->saveSettings();

    settings_.setUseAutostart(actAutoStart_->isChecked());
    settings_.useAutostart() ? createAutostartFile()
                             : deleteAutostartFile();
    settings_.save();
}

void Qtilities::Application::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        mnuVolume_->show();
        mnuVolume_->adjustSize();
        mnuVolume_->popUp();
    } else if (channel_
               && settings_.muteOnMiddleClick()
               && reason == QSystemTrayIcon::MiddleClick) {
        channel_->toggleMute();
    }
}

void Qtilities::Application::runMixer()
{
    QString command = settings_.mixerCommand();
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    QStringList args = QProcess::splitCommand(command);
    QProcess::startDetached(args.takeFirst(), args);
#else
    QProcess::startDetached(command);
#endif
}

void Qtilities::Application::updateTrayIcon()
{
    QString iconName;
    if (channel_->volume() <= 0 || channel_->mute())
        iconName = QLatin1String("audio-volume-muted");
    else if (channel_->volume() <= 33)
        iconName = QLatin1String("audio-volume-low");
    else if (channel_->volume() <= 66)
        iconName = QLatin1String("audio-volume-medium");
    else
        iconName = QLatin1String("audio-volume-high");

    trayIcon_->setIcon(QIcon::fromTheme(iconName));
}

int main(int argc, char* argv[])
{
    // UseHighDpiPixmaps is default from Qt6
#if QT_VERSION < 0x060000
    QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#endif
    Qtilities::Application app(argc, argv);
    return app.exec();
}
