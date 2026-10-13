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
    height: 600
    minimumWidth: 520
    minimumHeight: 600

    modality: Qt.ApplicationModal

    x: (Screen.width - width) / 2
    y: (Screen.height - height) / 2

    // Id of the source being edited, empty when adding a new one.
    property string editingId: ""

    function clearForm()
    {
        nameField.text = ""
        urlField.text = ""
        usernameField.text = ""
        passwordField.text = ""
        control.editingId = ""
    }

    function commit()
    {
        if (urlField.text.length === 0)
            return

        const changed = control.editingId.length > 0
            ? LocalCameraManager.updateSource(control.editingId, nameField.text,
                urlField.text, usernameField.text, passwordField.text)
            : LocalCameraManager.addSource(nameField.text, urlField.text,
                usernameField.text, passwordField.text)

        if (changed)
            control.clearForm()
    }

    function startEdit(source)
    {
        if (!source)
            return

        control.editingId = source.id
        nameField.text = source.name !== undefined ? source.name : ""
        urlField.text = source.url !== undefined ? source.url : ""
        usernameField.text = source.username !== undefined ? source.username : ""
        passwordField.text = ""
        urlField.forceActiveFocus()
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
                            anchors.right: actionRow.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter

                            // Only the sanitized name is shown, so credentials are never displayed;
                            // a source without a name falls back to its URL.
                            text: modelData
                                ? (modelData.name && modelData.name.length > 0
                                    ? modelData.name : modelData.url)
                                : ""
                            color: ColorTheme.colors.light4
                            font.pixelSize: FontConfig.normal.pixelSize
                            elide: Text.ElideMiddle
                        }

                        Row
                        {
                            id: actionRow

                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6

                            Button
                            {
                                text: qsTr("Edit")
                                onClicked: control.startEdit(modelData)
                            }

                            Button
                            {
                                text: qsTr("Remove")
                                onClicked: if (modelData) LocalCameraManager.removeSource(modelData.id)
                            }
                        }
                    }
                }
            }
        }

        Text
        {
            text: control.editingId.length > 0 ? qsTr("Edit source") : qsTr("Add a source")
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
            placeholderText: qsTr("rtsp://host:554/stream")
            onAccepted: control.commit()
        }

        Row
        {
            width: parent.width
            spacing: 10

            TextField
            {
                id: usernameField

                width: (parent.width - parent.spacing) / 2
                placeholderText: qsTr("Username (optional)")
            }

            TextField
            {
                id: passwordField

                width: (parent.width - parent.spacing) / 2
                placeholderText: control.editingId.length > 0
                    ? qsTr("Password (unchanged)")
                    : qsTr("Password (optional)")
                echoMode: TextInput.Password
                onAccepted: control.commit()
            }
        }

        Text
        {
            width: parent.width

            text: control.editingId.length > 0
                ? qsTr("Leave the password empty to keep the current one. Credentials are stored"
                    + " locally and never shown.")
                : qsTr("If the camera requires authentication, fill in the username and password"
                    + " above (or embed them into the URL). They are stored locally and never shown.")
            color: ColorTheme.transparent(ColorTheme.colors.light4, 0.6)
            font.pixelSize: FontConfig.small.pixelSize
            wrapMode: Text.WordWrap
        }

        Row
        {
            spacing: 10

            Button
            {
                isAccentButton: true
                text: control.editingId.length > 0 ? qsTr("Save") : qsTr("Add")
                enabled: urlField.text.length > 0
                onClicked: control.commit()
            }

            Button
            {
                visible: control.editingId.length > 0
                text: qsTr("Cancel")
                onClicked: control.clearForm()
            }
        }
    }

    buttonBox: DialogButtonBox
    {
        buttonLayout: DialogButtonBox.KdeLayout
        standardButtons: DialogButtonBox.Close
    }
}
