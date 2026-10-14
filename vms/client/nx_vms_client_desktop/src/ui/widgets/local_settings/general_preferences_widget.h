// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QScopedPointer>
#include <QtWidgets/QWidget>

#include <ui/widgets/common/abstract_preferences_widget.h>

namespace Ui {
class GeneralPreferencesWidget;
}

class QnGeneralPreferencesWidget : public QnAbstractPreferencesWidget
{
    Q_OBJECT
    using base_type = QnAbstractPreferencesWidget;

public:
    explicit QnGeneralPreferencesWidget(QWidget* parent = nullptr);
    virtual ~QnGeneralPreferencesWidget() override;

    virtual void applyChanges() override;
    virtual void loadDataToUi() override;
    virtual bool hasChanges() const override;

signals:
    void recordingSettingsChanged();

private:
    void at_addMediaFolderButton_clicked();
    void at_removeMediaFolderButton_clicked();
    void at_mediaFoldersList_selectionChanged();
    void at_browseStreamCacheFolderButton_clicked();
    void at_clearStreamCacheButton_clicked();

private:
    QStringList mediaFolders() const;
    void setMediaFolders(const QStringList& value);

    /** Cache folder shown in the UI; empty means the default one is used. */
    QString streamCacheFolder() const;
    void setStreamCacheFolder(const QString& value);

    qint64 streamCacheLimitBytes() const;
    void setStreamCacheLimitBytes(qint64 value);

    /** Shows how much of the cache limit is used up. */
    void updateStreamCacheUsage();

    quint64 userIdleTimeoutMs() const;
    void setUserIdleTimeoutMs(quint64 value);

    bool autoStart() const;
    void setAutoStart(bool value);

    bool autoLogin() const;
    void setAutoLogin(bool value);

    bool restoreUserSessionData() const;
    void setRestoreUserSessionData(bool value);

    bool allowComputerEnteringSleepMode() const;
    void setAllowComputerEnteringSleepMode(bool value);

    void initAudioDevices();

    QString primaryAudioDeviceName() const;
    void setPrimaryAudioDeviceName(const QString &name);

    QString secondaryAudioDeviceName() const;
    void setSecondaryAudioDeviceName(const QString& name);

    bool muteOnAudioTransmit() const;
    void setMuteOnAudioTransmit(bool value);

private:
    QScopedPointer<Ui::GeneralPreferencesWidget> ui;
};
