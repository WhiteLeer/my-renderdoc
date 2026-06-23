# Windows build

To build with PySide2 support or SSL support in Qt download [this zip](https://renderdoc.org/qrenderdoc_3rdparty.zip) and unzip to this folder (the 3rdparty/ in the zip should go into the 3rdparty/ folder here) before building.

## QML texture viewer UI

The custom Texture Viewer controls are compiled only when `RENDERDOC_QML_UI=1` is
set. The repository contains the QML and C++ source, but does not vendor the Qt
QML/Qt Quick SDK. Use a matching Qt 5 x64 prefix containing `include/QtQml`,
`include/QtQuick`, `lib/Qt5Qml.lib`, `lib/Qt5Quick.lib`, and the corresponding
runtime DLLs and QML plugins.

From the repository root, the supported Windows build wrapper is:

```powershell
.\util\buildscripts\scripts\compile_qml_win.ps1 `
  -QtPrefix64 'D:\path\to\qt-merged-x64'
```

The wrapper passes `RENDERDOC_QML_UI=1` and copies the Qt QML runtime files into
the Development output directory. A normal build without this option keeps the
original Qt Widgets-only UI.
