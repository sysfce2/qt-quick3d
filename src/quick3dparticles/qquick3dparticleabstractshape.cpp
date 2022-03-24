/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
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

#include "qquick3dparticleabstractshape_p.h"
#include <QtQuick3D/private/qquick3dnode_p.h>

QT_BEGIN_NAMESPACE

/*!
    \qmltype ParticleAbstractShape3D
    \inherits QtObject
    \inqmlmodule QtQuick3D.Particles3D
    \brief Abstract base type of particle shapes.
    \since 6.2

    The ParticleAbstractShape3D is an abstract base type of shapes like \l ParticleShape3D
    and \l ParticleModelShape3D. Shapes can be used to provide start and end positions
    for the particles.
*/
QQuick3DParticleAbstractShape::QQuick3DParticleAbstractShape(QObject *parent)
    : QObject(parent)
{
}

void QQuick3DParticleAbstractShape::componentComplete()
{
    if (!parentNode())
        qWarning() << "Shape requires parent Node to function correctly!";
}

QQuick3DNode *QQuick3DParticleAbstractShape::parentNode()
{
    QQuick3DNode *node = qobject_cast<QQuick3DNode *>(parent());
    if (!m_parentNode || m_parentNode != node)
        m_parentNode = node;
    return m_parentNode;
}

QT_END_NAMESPACE
