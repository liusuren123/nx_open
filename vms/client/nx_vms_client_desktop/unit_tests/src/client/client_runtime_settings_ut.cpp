// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <gtest/gtest.h>

#include <nx/utils/uuid.h>

#include <client/client_runtime_settings.h>
#include <client/client_startup_parameters.h>

namespace {

TEST(LocalModeTest, localModeIsTheDefault)
{
    const QnStartupParameters parameters;
    EXPECT_TRUE(parameters.localMode);
    EXPECT_FALSE(parameters.requiresServerConnection());
}

TEST(LocalModeTest, connectionRequestsRequireServerConnection)
{
    {
        QnStartupParameters parameters;
        parameters.acsMode = true;
        EXPECT_TRUE(parameters.requiresServerConnection());
    }
    {
        QnStartupParameters parameters;
        parameters.authenticationString = "some-token";
        EXPECT_TRUE(parameters.requiresServerConnection());
    }
    {
        QnStartupParameters parameters;
        parameters.videoWallGuid = nx::Uuid::createUuid();
        EXPECT_TRUE(parameters.requiresServerConnection());
    }
}

TEST(LocalModeTest, videoWallModeRequiresServerConnection)
{
    QnStartupParameters parameters;
    parameters.videoWallGuid = nx::Uuid::createUuid();
    EXPECT_TRUE(parameters.isVideoWallMode());
    EXPECT_TRUE(parameters.requiresServerConnection());
}

TEST(LocalModeTest, runtimeSettingsExposeLocalMode)
{
    QnStartupParameters parameters;
    parameters.localMode = true;

    const QnClientRuntimeSettings settings(parameters);
    EXPECT_TRUE(settings.isLocalMode());
    EXPECT_TRUE(settings.isDesktopMode());
}

TEST(LocalModeTest, runtimeSettingsExposeDisabledLocalMode)
{
    QnStartupParameters parameters;
    parameters.localMode = false;

    const QnClientRuntimeSettings settings(parameters);
    EXPECT_FALSE(settings.isLocalMode());
}

} // namespace
