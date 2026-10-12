// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_camera_resource_searcher.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

#include "local_camera_resource.h"

namespace nx::vms::client::desktop {

namespace {

/** The dshow input expects the device as "video=<name>". */
QString bareDeviceName(const QString& deviceName)
{
    static const QString prefix = "video=";
    return deviceName.startsWith(prefix) ? deviceName.mid(prefix.size()) : deviceName;
}

} // namespace

QnResourceList LocalCameraResourceSearcherImpl::findResources()
{
    QnResourceList result;

    // Enumerated through FFmpeg (the same backend the capture provider uses), so the reported
    // names match the ones used to open the device and the code stays off the GUI thread.
    avdevice_register_all();

    const AVInputFormat* inputFormat = av_find_input_format("dshow");
    if (!inputFormat)
        return result;

    AVDeviceInfoList* deviceList = nullptr;
    if (avdevice_list_input_sources(inputFormat, nullptr, nullptr, &deviceList) < 0 || !deviceList)
        return result;

    for (int i = 0; i < deviceList->nb_devices; ++i)
    {
        const AVDeviceInfo* device = deviceList->devices[i];
        if (!device)
            continue;

        if (device->nb_media_types > 0 && device->media_types[0] != AVMEDIA_TYPE_VIDEO)
            continue;

        const QString deviceId = bareDeviceName(QString::fromUtf8(device->device_name));
        if (deviceId.isEmpty())
            continue;

        const QString name = QString::fromUtf8(device->device_description);
        result.push_back(QnResourcePtr(new LocalCameraResource(deviceId, name)));
    }

    avdevice_free_list_devices(&deviceList);
    return result;
}

} // namespace nx::vms::client::desktop
