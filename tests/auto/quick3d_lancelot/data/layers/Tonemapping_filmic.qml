// Copyright (C) 2020 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR BSD-3-Clause

import QtQuick
import QtQuick3D

Rectangle {
    id: lightProbe
    width: 800
    height: 480
    color: Qt.rgba(1, 1, 1, 1)

    View3D {
        id: layer
        anchors.fill: parent
        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.SkyBox
            lightProbe: layer_lightprobe
            tonemapMode: SceneEnvironment.TonemapModeFilmic
        }

        PerspectiveCamera {
            id: camera
            position.z: 600
        }

        DirectionalLight {
        }

        TonemappingTestScene {
        }

        Texture {
            id: layer_lightprobe
            source: "../shared/maps/TestEnvironment-512.hdr"
            mappingMode: Texture.LightProbe
            tilingModeHorizontal: Texture.Repeat
        }
    }
}
