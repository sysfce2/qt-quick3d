/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Quick 3D.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

import QtQuick 2.15
import HelperWidgets 2.0
import QtQuick.Layouts 1.12

Column {
    width: parent.width

    Section {
        caption: qsTr("Environment Map")
        width: parent.width

        SectionLayout {
            Label {
                text: qsTr("Enabled")
                tooltip: qsTr("Specifies if the environment map is enabled.")
            }
            SecondColumnLayout {
                CheckBox {
                    text: backendValues.uEnvironmentMappingEnabled.valueToString
                    backendValue: backendValues.uEnvironmentMappingEnabled
                    Layout.fillWidth: true
                }
            }

// TODO: Proper support for TextureInput properties
//            Label {
//                text: qsTr("Texture")
//                tooltip: qsTr("Defines a texture for environment map.")
//            }
//            SecondColumnLayout {
//                IdComboBox {
//                    typeFilter: "QtQuick3D.Texture"
//                    Layout.fillWidth: true
//                    backendValue: backendValues.uEnvironmentTexture.texture
//                }
//            }
        }
    }

    Section {
        caption: qsTr("Shadow Map")
        width: parent.width

        SectionLayout {
            Label {
                text: qsTr("Enabled")
                tooltip: qsTr("Specifies if the shadow map is enabled.")
            }
            SecondColumnLayout {
                CheckBox {
                    text: backendValues.uShadowMappingEnabled.valueToString
                    backendValue: backendValues.uShadowMappingEnabled
                    Layout.fillWidth: true
                }
            }

// TODO: Proper support for TextureInput properties
//            Label {
//                text: qsTr("Texture")
//                tooltip: qsTr("Defines a texture for shadow map.")
//            }
//            SecondColumnLayout {
//                IdComboBox {
//                    typeFilter: "QtQuick3D.Texture"
//                    Layout.fillWidth: true
//                    backendValue: backendValues.uBakedShadowTexture.texture
//                }
//            }
        }
    }

    Section {
        caption: qsTr("Base Color")
        width: parent.width
        ColorEditor {
            caption: qsTr("Base Color")
            backendValue: backendValues.base_color
            supportGradient: false
            isVector3D: true
            Layout.fillWidth: true
        }
    }

    Section {
        caption: qsTr("General")
        width: parent.width
        SectionLayout {
            Label {
                text: qsTr("Roughness")
                tooltip: qsTr("Set the material roughness.")
            }
            SecondColumnLayout {
                SpinBox {
                    maximumValue: 1
                    minimumValue: 0
                    decimals: 2
                    backendValue: backendValues.roughness
                    Layout.fillWidth: true
                }
            }
        }
    }
}
