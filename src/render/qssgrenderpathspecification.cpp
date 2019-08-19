/****************************************************************************
**
** Copyright (C) 2015 NVIDIA Corporation.
** Copyright (C) 2019 The Qt Company Ltd.
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

#include "qssgrenderpathspecification_p.h"
#include <QtQuick3DRender/private/qssgrenderbasetypes_p.h>
#include <QtQuick3DRender/private/qssgrendercontext_p.h>

QT_BEGIN_NAMESPACE

QSSGRenderPathSpecification::QSSGRenderPathSpecification(const QSSGRef<QSSGRenderContext> &context)
    : m_context(context), m_backend(context->backend())
{
}

QSSGRenderPathSpecification::~QSSGRenderPathSpecification() = default;

void QSSGRenderPathSpecification::reset()
{
    m_pathCommands.clear();
    m_pathCoords.clear();
}

void QSSGRenderPathSpecification::addPoint(QVector2D inData)
{
    m_pathCoords.push_back(inData.x());
    m_pathCoords.push_back(inData.y());
}

void QSSGRenderPathSpecification::moveTo(QVector2D inPoint)
{
    m_pathCommands.push_back(static_cast<uchar>(QSSGRenderPathCommands::MoveTo));
    addPoint(inPoint);
}

void QSSGRenderPathSpecification::cubicCurveTo(QVector2D inC1, QVector2D inC2, QVector2D inDest)
{
    m_pathCommands.push_back(static_cast<uchar>(QSSGRenderPathCommands::CubicCurveTo));
    addPoint(inC1);
    addPoint(inC2);
    addPoint(inDest);
}

void QSSGRenderPathSpecification::closePath()
{
    m_pathCommands.push_back(static_cast<uchar>(QSSGRenderPathCommands::Close));
}

QSSGRef<QSSGRenderPathSpecification> QSSGRenderPathSpecification::createPathSpecification(const QSSGRef<QSSGRenderContext> &context)
{
    Q_ASSERT(context->supportsPathRendering());

    return QSSGRef<QSSGRenderPathSpecification>(new QSSGRenderPathSpecification(context));
}
QT_END_NAMESPACE
