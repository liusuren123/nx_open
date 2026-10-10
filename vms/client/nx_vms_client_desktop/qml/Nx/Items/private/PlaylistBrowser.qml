// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Nx.Core 1.0
import Nx.Controls 1.0 as NxControls

import nx.vms.client.desktop 1.0

/**
 * Local player sidebar: a playlist selector, a filterable list of the entries of the current
 * playlist, and the playback / editing toolbar.
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
        filter: searchField.text
    }

    // ---------------------------------------------------------------------------------------------
    // Playback

    function playEntry(row)
    {
        if (typeof playlistPlayer === "undefined" || !playlistPlayer || row < 0)
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
    // Compact icon button used by the toolbar.

    component ToolButton: NxControls.Button
    {
        height: 24
        width: 28
        leftPadding: 4
        rightPadding: 4
        backgroundColor: ColorTheme.colors.dark10
        enabled: true
    }

    // ---------------------------------------------------------------------------------------------
    // Playlist selector row.

    Rectangle
    {
        id: selectorRow

        width: parent.width
        height: 30
        color: ColorTheme.colors.dark8

        ListView
        {
            id: playlistSelector

            anchors.left: parent.left
            anchors.right: selectorButtons.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom

            orientation: ListView.Horizontal
            clip: true
            spacing: 1
            model: playlistsModel

            onCountChanged: ensureCurrentVisible()

            function ensureCurrentVisible()
            {
                for (let i = 0; i < count; ++i)
                {
                    const item = itemAtIndex(i)
                    if (item && item.isCurrent)
                    {
                        positionViewAtIndex(i, ListView.Contain)
                        return
                    }
                }
            }

            delegate: Rectangle
            {
                id: playlistDelegate

                readonly property bool isCurrent:
                    playlistId === PlaylistManager.currentPlaylistId

                width: Math.max(64, playlistLabel.implicitWidth + 20)
                height: playlistSelector.height
                color: isCurrent
                    ? ColorTheme.colors.dark11
                    : (playlistMouse.containsMouse ? ColorTheme.colors.dark9 : "transparent")

                Text
                {
                    id: playlistLabel

                    anchors.centerIn: parent
                    width: parent.width - 12
                    text: name
                    color: playlistDelegate.isCurrent
                        ? ColorTheme.colors.light10
                        : ColorTheme.colors.light12
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: 12
                }

                Rectangle
                {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 2
                    color: ColorTheme.colors.brand_core
                    visible: playlistDelegate.isCurrent
                }

                MouseArea
                {
                    id: playlistMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: PlaylistManager.currentPlaylistId = playlistId
                }
            }
        }

        Row
        {
            id: selectorButtons

            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            spacing: 1

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                text: "+"
                GlobalToolTip.text: qsTr("New playlist")
                onClicked: PlaylistManager.createPlaylist()
            }

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                text: "\u25BE"
                GlobalToolTip.text: qsTr("Playlist actions")
                onClicked: playlistMenu.popup(this)
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Entry filter.

    NxControls.SearchField
    {
        id: searchField

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: selectorRow.bottom
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        anchors.topMargin: 4

        height: 26
        darkMode: true
    }

    // ---------------------------------------------------------------------------------------------
    // Entries list.

    ListView
    {
        id: entryList

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchField.bottom
        anchors.bottom: toolbar.top
        anchors.topMargin: 4

        clip: true
        model: entriesModel
        keyNavigationEnabled: true
        currentIndex: -1

        Keys.onDeletePressed:
        {
            if (currentIndex >= 0)
                removeCurrentEntry()
        }

        Keys.onReturnPressed:
        {
            if (currentIndex >= 0)
                playlistBrowser.playEntry(currentIndex)
        }

        delegate: Rectangle
        {
            id: entryDelegate

            width: entryList.width
            height: 26
            color: ListView.isCurrentItem
                ? ColorTheme.colors.dark11
                : (entryMouse.containsMouse ? ColorTheme.colors.dark9 : "transparent")

            MouseArea
            {
                id: entryMouse

                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                onClicked: entryList.currentIndex = index
                onDoubleClicked:
                {
                    entryList.currentIndex = index
                    playlistBrowser.playEntry(index)
                }
            }

            Row
            {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 8
                spacing: 6

                Text
                {
                    width: 26
                    text: index + 1
                    color: ColorTheme.colors.light12
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideRight
                    anchors.verticalCenter: parent.verticalCenter
                    font.pixelSize: 12
                }

                Image
                {
                    width: 16
                    height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    sourceSize: Qt.size(16, 16)
                    source: isFolder
                        ? "image://skin/20x20/Outline/folder.svg?primary=light12"
                        : "image://skin/20x20/Outline/play.svg?primary=light12"
                }

                Text
                {
                    width: parent.width - 26 - 16 - parent.spacing * 3
                    text: fileName
                    color: ColorTheme.colors.light12
                    elide: Text.ElideMiddle
                    anchors.verticalCenter: parent.verticalCenter
                    font.pixelSize: 12
                }
            }
        }
    }

    Text
    {
        anchors.centerIn: entryList
        width: entryList.width - 24
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        visible: entryList.count === 0
        color: ColorTheme.colors.light12
        text: playlistSelector.count === 0
            ? qsTr("No playlists yet.\nClick + to create one, or open a folder.")
            : (searchField.text
                ? qsTr("Nothing matches the filter")
                : qsTr("The playlist is empty.\nUse Add to add media files or folders."))
    }

    // ---------------------------------------------------------------------------------------------
    // Toolbar.

    Rectangle
    {
        id: toolbar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 32
        color: ColorTheme.colors.dark10

        Row
        {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 4
            spacing: 2
            layoutDirection: Qt.LeftToRight

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                icon.source: "image://skin/20x20/Outline/play.svg"
                GlobalToolTip.text: qsTr("Play")
                enabled: entryList.currentIndex >= 0
                onClicked: playlistBrowser.playEntry(entryList.currentIndex)
            }

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                icon.source: "image://skin/20x20/Outline/addtolist.svg"
                GlobalToolTip.text: qsTr("Add media files")
                onClicked: addFilesDialog.open()
            }

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                icon.source: "image://skin/20x20/Outline/folder.svg"
                GlobalToolTip.text: qsTr("Add folder")
                onClicked: addFolderDialog.open()
            }

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                icon.source: "image://skin/20x20/Outline/delete.svg"
                GlobalToolTip.text: qsTr("Remove selected")
                enabled: entryList.currentIndex >= 0
                onClicked: removeCurrentEntry()
            }

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                icon.source: "image://skin/20x20/Outline/arrow_up.svg"
                GlobalToolTip.text: qsTr("Move up")
                enabled: entryList.currentIndex > 0
                onClicked: moveCurrent(-1)
            }

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                icon.source: "image://skin/20x20/Outline/arrow_down.svg"
                GlobalToolTip.text: qsTr("Move down")
                enabled: entryList.currentIndex >= 0
                    && entryList.currentIndex < entryList.count - 1
                onClicked: moveCurrent(1)
            }
        }
    }

    function removeCurrentEntry()
    {
        const row = entryList.currentIndex
        if (row < 0)
            return

        PlaylistManager.removeEntries(PlaylistManager.currentPlaylistId, [row])
        entryList.currentIndex = Math.min(row, entryList.count - 1)
    }

    function moveCurrent(offset)
    {
        const row = entryList.currentIndex
        if (row < 0)
            return

        const target = row + offset
        if (target < 0 || target >= entryList.count)
            return

        PlaylistManager.moveEntry(PlaylistManager.currentPlaylistId, row, target)
        entryList.currentIndex = target
    }

    // ---------------------------------------------------------------------------------------------
    // Playlist actions menu.

    Menu
    {
        id: playlistMenu

        MenuItem
        {
            text: qsTr("Sort by name")
            enabled: entryList.count > 1
            onTriggered: PlaylistManager.sortEntriesByName(PlaylistManager.currentPlaylistId)
        }

        MenuItem
        {
            text: qsTr("Clear playlist")
            enabled: entryList.count > 0
            onTriggered: PlaylistManager.clearEntries(PlaylistManager.currentPlaylistId)
        }

        MenuSeparator {}

        MenuItem
        {
            text: qsTr("Remove playlist")
            enabled: playlistSelector.count > 0
            onTriggered:
                PlaylistManager.removePlaylist(PlaylistManager.currentPlaylistId)
        }
    }

    // ---------------------------------------------------------------------------------------------
    // File dialogs.

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

        onAccepted: PlaylistManager.addFilesToCurrentPlaylist(selectedFiles)
    }

    FolderDialog
    {
        id: addFolderDialog

        title: qsTr("Add Folder to Playlist")
        onAccepted: PlaylistManager.addFilesToCurrentPlaylist([selectedFolder])
    }
}
