// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/vms/client/core/resource/screen_recording/abstract_desktop_resource_searcher_impl.h>

namespace nx::vms::client::desktop {

/**
 * Enumerates local capture devices (USB / built-in cameras) and exposes them as LocalCameraResource
 * objects, so they are available in the client without a media server. Reuses the generic
 * DesktopResourceSearcher, which treats the returned resources as local/virtual ones.
 */
class LocalCameraResourceSearcherImpl: public QnAbstractDesktopResourceSearcherImpl
{
public:
    virtual QnResourceList findResources() override;
};

} // namespace nx::vms::client::desktop
