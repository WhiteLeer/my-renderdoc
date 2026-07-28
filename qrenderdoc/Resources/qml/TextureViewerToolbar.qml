import QtQuick 2.9

Rectangle {
  id: root

  readonly property bool compact: width > 0 && width < 500
  readonly property int barHeight: 82
  readonly property int buttonHeight: compact ? 30 : 32
  readonly property int buttonRadius: compact ? 7 : 8
  readonly property int outerRadius: compact ? 10 : 11
  readonly property int horizontalPadding: compact ? 8 : 10
  readonly property int closeButtonSpace: compact ? 30 : 34
  readonly property int controlGap: compact ? 3 : 5
  readonly property int channelWidth: compact ? 28 : 32
  readonly property int gammaWidth: compact ? 60 : 68
  readonly property int flipWidth: compact ? 58 : 66
  readonly property int saveWidth: compact ? 66 : 74
  readonly property int scrollbarHeight: 7
  readonly property color panelColor: "#1b232e"
  readonly property color buttonColor: "#293748"
  readonly property color selectedColor: "#315b86"
  readonly property color hoverColor: "#354a62"
  readonly property color pressedColor: "#4777a5"
  readonly property color borderColor: "#435265"
  readonly property color textColor: "#e7eef7"
  readonly property color mutedColor: "#9eacbb"

  implicitHeight: barHeight
  implicitWidth: 520
  radius: outerRadius
  color: panelColor
  border.width: 1
  border.color: "#3a4758"
  clip: false

  property bool redEnabled: textureViewer.qmlGetChannelVisibility(0)
  property bool greenEnabled: textureViewer.qmlGetChannelVisibility(1)
  property bool blueEnabled: textureViewer.qmlGetChannelVisibility(2)
  property bool alphaEnabled: textureViewer.qmlGetChannelVisibility(3)
  property bool srgbDisplay: textureViewer.qmlIsGammaDisplay()
  property bool gammaAvailable: textureViewer.qmlCanUseGamma()
  property bool flipY: textureViewer.qmlIsFlipY()
  property bool saveAvailable: textureViewer.qmlCanSaveTexture()

  function channelEnabled(channelIndex) {
    if(channelIndex === 0)
      return root.redEnabled
    if(channelIndex === 1)
      return root.greenEnabled
    if(channelIndex === 2)
      return root.blueEnabled
    return root.alphaEnabled
  }

  Connections {
    target: textureViewer
    onQmlChannelsChanged: {
      root.redEnabled = red
      root.greenEnabled = green
      root.blueEnabled = blue
      root.alphaEnabled = alpha
    }
    onQmlGammaChanged: root.srgbDisplay = srgb
    onQmlGammaAvailabilityChanged: root.gammaAvailable = available
    onQmlFlipChanged: root.flipY = flip
    onQmlSaveAvailabilityChanged: root.saveAvailable = available
  }

  Rectangle {
    anchors.fill: parent
    anchors.margins: 1
    radius: root.outerRadius - 1
    color: "#202a36"
    opacity: 0.55
    z: -1
  }

  Flickable {
    id: controlsViewport
    anchors.left: parent.left
    anchors.leftMargin: root.horizontalPadding
    anchors.right: parent.right
    anchors.rightMargin: root.horizontalPadding + root.closeButtonSpace
    anchors.top: parent.top
    anchors.topMargin: 7
    anchors.bottom: horizontalScroll.top
    anchors.bottomMargin: 5
    contentWidth: controlGroups.width
    contentHeight: controlGroups.height
    clip: true
    interactive: contentWidth > width
    flickableDirection: Flickable.HorizontalFlick
    boundsBehavior: Flickable.StopAtBounds
    z: 1

    Row {
    id: controlGroups
    x: 0
    y: 0
    width: implicitWidth
    height: 52
    spacing: root.controlGap + 4

    Column {
      spacing: 4

      Text {
        text: "CHANNELS"
        color: root.mutedColor
        font.family: "Segoe UI"
        font.pixelSize: 10
        font.weight: Font.DemiBold
        height: 14
        verticalAlignment: Text.AlignVCenter
      }

      Row {
        spacing: root.controlGap

        Repeater {
          model: ["R", "G", "B", "A"]

          delegate: Rectangle {
            width: root.channelWidth
            height: root.buttonHeight
            radius: root.buttonRadius
            property bool selected: root.channelEnabled(index)
            color: selected ? root.selectedColor :
                   (channelMouse.pressed ? root.pressedColor :
                   (channelMouse.containsMouse ? root.hoverColor : root.buttonColor))
            border.width: 1
            border.color: selected ? "#5e8dbb" : root.borderColor

            Behavior on color { ColorAnimation { duration: 90 } }

            Text {
              anchors.centerIn: parent
              text: modelData
              color: root.textColor
              font.family: "Segoe UI"
              font.pixelSize: root.compact ? 10 : 11
              font.weight: Font.Medium
            }

            MouseArea {
              id: channelMouse
              anchors.fill: parent
              hoverEnabled: true
              cursorShape: Qt.PointingHandCursor
              onClicked: textureViewer.qmlToggleChannel(index)
            }
          }
        }
      }
    }

    Rectangle {
      width: 1
      height: 42
      color: "#4a5868"
      opacity: 0.8
      anchors.verticalCenter: parent.verticalCenter
    }

    Column {
      spacing: 4

      Text {
        text: "DISPLAY"
        color: root.mutedColor
        font.family: "Segoe UI"
        font.pixelSize: 10
        font.weight: Font.DemiBold
        height: 14
        verticalAlignment: Text.AlignVCenter
      }

      Row {
        spacing: root.controlGap

        Rectangle {
          width: root.gammaWidth
          height: root.buttonHeight
          radius: root.buttonRadius
          enabled: root.gammaAvailable
          opacity: enabled ? 1.0 : 0.45
          property bool selected: root.srgbDisplay
          color: selected ? root.selectedColor :
                 (gammaMouse.pressed ? root.pressedColor :
                 (gammaMouse.containsMouse ? root.hoverColor : root.buttonColor))
          border.width: 1
          border.color: selected ? "#5e8dbb" : root.borderColor

          Behavior on color { ColorAnimation { duration: 90 } }

          Text {
            anchors.centerIn: parent
            text: root.srgbDisplay ? "sRGB" : "Linear"
            color: root.textColor
            font.family: "Segoe UI"
            font.pixelSize: root.compact ? 10 : 11
            font.weight: Font.Medium
          }

          MouseArea {
            id: gammaMouse
            anchors.fill: parent
            enabled: parent.enabled
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: textureViewer.qmlToggleGamma()
          }
        }

        Rectangle {
          width: root.flipWidth
          height: root.buttonHeight
          radius: root.buttonRadius
          property bool selected: root.flipY
          color: selected ? root.selectedColor :
                 (flipMouse.pressed ? root.pressedColor :
                 (flipMouse.containsMouse ? root.hoverColor : root.buttonColor))
          border.width: 1
          border.color: selected ? "#5e8dbb" : root.borderColor

          Behavior on color { ColorAnimation { duration: 90 } }

          Text {
            anchors.centerIn: parent
            text: "Flip Y"
            color: root.textColor
            font.family: "Segoe UI"
            font.pixelSize: root.compact ? 10 : 11
            font.weight: Font.Medium
          }

          MouseArea {
            id: flipMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: textureViewer.qmlToggleFlipY()
          }
        }
      }
    }

    Rectangle {
      width: 1
      height: 42
      color: "#4a5868"
      opacity: 0.8
      anchors.verticalCenter: parent.verticalCenter
    }

    Column {
      spacing: 4

      Text {
        text: "ACTIONS"
        color: root.mutedColor
        font.family: "Segoe UI"
        font.pixelSize: 10
        font.weight: Font.DemiBold
        height: 14
        verticalAlignment: Text.AlignVCenter
      }

      Rectangle {
        id: saveButton
        width: root.saveWidth
        height: root.buttonHeight
        radius: root.buttonRadius
        enabled: root.saveAvailable
        opacity: enabled ? 1.0 : 0.45
        color: saveMouse.pressed ? root.pressedColor :
               (saveMouse.containsMouse ? root.hoverColor : root.buttonColor)
        border.width: 1
        border.color: root.borderColor

        Behavior on color { ColorAnimation { duration: 90 } }

        Row {
          anchors.centerIn: parent
          spacing: 7

          Item {
            width: 18
            height: 18
            anchors.verticalCenter: parent.verticalCenter

            Rectangle {
              x: 2
              y: 1
              width: 14
              height: 16
              radius: 2
              color: "transparent"
              border.width: 1.5
              border.color: root.textColor
            }
            Rectangle {
              x: 5
              y: 2
              width: 7
              height: 4
              radius: 1
              color: root.textColor
            }
            Rectangle {
              x: 5
              y: 11
              width: 8
              height: 5
              radius: 1
              color: "transparent"
              border.width: 1.4
              border.color: root.textColor
            }
          }

          Text {
            text: "Save"
            color: root.textColor
            font.family: "Segoe UI"
            font.pixelSize: root.compact ? 10 : 11
            font.weight: Font.Medium
            anchors.verticalCenter: parent.verticalCenter
          }
        }

        MouseArea {
          id: saveMouse
          anchors.fill: parent
          enabled: saveButton.enabled
          hoverEnabled: true
          cursorShape: Qt.PointingHandCursor
          onClicked: textureViewer.qmlSaveCurrentTexture()
        }
      }
    }
  }
  }

  Rectangle {
    id: horizontalScroll
    anchors.left: parent.left
    anchors.leftMargin: root.horizontalPadding
    anchors.right: parent.right
    anchors.rightMargin: root.horizontalPadding + root.closeButtonSpace
    anchors.bottom: parent.bottom
    anchors.bottomMargin: 5
    height: root.scrollbarHeight
    radius: height / 2
    color: root.buttonColor
    visible: controlsViewport.contentWidth > controlsViewport.width
    z: 2

    Rectangle {
      id: horizontalHandle
      width: Math.max(24, horizontalScroll.width * controlsViewport.width /
                             Math.max(controlsViewport.contentWidth, controlsViewport.width))
      height: parent.height
      x: controlsViewport.contentWidth > controlsViewport.width ?
           (parent.width - width) * controlsViewport.contentX /
               (controlsViewport.contentWidth - controlsViewport.width) : 0
      radius: height / 2
      color: scrollMouse.pressed ? root.pressedColor : root.borderColor

      Behavior on color { ColorAnimation { duration: 90 } }
    }

    MouseArea {
      id: scrollMouse
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.SizeHorCursor

      function updateScroll(position) {
        var travel = horizontalScroll.width - horizontalHandle.width
        var maximum = controlsViewport.contentWidth - controlsViewport.width
        if(travel > 0 && maximum > 0)
          controlsViewport.contentX = Math.max(0, Math.min(maximum,
              (position - horizontalHandle.width / 2) / travel * maximum))
      }

      onPressed: updateScroll(mouse.x)
      onPositionChanged: if(pressed) updateScroll(mouse.x)
    }
  }

  Rectangle {
    id: closeButton
    anchors.right: parent.right
    anchors.rightMargin: 5
    anchors.top: parent.top
    anchors.topMargin: 1
    width: 28
    height: 28
    radius: 14
    color: closeMouse.pressed ? root.pressedColor :
           (closeMouse.containsMouse ? root.hoverColor : "transparent")
    border.width: 0
    z: 2

    Behavior on color { ColorAnimation { duration: 90 } }

    Image {
      anchors.centerIn: parent
      width: 19
      height: 19
      source: "image://renderdoc_panel/close"
      sourceSize.width: 19
      sourceSize.height: 19
      smooth: false
    }

    MouseArea {
      id: closeMouse
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onClicked: textureViewer.qmlCloseToolbar()
    }
  }
}
