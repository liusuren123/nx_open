// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Nx.Core 1.0

import nx.vms.client.desktop 1.0

/**
 * Local player sidebar page: a playlist selector, the list of media entries of the current
 * playlist and a toolbar with the playlist operations.
 */
Item
{
    id: playlistBrowser

    clip: true

    PlaylistListModel
    {
        id: playlistsModel
    }

    PlaylistModel
    {
        id: entriesModel

        playlistId: PlaylistManager.currentPlaylistId
    }

    function playEntry(row)
    {
        if (typeof playlistPlayer === "undefined" || !playlistPlayer)
            return

        playlistPlayer.playEntry(PlaylistManager.currentPlaylistId, row)
    }

    function playPrevious()
    {
        if (typeof playlistPlayer === "undefined" || !playlistPlayer)
            return

        playlistPlayer.playPrevious()
    }

    function playNext()
    {
        if (typeof playlistPlayer === "undefined" || !playlistPlayer)
            return

        playlistPlayer.playNext()
    }

    Connections
    {
        target: PlaylistManager

        function onCurrentPlaylistChanged()
        {
            if (typeof playlistPlayer === "undefined" || !playlistPlayer)
                return

            playlistPlayer.setCurrentPlaylist(PlaylistManager.currentPlaylistId)
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Playlist selector

    Rectangle
    {
        id: selectorBackground

        width: parent.width
        height: 32
        color: ColorTheme.colors.dark8

        ListView
        {
            id: playlistSelector

            anchors.left: parent.left
            anchors.right: newPlaylistButton.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom

            orientation: ListView.Horizontal
            clip: true
            spacing: 1
            model: playlistsModel

            delegate: Rectangle
            {
                readonly property bool current: playlistId === PlaylistManager.currentPlaylistId

                width: Math.max(80, playlistLabel.implicitWidth + 24)
                height: playlistSelector.height
                color: current ? ColorTheme.colors.dark11 : ColorTheme.colors.dark8

                Text
                {
                    id: playlistLabel

                    anchors.centerIn: parent
                    width: parent.width - 16
                    text: name
                    color: parent.current ? ColorTheme.colors.light10 : ColorTheme.colors.light12
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }

                MouseArea
                {
                    anchors.fill: parent
                    onClicked: PlaylistManager.currentPlaylistId = playlistId
                }
            }
        }

        Button
        {
            id: newPlaylistButton

            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 32
            text: "+"
            onClicked: PlaylistManager.createPlaylist()
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Entries list

    ListView
    {
        id: entryList

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: selectorBackground.bottom
        anchors.bottom: toolbar.top

        clip: true
        spacing: 1
        model: entriesModel
        keyNavigationEnabled: true

        delegate: Rectangle
        {
            width: entryList.width
            height: 28
            color: ListView.isCurrentItem ? ColorTheme.colors.dark11 : "transparent"

            Row
            {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Text
                {
                    width: 32
                    text: index + 1
                    color: ColorTheme.colors.light12
                    horizontalAlignment: Text.AlignRight
                }

                Rectangle
                {
                    width: 16
                    height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    color: isFolder ? ColorTheme.colors.brand_core : ColorTheme.colors.dark10
                }

                Text
                {
                    width: parent.width - 32 - 16 - parent.spacing * 2
                    text: fileName
                    color: ColorTheme.colors.light12
                    elide: Text.ElideMiddle
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            MouseArea
            {
                anchors.fill: parent
                onClicked: entryList.currentIndex = index
                onDoubleClicked:
                {
                    entryList.currentIndex = index
                    playlistBrowser.playEntry(index)
                }
            }
        }
    }

    Text
    {
        anchors.centerIn: entryList
        visible: entryList.count === 0
        text: qsTr("Playlist is empty")
        color: ColorTheme.colors.light12
    }

    // ---------------------------------------------------------------------------------------------
    // Toolbar

    Rectangle
    {
        id: toolbar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 36
        color: ColorTheme.colors.dark10

        RowLayout
        {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 4
            spacing: 4

            Button
            {
                text: qsTr("Prev")
                onClicked: playlistBrowser.playPrevious()
            }

            Button
            {
                text: qsTr("Next")
                onClicked: playlistBrowser.playNext()
            }

            Button
            {
                text: qsTr("Add")
                onClicked: addFilesDialog.open()
            }

            Button
            {
                text: qsTr("Add Folder")
                onClicked: addFolderDialog.open()
            }

            Button
            {
                text: qsTr("Remove")
                enabled: entryList.currentIndex >= 0
                onClicked: PlaylistManager.removeEntries(
                    PlaylistManager.currentPlaylistId, [entryList.currentIndex])
            }

            Button
            {
                text: qsTr("Up")
                enabled: entryList.currentIndex > 0
                onClicked: PlaylistManager.moveEntry(PlaylistManager.currentPlaylistId,
                    entryList.currentIndex, entryList.currentIndex - 1)
            }

            Button
            {
                text: qsTr("Down")
                enabled: entryList.currentIndex >= 0
                    && entryList.currentIndex < entryList.count - 1
                onClicked: PlaylistManager.moveEntry(PlaylistManager.currentPlaylistId,
                    entryList.currentIndex, entryList.currentIndex + 1)
            }

            Item { Layout.fillWidth: true }

            Button
            {
                text: qsTr("Clear")
                enabled: entryList.count > 0
                onClicked: PlaylistManager.clearEntries(PlaylistManager.currentPlaylistId)
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // File dialogs

    FileDialog
    {
        id: addFilesDialog

        title: qsTr("Add Media Files")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            qsTr("Media files") + " (*.mp4 *.mkv *.avi *.mov *.wmv *.flv *.webm *.m4v *.mpg *.mpeg"
                + " *.ts *.m2ts *.mts *.vob *.ogv *.mxf *.3gp)",
            qsTr("All files") + " (*)"
        ]

        onAccepted: PlaylistManager.addFiles(PlaylistManager.currentPlaylistId, selectedFiles)
    }

    FolderDialog
    {
        id: addFolderDialog

        title: qsTr("Add Folder to Playlist")
        onAccepted: PlaylistManager.addFiles(PlaylistManager.currentPlaylistId, [selectedFolder])
    }
}
