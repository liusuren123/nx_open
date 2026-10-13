// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QVariantList>

namespace nx::vms::client::desktop {

class LocalSettings;

/**
 * Owns the client-side video sources configured by the user (e.g. RTSP cameras) and keeps the
 * resource pool in sync with them.
 *
 * The list itself is stored in LocalSettings, so every change is persisted automatically. Resources
 * are added to and removed from the resource pool as sources are configured, which is what makes
 * them appear in the resource tree and become watchable without a media server.
 */
class NX_VMS_CLIENT_DESKTOP_API LocalCameraManager: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)

public:
    explicit LocalCameraManager(LocalSettings* settings, QObject* parent = nullptr);
    virtual ~LocalCameraManager() override;

    /** Configured sources as a list of { id, name, url } maps. */
    QVariantList sources() const;

    /**
     * Adds a source. An empty URL is rejected, and a URL that is already configured is ignored.
     * The credentials are optional; when given, they are stored separately and embedded into the
     * URL only when the stream is opened. Returns whether the source list was changed.
     */
    Q_INVOKABLE bool addSource(const QString& name, const QString& url,
        const QString& username, const QString& password);

    /** Removes the source with the given id. Returns whether it was found. */
    Q_INVOKABLE bool removeSource(const QString& id);

signals:
    void sourcesChanged();

private:
    void syncPool();

    LocalSettings* const m_settings;
};

} // namespace nx::vms::client::desktop
