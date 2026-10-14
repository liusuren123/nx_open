import QtQuick 2.15
import QtQuick.Window 2.15

import Nx.Core 1.0
import Nx.Controls 1.0
import Nx.Dialogs 1.0

import nx.vms.client.core 1.0
import nx.vms.client.desktop 1.0

/**
 * Imports M3U/M3U8 IPTV playlists (from a URL or a local file) and plays their channels in the
 * current layout. Channels are not added to the resource tree up front: a channel is opened on
 * demand, so a playlist with thousands of entries stays cheap.
 */
Dialog
{
    id: control

    title: qsTr("IPTV Playlists")

    width: 780
    height: 640
    minimumWidth: 700
    minimumHeight: 520

    modality: Qt.ApplicationModal

    x: (Screen.width - width) / 2
    y: (Screen.height - height) / 2

    property string selectedPlaylistId: ""
    property var channelItems: []

    function refreshChannels()
    {
        const all = control.selectedPlaylistId.length > 0
            ? IptvManager.channels(control.selectedPlaylistId) : []
        const filter = searchField.text.toLowerCase()
        control.channelItems = filter.length === 0
            ? all
            : all.filter(function(channel) {
                return (channel.name + " " + channel.group).toLowerCase().indexOf(filter) >= 0
            })
    }

    function importPlaylist()
    {
        const source = sourceField.text.trim()
        if (source.length === 0)
            return

        errorText.text = ""

        const isRemote = source.toLowerCase().indexOf("http://") === 0
            || source.toLowerCase().indexOf("https://") === 0
        // A remote playlist is downloaded in the background; the result arrives via importFinished.
        const started = isRemote
            ? IptvManager.importFromUrl(source, nameField.text)
            : IptvManager.importFromFile(source, nameField.text)

        if (!started)
            errorText.text = IptvManager.lastError()
    }

    contentItem: Column
    {
        spacing: 8

        Text
        {
            width: parent.width

            text: qsTr("Import an M3U playlist from an http(s) URL or a local file, then"
                + " double-click a channel to open it in the current layout.")
            color: ColorTheme.colors.light4
            font.pixelSize: FontConfig.normal.pixelSize
            wrapMode: Text.WordWrap
        }

        Row
        {
            width: parent.width
            spacing: 8

            TextField
            {
                id: sourceField

                width: parent.width - nameField.width - importButton.width - parent.spacing * 2
                placeholderText: qsTr("http://host/playlist.m3u  or  C:\\list.m3u")
                onAccepted: control.importPlaylist()
            }

            TextField
            {
                id: nameField

                width: 160
                placeholderText: qsTr("Name (optional)")
                onAccepted: control.importPlaylist()
            }

            Button
            {
                id: importButton

                isAccentButton: true
                text: qsTr("Import")
                enabled: sourceField.text.length > 0 && !IptvManager.importing
                onClicked: control.importPlaylist()
            }
        }

        Text
        {
            id: statusText

            width: parent.width
            visible: IptvManager.importing
            text: qsTr("Importing\u2026")
            color: ColorTheme.colors.light4
            font.pixelSize: FontConfig.small.pixelSize
        }

        Text
        {
            id: errorText

            width: parent.width
            color: ColorTheme.colors.red
            font.pixelSize: FontConfig.small.pixelSize
            wrapMode: Text.WordWrap
        }

        Text
        {
            text: qsTr("Playlists")
            color: ColorTheme.colors.light4
            font.pixelSize: FontConfig.normal.pixelSize
        }

        Flickable
        {
            id: playlistView

            width: parent.width
            height: 150
            clip: true
            contentWidth: width
            contentHeight: playlistColumn.height
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds

            Column
            {
                id: playlistColumn

                width: playlistView.width
                spacing: 2

                Repeater
                {
                    model: IptvManager.playlists

                    delegate: Item
                    {
                        width: playlistColumn.width
                        height: 32

                        Rectangle
                        {
                            anchors.fill: parent
                            color: modelData && modelData.id === control.selectedPlaylistId
                                ? ColorTheme.colors.dark7 : "transparent"

                            MouseArea
                            {
                                anchors.fill: parent
                                onClicked: control.selectPlaylist(modelData.id)
                            }
                        }

                        Text
                        {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.right: actions.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter

                            text: modelData
                                ? (modelData.name + "  (" + modelData.channelCount + ")")
                                : ""
                            color: ColorTheme.colors.light4
                            font.pixelSize: FontConfig.normal.pixelSize
                            elide: Text.ElideMiddle
                        }

                        Row
                        {
                            id: actions

                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6

                            Button
                            {
                                text: qsTr("Reload")
                                onClicked: if (modelData) IptvManager.reloadPlaylist(modelData.id)
                            }

                            Button
                            {
                                text: qsTr("Remove")
                                onClicked:
                                {
                                    if (!modelData)
                                        return
                                    if (control.selectedPlaylistId === modelData.id)
                                        control.selectedPlaylistId = ""
                                    IptvManager.removePlaylist(modelData.id)
                                }
                            }
                        }
                    }
                }
            }
        }

        TextField
        {
            id: searchField

            width: parent.width
            placeholderText: qsTr("Search channels")
            onTextChanged: control.refreshChannels()
        }

        ListView
        {
            id: channelView

            width: parent.width
            height: 200
            clip: true
            model: control.channelItems

            delegate: Item
            {
                width: channelView.width
                height: 30

                MouseArea
                {
                    anchors.fill: parent
                    onDoubleClicked: IptvManager.openChannel(control.selectedPlaylistId, modelData.id)
                }

                Text
                {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter

                    text: modelData
                        ? (modelData.group.length > 0
                            ? (modelData.group + "  \u00b7  " + modelData.name)
                            : modelData.name)
                        : ""
                    color: ColorTheme.colors.light4
                    font.pixelSize: FontConfig.normal.pixelSize
                    elide: Text.ElideRight
                }
            }
        }
    }

    buttonBox: DialogButtonBox
    {
        buttonLayout: DialogButtonBox.KdeLayout
        standardButtons: DialogButtonBox.Close
    }

    Connections
    {
        target: IptvManager

        function onImportFinished(ok)
        {
            if (ok)
            {
                errorText.text = ""
                sourceField.text = ""
                nameField.text = ""
            }
            else
            {
                errorText.text = IptvManager.lastError()
            }
        }
    }

    function selectPlaylist(id)
    {
        control.selectedPlaylistId = id
        control.refreshChannels()
    }
}
