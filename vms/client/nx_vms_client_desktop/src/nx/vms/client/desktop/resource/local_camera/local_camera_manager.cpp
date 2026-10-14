// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_camera_manager.h"

#include <QtCore/QSet>
#include <QtCore/QVariantMap>

#include <core/resource_management/resource_pool.h>
#include <nx/utils/log/log.h>
#include <nx/utils/property_storage/property.h>
#include <nx/vms/client/desktop/application_context.h>
#include <nx/vms/client/desktop/settings/local_settings.h>
#include <nx/vms/client/desktop/system_context.h>

#include "local_camera_resource.h"
#include "local_camera_source.h"

namespace nx::vms::client::desktop {

LocalCameraManager::LocalCameraManager(LocalSettings* settings, QObject* parent):
    QObject(parent),
    m_settings(settings)
{
    if (!m_settings)
        return;

    connect(&m_settings->localCameraSources,
        &nx::utils::property_storage::BaseProperty::changed,
        this,
        [this]()
        {
            syncPool();
            emit sourcesChanged();
        });

    // Restore the persisted sources into the resource pool.
    syncPool();
}

LocalCameraManager::~LocalCameraManager() = default;

QVariantList LocalCameraManager::sources() const
{
    QVariantList result;
    if (!m_settings)
        return result;

    for (const LocalCameraSource& source: m_settings->localCameraSources())
    {
        // The password is deliberately not exposed, and the URL is stripped of any embedded
        // credentials, so nothing sensitive can leak into the dialog.
        result.append(QVariantMap{
            {"id", source.id.toString()},
            {"name", source.name}, //< Raw name, may be empty; the UI falls back to the URL.
            {"url", localCameraSourceUrlForDisplay(source.url)},
            {"username", source.username}});
    }

    return result;
}

bool LocalCameraManager::addSource(const QString& name, const QString& url,
    const QString& username, const QString& password)
{
    if (!m_settings)
        return false;

    const QString trimmedUrl = url.trimmed();
    if (trimmedUrl.isEmpty())
        return false;

    auto list = m_settings->localCameraSources();
    for (const LocalCameraSource& source: list)
    {
        if (source.url.compare(trimmedUrl, Qt::CaseInsensitive) == 0)
            return false; //< Already configured.
    }

    LocalCameraSource source;
    source.id = nx::Uuid::createUuid();
    source.name = name.trimmed(); //< May be empty: the name is then derived from the URL.
    source.url = trimmedUrl;
    source.username = username.trimmed();
    source.password = password;
    classifyLocalCameraSource(source);
    list.append(source);

    m_settings->localCameraSources = list; //< Emits changed => syncPool().
    return true;
}

bool LocalCameraManager::updateSource(const QString& id, const QString& name, const QString& url,
    const QString& username, const QString& password)
{
    if (!m_settings)
        return false;

    const nx::Uuid uuid = id.isEmpty() ? nx::Uuid() : nx::Uuid::fromStringSafe(id);
    if (uuid.isNull())
        return false;

    const QString trimmedUrl = url.trimmed();
    if (trimmedUrl.isEmpty())
        return false;

    auto list = m_settings->localCameraSources();

    int index = -1;
    for (int i = 0; i < list.size(); ++i)
    {
        if (list[i].id == uuid)
        {
            index = i;
            continue;
        }

        if (list[i].url.compare(trimmedUrl, Qt::CaseInsensitive) == 0)
            return false; //< Another source already uses this URL.
    }

    if (index < 0)
        return false;

    LocalCameraSource& source = list[index];
    source.name = name.trimmed();
    source.url = trimmedUrl;
    source.username = username.trimmed();
    if (!password.isEmpty())
        source.password = password; //< An empty password keeps the stored one.
    classifyLocalCameraSource(source);

    m_settings->localCameraSources = list; //< Emits changed => syncPool().
    return true;
}

bool LocalCameraManager::removeSource(const QString& id)
{
    if (!m_settings)
        return false;

    const nx::Uuid uuid = id.isEmpty() ? nx::Uuid() : nx::Uuid::fromStringSafe(id);
    if (uuid.isNull())
        return false;

    auto list = m_settings->localCameraSources();
    for (int i = 0; i < list.size(); ++i)
    {
        if (list[i].id != uuid)
            continue;

        list.removeAt(i);
        m_settings->localCameraSources = list; //< Emits changed => syncPool().
        return true;
    }

    return false;
}

void LocalCameraManager::syncPool()
{
    if (!m_settings)
        return;

    auto systemContext = appContext()->currentSystemContext();
    if (!systemContext)
        return;

    auto pool = systemContext->resourcePool();
    if (!pool)
        return;

    const auto configured = m_settings->localCameraSources();

    QSet<nx::Uuid> configuredIds;
    for (const LocalCameraSource& source: configured)
        configuredIds.insert(source.id);

    // Add resources for the configured sources that are not in the pool yet, and refresh the ones
    // that changed (e.g. after an edit), so the tree and the streams pick up the new values.
    for (const LocalCameraSource& source: configured)
    {
        if (const auto existing = pool->getResourceById(source.id))
        {
            if (const auto urlResource = existing.dynamicCast<LocalCameraResource>())
                urlResource->updateFromSource(source);

            continue;
        }

        pool->addResources(QnResourceList{QnResourcePtr(new LocalCameraResource(source))});
    }

    // Remove resources of sources that are no longer configured.
    for (const auto& resource: pool->getResources<LocalCameraResource>())
    {
        if (!resource->isUrlSource() || configuredIds.contains(resource->sourceId()))
            continue;

        pool->removeResource(resource);
    }
}

} // namespace nx::vms::client::desktop
