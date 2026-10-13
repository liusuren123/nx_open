// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

import QtQuick 2.15
import QtQuick.Window 2.15

import Nx.Core 1.0
import Nx.Controls 1.0
import Nx.Dialogs 1.0

import nx.vms.client.core 1.0
import nx.vms.client.desktop 1.0

/**
 * Manages the client-side video sources (e.g. RTSP cameras) that are opened directly by the client,
 * without a media server. Configured sources appear in the Browser tab and can be watched there.
 */
Dialog
{
    id: control

    title: qsTr("Local Video Sources")

    width: 520
    height: 480
    minimumWidth: 520
    minimumHeight: 480

    modality: Qt.ApplicationModal

    x: (Screen.width - width) / 2
    y: (Screen.height - height) / 2

    function addSource()
    {
        if (!LocalCameraManager.addSource(nameField.text, urlField.text))
            return

        nameField.text = ""
        urlField.text = ""
    }

    contentItem: Column
    {
        spacing: 10

        Text
        {
            width: parent.width

            text: qsTr("The sources below are opened directly by the client, without a server, and"
                + " are available in the Browser tab.")
            color: ColorTheme.colors.light4
            font.pixelSize: FontConfig.normal.pixelSize
            wrapMode: Text.WordWrap
        }

        Flickable
        {
            id: sourceList

            width: parent.width
            height: 160
            clip: true
            contentWidth: width
            contentHeight: sourceColumn.height
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds

            Column
            {
                id: sourceColumn

                width: sourceList.width
                spacing: 2

                Repeater
                {
                    model: LocalCameraManager.sources

                    delegate: Item
                    {
                        width: sourceColumn.width
                        height: 30

                        Text
                        {
                            anchors.left: parent.left
                            anchors.right: removeButton.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter

                            // Only the sanitized name is shown, so credentials from the URL are
                            // never displayed.
                            text: modelData ? modelData.name : ""
                            color: ColorTheme.colors.light4
                            font.pixelSize: FontConfig.normal.pixelSize
                            elide: Text.ElideMiddle
                        }

                        Button
                        {
                            id: removeButton

                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter

                            text: qsTr("Remove")
                            onClicked: if (modelData) LocalCameraManager.removeSource(modelData.id)
                        }
                    }
                }
            }
        }

        Text
        {
            text: qsTr("Add a source")
            color: ColorTheme.colors.light4
            font.pixelSize: FontConfig.normal.pixelSize
        }

        TextField
        {
            id: nameField

            width: parent.width
            placeholderText: qsTr("Name (optional)")
        }

        TextField
        {
            id: urlField

            width: parent.width
            placeholderText: qsTr("rtsp://user:password@host:554/stream")
            onAccepted: control.addSource()
        }

        Text
        {
            width: parent.width

            text: qsTr("If the camera requires authentication, embed the credentials into the URL,"
                + " e.g. rtsp://user:password@host/stream.")
            color: ColorTheme.transparent(ColorTheme.colors.light4, 0.6)
            font.pixelSize: FontConfig.small.pixelSize
            wrapMode: Text.WordWrap
        }

        Button
        {
            isAccentButton: true
            text: qsTr("Add")
            enabled: urlField.text.length > 0
            onClicked: control.addSource()
        }
    }

    buttonBox: DialogButtonBox
    {
        buttonLayout: DialogButtonBox.KdeLayout
        standardButtons: DialogButtonBox.Close
    }
}
