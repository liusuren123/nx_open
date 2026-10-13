// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

import QtQuick 2.6

import Nx.Core 1.0
import Nx.Controls 1.0
import Nx.RightPanel 1.0

import "private"

Item
{
    id: resourcesPanel

    property alias scene: resourceTree.scene
    property alias tree: resourceTree

    property bool playlistTabAutoSelected: false

    // In the local player mode the resource tree is empty, so the Playlist tab is shown first.
    function autoSelectPlaylistTab()
    {
        if (playlistTabAutoSelected || !resourceTree.localFilesMode)
            return

        playlistTabAutoSelected = true
        tabControl.currentTabIndex = 1
    }

    Component.onCompleted: autoSelectPlaylistTab()

    Connections
    {
        target: resourceTree

        function onLocalFilesModeChanged()
        {
            resourcesPanel.autoSelectPlaylistTab()
        }
    }

    clip: true

    function clearFilters()
    {
        resourceSearchPane.clear()
    }

    function focusSearchField()
    {
        resourceSearchPane.focus = true
    }

    TabControl
    {
        id: tabControl

        anchors.fill: parent
        spacing: 0

        Tab
        {
            button: PrimaryTabButton
            {
                text: qsTr("Browser")
            }

            page: Item
            {
                ResourceSearchPane
                {
                    id: resourceSearchPane

                    width: parent.width
                    menuEnabled: !resourceTree.localFilesMode
                    isFilterRelevant: (type => resourceTree.isFilterRelevant(type))

                    onEnterPressed: (modifiers) =>
                    {
                        if (resourceTree.model.isFiltering)
                            resourceTree.model.activateSearchResults(modifiers)
                    }

                    Keys.onPressed: (event) =>
                    {
                        if (event.key === Qt.Key_Down)
                        {
                            resourceTree.focus = true
                            resourceTree.setSelection([resourceTree.topmostSelectableIndex()])
                        }

                        event.accepted = true
                    }
                }

                // In the local player mode there is no server, so client-side video sources
                // (RTSP cameras and the like) are configured from here.
                Item
                {
                    id: localSourcesPane

                    width: parent.width
                    height: visible ? 36 : 0
                    y: resourceSearchPane.y + resourceSearchPane.height
                    visible: resourceTree.localFilesMode

                    Button
                    {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter

                        text: qsTr("Local sources…")
                        onClicked: localSourcesDialog.show()
                    }
                }

                Rectangle
                {
                    id: separator

                    width: parent.width
                    height: 1
                    y: localSourcesPane.y + localSourcesPane.height
                    color: ColorTheme.transparent(ColorTheme.colors.dark8, 0.4)
                }

                ResourceTree
                {
                    id: resourceTree

                    y: separator.y + separator.height
                    width: parent.width - 1
                    height: parent.height - y

                    filterText: resourceSearchPane.filterText
                    filterType: resourceSearchPane.filterType
                    shortcutHintsEnabled: resourceSearchPane.activeFocus

                    toolTipEnclosingRect: Qt.rect(
                        resourceTree.width,
                        -resourceSearchPane.height + 2,
                        Number.MAX_VALUE / 2,
                        resourceTree.height + resourceSearchPane.height - 4)

                    function topmostSelectableIndex()
                    {
                        const count = resourceTree.model.rowCount(resourceTree.rootIndex)
                        for (let row = 0; row < count; ++row)
                        {
                            const index = resourceTree.model.index(row, 0, resourceTree.rootIndex)
                            if (resourceTree.model.flags(index) & Qt.ItemIsSelectable)
                                return index
                        }

                        return NxGlobals.invalidModelIndex()
                    }

                    Keys.onUpPressed: (event) =>
                    {
                        if (NxGlobals.fromPersistent(currentIndex) == topmostSelectableIndex())
                            resourceSearchPane.focus = true
                        else
                            event.accepted = false
                    }

                    Connections
                    {
                        target: resourceTree.model

                        function onLocalFilesModeChanged()
                        {
                            resourcesPanel.clearFilters()
                        }
                    }
                }

                ResultsPlaceholder
                {
                    id: placeholderItem

                    anchors.topMargin: resourceTree.y
                    anchors.fill: parent
                    shown: resourceTree.empty

                    icon: resourceTree.localFilesMode
                        ? "image://skin/64x64/Outline/openfolder.svg?primary=dark10"
                        : "image://skin/64x64/Outline/notfound.svg?primary=dark10"

                    title: resourceTree.localFilesMode && !resourceTree.isFiltering
                        ? qsTr("No local files") : qsTr("Nothing found")

                    description:
                    {
                        if (resourceTree.localFilesMode)
                        {
                            return qsTr("Drag video files or images to the client window, or add local media"
                                + " folder through the Local Settings dialog")
                        }

                        return qsTr("Try searching for something else")
                    }
                }
            }
        }

        Tab
        {
            button: PrimaryTabButton
            {
                text: qsTr("Playlist")
            }

            page: PlaylistBrowser
            {
                id: playlistBrowser
            }
        }
    }

    LocalSourcesDialog
    {
        id: localSourcesDialog

        transientParent: resourcesPanel.Window.window
    }
}
