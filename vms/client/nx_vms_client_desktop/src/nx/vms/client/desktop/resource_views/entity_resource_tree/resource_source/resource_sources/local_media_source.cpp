// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_media_source.h"

#include <core/resource_management/resource_pool.h>
#include <core/resource/avi/avi_resource.h>

namespace nx::vms::client::desktop {
namespace entity_resource_tree {

namespace {

/**
 * Client-side live cameras (USB / built-in) are local, self-contained media sources, so they are
 * listed along with local media files, as opposed to the desktop camera which is exposed separately.
 */
bool isLocalCamera(const QnResourcePtr& resource)
{
    return resource->hasFlags(Qn::local_live_cam)
        && !resource->hasFlags(Qn::desktop_camera);
}

} // namespace

LocalMediaSource::LocalMediaSource(const QnResourcePool* resourcePool):
    base_type(),
    m_resourcePool(resourcePool)
{
    if (!NX_ASSERT(m_resourcePool))
        return;

    connect(resourcePool, &QnResourcePool::resourceAdded, this,
        [this](const QnResourcePtr& resource)
        {
            if (auto aviResource = resource.dynamicCast<QnAviResource>())
            {
                if (aviResource->isEmbedded())
                    return;

                connect(aviResource.get(), &QnAviResource::statusChanged, this,
                    [this](const QnResourcePtr& resource)
                    {
                        if (resource->isOnline())
                            emit resourceAdded(resource);
                        else
                            emit resourceRemoved(resource);
                    });

                if (aviResource->isOnline())
                    emit resourceAdded(resource);
            }
            else if (isLocalCamera(resource))
            {
                emit resourceAdded(resource);
            }
        });

    connect(resourcePool, &QnResourcePool::resourceRemoved, this,
        [this](const QnResourcePtr& resource)
        {
            if (auto aviResource = resource.dynamicCast<QnAviResource>())
            {
                aviResource->disconnect(this);
                emit resourceRemoved(resource);
            }
            else if (isLocalCamera(resource))
            {
                emit resourceRemoved(resource);
            }
        });
}

QVector<QnResourcePtr> LocalMediaSource::getResources()
{
    const auto aviResources = m_resourcePool->getResources<QnAviResource>().filtered(
        [](const QnAviResourcePtr& aviResource)
        {
            return !aviResource->isEmbedded() && aviResource->isOnline();
        });

    QVector<QnResourcePtr> result;
    std::copy(std::cbegin(aviResources), std::cend(aviResources), std::back_inserter(result));

    for (const auto& resource: m_resourcePool->getResourcesWithFlag(Qn::local_live_cam))
    {
        if (isLocalCamera(resource))
            result.push_back(resource);
    }

    return result;
}

} // namespace entity_resource_tree
} // namespace nx::vms::client::desktop
