// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import QtQml

import Nx.Core 1.0
import Nx.Controls 1.0 as NxControls
import Nx.Dialogs 1.0 as NxDialogs

import nx.vms.client.core 1.0
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

    // Downloads started from the context menu report progress; the rows poll it while it lasts.
    Timer
    {
        id: cacheTick

        property int value: 0

        interval: 500
        repeat: true
        running: true
        onTriggered: value++
    }

    // ---------------------------------------------------------------------------------------------
    // Playback

    function playEntry(row)
    {
        if (typeof playlistPlayer === "undefined" || !playlistPlayer || row < 0)
            return

        playlistPlayer.playEntry(PlaylistManager.currentPlaylistId, row)
    }

    /** Formats a duration given in milliseconds as "mm:ss" or "h:mm:ss"; empty for unknown. */
    function formatDuration(ms)
    {
        if (ms === undefined || ms === null || ms < 0)
            return ""

        const totalSeconds = Math.floor(ms / 1000)
        const seconds = totalSeconds % 60
        const minutes = Math.floor(totalSeconds / 60) % 60
        const hours = Math.floor(totalSeconds / 3600)
        const pad = (value) => (value < 10 ? "0" + value : "" + value)

        return hours > 0
            ? hours + ":" + pad(minutes) + ":" + pad(seconds)
            : pad(minutes) + ":" + pad(seconds)
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
        anchors.bottom: playbackErrorBar.top
        anchors.topMargin: 4

        clip: true
        model: entriesModel
        keyNavigationEnabled: true
        currentIndex: -1

        Keys.onDeletePressed:
        {
            if (currentIndex >= 0)
                removeEntry(currentIndex)
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
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (mouse) =>
                {
                    entryList.currentIndex = index

                    if (mouse.button === Qt.RightButton)
                    {
                        entryContextMenu.targetRow = index
                        entryContextMenu.popup(entryDelegate, mouse.x, mouse.y)
                    }
                }
                onDoubleClicked:
                {
                    entryList.currentIndex = index
                    playlistBrowser.playEntry(index)
                }
            }

            Text
            {
                id: entryIndex

                anchors.left: parent.left
                anchors.leftMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                width: 26
                text: index + 1
                color: ColorTheme.colors.light12
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                font.pixelSize: 12
            }

            Image
            {
                id: entryIcon

                anchors.left: entryIndex.right
                anchors.leftMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                sourceSize: Qt.size(16, 16)
                source: isFolder
                    ? "image://skin/20x20/Outline/folder.svg?primary=light12"
                    : (isStream
                        ? "image://skin/20x20/Outline/earth.svg?primary=light12"
                        : "image://skin/20x20/Outline/play.svg?primary=light12")
            }

            Text
            {
                id: entryDuration

                readonly property real cacheProgress: {
                    cacheTick.value //< Re-read while a download is in progress.
                    return (isStream && entryUrl.length > 0
                        && typeof streamCache !== "undefined" && streamCache)
                        ? streamCache.cachingProgress(entryUrl)
                        : -1
                }

                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: cacheProgress >= 0
                    ? Math.round(cacheProgress * 100) + "%"
                    : playlistBrowser.formatDuration(durationMs)
                color: cacheProgress >= 0
                    ? ColorTheme.colors.brand_core
                    : ColorTheme.colors.light12
                font.pixelSize: 11
            }

            Text
            {
                anchors.left: entryIcon.right
                anchors.leftMargin: 6
                anchors.right: entryDuration.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                text: fileName
                color: ColorTheme.colors.light12
                elide: Text.ElideMiddle
                font.pixelSize: 12
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

    // A stream address may carry a signature that has expired; the only fix is scanning the page
    // it came from again, so the player says so instead of leaving the item silent.
    Rectangle
    {
        id: playbackErrorBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: toolbar.top
        height: playbackErrorText.text === "" ? 0 : playbackErrorText.implicitHeight + 10
        visible: height > 0
        color: ColorTheme.colors.dark10

        Text
        {
            id: playbackErrorText

            anchors.fill: parent
            anchors.margins: 5
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: ColorTheme.colors.red
            font.pixelSize: 12
            text: (typeof playlistPlayer !== "undefined" && playlistPlayer && playlistPlayer.lastError)
                ? playlistPlayer.lastError
                : ""
        }
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
                icon.source: "image://skin/20x20/Outline/earth.svg"
                GlobalToolTip.text: qsTr("Add program source")
                onClicked:
                {
                    sourceField.text = ""
                    nameField.text = ""
                    errorText.text = ""
                    addSourceDialog.show()
                    sourceField.forceActiveFocus()
                }
            }

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                icon.source: "image://skin/20x20/Outline/delete.svg"
                GlobalToolTip.text: qsTr("Remove selected")
                enabled: entryList.currentIndex >= 0
                onClicked: removeEntry(entryList.currentIndex)
            }

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                icon.source: "image://skin/20x20/Outline/arrow_up.svg"
                GlobalToolTip.text: qsTr("Move up")
                enabled: entryList.currentIndex > 0
                onClicked: moveEntry(entryList.currentIndex, entryList.currentIndex - 1)
            }

            ToolButton
            {
                anchors.verticalCenter: parent.verticalCenter
                icon.source: "image://skin/20x20/Outline/arrow_down.svg"
                GlobalToolTip.text: qsTr("Move down")
                enabled: entryList.currentIndex >= 0
                    && entryList.currentIndex < entryList.count - 1
                onClicked: moveEntry(entryList.currentIndex, entryList.currentIndex + 1)
            }
        }
    }

    function removeEntry(row)
    {
        if (row < 0)
            return

        PlaylistManager.removeEntries(PlaylistManager.currentPlaylistId, [row])
        entryList.currentIndex = Math.min(row, entryList.count - 1)
    }

    function moveEntry(fromRow, toRow)
    {
        if (fromRow < 0 || toRow < 0 || toRow >= entryList.count)
            return

        PlaylistManager.moveEntry(PlaylistManager.currentPlaylistId, fromRow, toRow)
        entryList.currentIndex = toRow
    }

    function renameEntry(row)
    {
        if (row < 0)
            return

        renameEntryDialog.targetRow = row
        renameEntryDialog.open()
    }

    function entryDisplayNameAt(row)
    {
        return PlaylistManager.entryDisplayName(PlaylistManager.currentPlaylistId, row)
    }

    function entryPathAt(row)
    {
        return PlaylistManager.entryPath(PlaylistManager.currentPlaylistId, row)
    }

    function entryUrlAt(row)
    {
        return PlaylistManager.entryUrl(PlaylistManager.currentPlaylistId, row)
    }

    function entryIsStreamAt(row)
    {
        return entryUrlAt(row).length > 0
    }

    /** Path of local media or URL of a stream: whatever identifies the entry. */
    function entryTargetAt(row)
    {
        return entryIsStreamAt(row) ? entryUrlAt(row) : entryPathAt(row)
    }

    // ---------------------------------------------------------------------------------------------
    // Playlist actions menu.

    NxControls.CompactMenu
    {
        id: playlistMenu

        background: Rectangle
        {
            radius: 2
            color: ColorTheme.colors.dark13
        }

        NxControls.CompactMenuItem
        {
            text: qsTr("Sort by name")
            enabled: entryList.count > 1
            onTriggered: PlaylistManager.sortEntriesByName(PlaylistManager.currentPlaylistId)
        }

        NxControls.CompactMenuItem
        {
            text: qsTr("Clear playlist")
            enabled: entryList.count > 0
            onTriggered: PlaylistManager.clearEntries(PlaylistManager.currentPlaylistId)
        }

        NxControls.CompactMenuSeparator {}

        NxControls.CompactMenuItem
        {
            text: qsTr("Rename playlist")
            enabled: playlistSelector.count > 0
            onTriggered: renameDialog.open()
        }

        NxControls.CompactMenuItem
        {
            text: qsTr("Remove playlist")
            enabled: playlistSelector.count > 0
            onTriggered:
                PlaylistManager.removePlaylist(PlaylistManager.currentPlaylistId)
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Entry context menu.

    NxControls.CompactMenu
    {
        id: entryContextMenu

        background: Rectangle
        {
            radius: 2
            color: ColorTheme.colors.dark13
        }

        property int targetRow: -1

        NxControls.CompactMenuItem
        {
            text: qsTr("Play")
            enabled: entryContextMenu.targetRow >= 0
            onTriggered: playlistBrowser.playEntry(entryContextMenu.targetRow)
        }

        NxControls.CompactMenuItem
        {
            readonly property string targetUrl: entryContextMenu.targetRow >= 0
                ? playlistBrowser.entryUrlAt(entryContextMenu.targetRow)
                : ""
            readonly property real progress: (targetUrl.length > 0
                && typeof streamCache !== "undefined" && streamCache)
                ? streamCache.cachingProgress(targetUrl)
                : -1
            readonly property bool cached: (targetUrl.length > 0
                && typeof streamCache !== "undefined" && streamCache)
                ? streamCache.isFullyCached(targetUrl)
                : false

            text: progress >= 0
                ? qsTr("Stop caching")
                : (cached ? qsTr("Cached") : qsTr("Cache locally"))
            enabled: targetUrl.length > 0 && !cached
            onTriggered:
            {
                if (progress >= 0)
                    streamCache.stopCaching(targetUrl)
                else
                    streamCache.cacheStream(targetUrl)
            }
        }

        NxControls.CompactMenuSeparator {}

        NxControls.CompactMenuItem
        {
            text: qsTr("Move up")
            enabled: entryContextMenu.targetRow > 0
            onTriggered: playlistBrowser.moveEntry(entryContextMenu.targetRow,
                entryContextMenu.targetRow - 1)
        }

        NxControls.CompactMenuItem
        {
            text: qsTr("Move down")
            enabled: entryContextMenu.targetRow >= 0
                && entryContextMenu.targetRow < entryList.count - 1
            onTriggered: playlistBrowser.moveEntry(entryContextMenu.targetRow,
                entryContextMenu.targetRow + 1)
        }

        NxControls.CompactMenuItem
        {
            text: qsTr("Rename")
            enabled: entryContextMenu.targetRow >= 0
            onTriggered: playlistBrowser.renameEntry(entryContextMenu.targetRow)
        }

        NxControls.CompactMenuItem
        {
            text: qsTr("Remove from playlist")
            enabled: entryContextMenu.targetRow >= 0
            onTriggered: playlistBrowser.removeEntry(entryContextMenu.targetRow)
        }

        NxControls.CompactMenu
        {
            id: addToPlaylistMenu

            title: qsTr("Add to playlist")
            enabled: entryContextMenu.targetRow >= 0 && playlistSelector.count > 1

            background: Rectangle
            {
                radius: 2
                color: ColorTheme.colors.dark13
            }

            Instantiator
            {
                model: playlistsModel

                delegate: NxControls.CompactMenuItem
                {
                    readonly property string targetPlaylistId: playlistId

                    // The entry already belongs to the current playlist.
                    visible: targetPlaylistId !== PlaylistManager.currentPlaylistId
                    text: name

                    onTriggered: PlaylistManager.copyEntry(PlaylistManager.currentPlaylistId,
                        entryContextMenu.targetRow, targetPlaylistId)
                }

                onObjectAdded: (index, object) =>
                    addToPlaylistMenu.insertItem(index, object)
                onObjectRemoved: (index, object) =>
                    addToPlaylistMenu.removeItem(object)
            }
        }

        NxControls.CompactMenuSeparator {}

        NxControls.CompactMenuItem
        {
            text: playlistBrowser.entryIsStreamAt(entryContextMenu.targetRow)
                ? qsTr("Copy URL") : qsTr("Copy path")
            enabled: entryContextMenu.targetRow >= 0
            onTriggered: NxGlobals.copyToClipboard(
                playlistBrowser.entryTargetAt(entryContextMenu.targetRow))
        }

        NxControls.CompactMenuItem
        {
            text: qsTr("Show in file manager")
            enabled: entryContextMenu.targetRow >= 0
                && !playlistBrowser.entryIsStreamAt(entryContextMenu.targetRow)
            onTriggered: PlaylistManager.showInFileManager(
                playlistBrowser.entryPathAt(entryContextMenu.targetRow))
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

    // Adds the channels of an M3U program source (an IPTV playlist) as ordinary playlist entries.
    // A window-based dialog is required: the panel itself lives in a small QQuickWidget, which
    // would clip a popup-style dialog.
    NxDialogs.Dialog
    {
        id: addSourceDialog

        title: qsTr("Add Program Source")
        modality: Qt.ApplicationModal
        transientParent: playlistBrowser.Window.window

        width: 520
        height: 270
        x: (Screen.width - width) / 2
        y: (Screen.height - height) / 2

        function submit()
        {
            errorText.text = ""
            if (!IptvManager.addSourceToCurrentPlaylist(sourceField.text.trim(), nameField.text))
                errorText.text = IptvManager.lastError()
        }

        contentItem: ColumnLayout
        {
            spacing: 8

            Text
            {
                Layout.fillWidth: true
                text: qsTr("Enter an M3U playlist (an http(s) URL or a local file), or a single"
                    + " stream URL. Whatever it lists is added to the current playlist.")
                color: ColorTheme.colors.light4
                font.pixelSize: FontConfig.normal.pixelSize
                wrapMode: Text.WordWrap
            }

            TextField
            {
                id: sourceField

                Layout.fillWidth: true
                placeholderText: qsTr("http://host/playlist.m3u, rtsp://host/stream or C:\\list.m3u")
                selectByMouse: true
                onAccepted: addSourceDialog.submit()
            }

            TextField
            {
                id: nameField

                Layout.fillWidth: true
                placeholderText: qsTr("Name (optional) - shown in the list instead of the URL")
                selectByMouse: true
                onAccepted: addSourceDialog.submit()
            }

            RowLayout
            {
                Layout.fillWidth: true

                Item { Layout.fillWidth: true }

                NxControls.Button
                {
                    text: qsTr("Add")
                    enabled: sourceField.text.length > 0 && !IptvManager.importing
                    onClicked: addSourceDialog.submit()
                }
            }

            Text
            {
                id: errorText

                Layout.fillWidth: true
                color: ColorTheme.colors.red
                wrapMode: Text.WordWrap
                visible: text.length > 0
            }
        }

        buttonBox: NxControls.DialogButtonBox
        {
            buttonLayout: NxControls.DialogButtonBox.KdeLayout
            standardButtons: NxControls.DialogButtonBox.Close
        }

        Connections
        {
            target: IptvManager

            function onImportFinished(ok)
            {
                if (!addSourceDialog.visible)
                    return

                if (ok)
                    addSourceDialog.close()
                else
                    errorText.text = IptvManager.lastError()
            }
        }
    }

    Dialog
    {
        id: renameDialog

        title: qsTr("Rename playlist")
        modal: true
        anchors.centerIn: parent
        width: 320
        padding: 12
        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened:
        {
            renameField.text = PlaylistManager.currentPlaylistName
            renameField.selectAll()
            renameField.forceActiveFocus()
        }

        onAccepted:
        {
            if (renameField.text.length > 0)
                PlaylistManager.renamePlaylist(PlaylistManager.currentPlaylistId, renameField.text)
        }

        contentItem: TextField
        {
            id: renameField
            selectByMouse: true
        }
    }

    Dialog
    {
        id: renameEntryDialog

        /** Row of the entry being renamed. */
        property int targetRow: -1

        title: qsTr("Rename")
        modal: true
        anchors.centerIn: parent
        width: 320
        padding: 12
        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened:
        {
            renameEntryField.text = playlistBrowser.entryDisplayNameAt(targetRow)
            renameEntryField.selectAll()
            renameEntryField.forceActiveFocus()
        }

        onAccepted:
        {
            if (renameEntryField.text.length > 0)
            {
                PlaylistManager.renameEntry(PlaylistManager.currentPlaylistId,
                    targetRow, renameEntryField.text)
            }
        }

        contentItem: TextField
        {
            id: renameEntryField
            selectByMouse: true
        }
    }
}
