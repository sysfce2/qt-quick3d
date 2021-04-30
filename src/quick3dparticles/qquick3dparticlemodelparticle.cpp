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

#include "qquick3dparticlemodelparticle_p.h"

QT_BEGIN_NAMESPACE

/*!
    \qmltype ModelParticle3D
    \inherits Particle3D
    \inqmlmodule QtQuick3D.Particles3D
    \brief Particle using a Qt Quick 3D Model.
    \since 6.2

    The ModelParticle3D is a logical particle element that creates particles
    from a Qt Quick 3D \l Model component.
*/

QQuick3DParticleModelParticle::QQuick3DParticleModelParticle(QQuick3DNode *parent)
    : QQuick3DParticle(parent)
    , m_initialScale(1.0f, 1.0f, 1.0f)
{
    QObject::connect(this, &QQuick3DParticle::maxAmountChanged, [this]() {
        handleMaxAmountChanged(m_maxAmount);
    });
    QObject::connect(this, &QQuick3DParticle::sortModeChanged, [this]() {
        handleSortModeChanged(sortMode());
    });
}

void QQuick3DParticleModelParticle::handleMaxAmountChanged(int amount)
{
    if (m_particleData.size() == amount)
        return;

    m_particleData.resize(amount);
    m_particleData.fill({});
}

/*!
    \qmlproperty Component ModelParticle3D::delegate

    The delegate provides a template defining each object instantiated by the particle.

    For example, to allocate 200 red cube particles:

    \qml
    Component {
        id: particleComponent
        Model {
            source: "#Cube"
            scale: Qt.vector3d(0.2, 0.2, 0.2)
            materials: DefaultMaterial { }
        }
    }

    ModelParticle3D {
        id: particleRed
        delegate: particleComponent
        maxAmount: 200
        color: "#ff0000"
    }
    \endqml
*/
QQmlComponent *QQuick3DParticleModelParticle::delegate() const
{
    return m_delegate.data();
}

void QQuick3DParticleModelParticle::setDelegate(QQmlComponent *delegate)
{
    if (delegate == m_delegate)
        return;
    m_delegate = delegate;

    regenerate();
    Q_EMIT delegateChanged();
}

class QQuick3DParticleInstanceTable : public QQuick3DInstancing
{
    struct SortData
    {
        float age;
        int index;
    };

public:
    QQuick3DParticleInstanceTable() {}
    void clear() { m_instances.clear(); m_sortData.clear(); }
    void commit() { sort(); markDirty(); }
    void addInstance(const QVector3D &position,
                     const QVector3D &scale,
                     const QVector3D &eulerRotation,
                     const QColor &color,
                     float age) {
        auto entry = calculateTableEntry(position, scale, eulerRotation, color);
        m_instances.append(reinterpret_cast<char *>(&entry), sizeof(InstanceTableEntry));
        if (m_ageSorting)
            m_sortData.append({age, int(m_instances.size() / sizeof(InstanceTableEntry))});
    }
    void setSorting(bool enable, bool inverted = false)
    {
        m_ageSorting = enable;
        m_inverted = inverted;
    }
protected:
    QByteArray getInstanceBuffer(int *instanceCount) override
    {
        if (instanceCount)
            *instanceCount = int(m_instances.count() / sizeof(InstanceTableEntry));

        if (!m_ageSorting)
            return m_instances;
        return m_sortedInstances;
    }
    void sort()
    {
        if (!m_ageSorting)
            return;

        if (m_inverted) {
            std::sort(m_sortData.begin(), m_sortData.end(), [&](const SortData &a, const SortData &b) {
                return a.age < b.age;
            });
        } else {
            std::sort(m_sortData.begin(), m_sortData.end(), [&](const SortData &a, const SortData &b) {
                return a.age > b.age;
            });
        }
        m_sortedInstances.resize(m_instances.size());
        const InstanceTableEntry *src = reinterpret_cast<InstanceTableEntry *>(m_instances.data());
        InstanceTableEntry *dst = reinterpret_cast<InstanceTableEntry *>(m_sortedInstances.data());
        for (auto &e : m_sortData)
            *dst++ = src[e.index];
    }

private:
    QList<SortData> m_sortData;
    QByteArray m_instances;
    QByteArray m_sortedInstances;
    bool m_ageSorting = false;
    bool m_inverted = false;
};

void QQuick3DParticleModelParticle::handleSortModeChanged(QQuick3DParticle::SortMode mode)
{
    if (m_instanceTable) {
        if (mode == QQuick3DParticle::SortNewest || mode == QQuick3DParticle::SortOldest)
            m_instanceTable->setSorting(true, mode == QQuick3DParticle::SortNewest);
        else
            m_instanceTable->setSorting(false);
        m_instanceTable->setDepthSorting(mode == QQuick3DParticle::SortDistance);
    }
}

QQuick3DInstancing *QQuick3DParticleModelParticle::instanceTable() const
{
    return m_instanceTable;
}

void QQuick3DParticleModelParticle::clearInstanceTable()
{
    if (m_instanceTable)
        m_instanceTable->clear();
}

void QQuick3DParticleModelParticle::addInstance(const QVector3D &position, const QVector3D &scale, const QVector3D &eulerRotation, const QColor &color, float age)
{
    if (m_instanceTable)
        m_instanceTable->addInstance(position, scale, eulerRotation, color, age);
}

void QQuick3DParticleModelParticle::commitInstance()
{
    if (m_instanceTable) {
        m_instanceTable->setHasTransparency(hasTransparency());
        m_instanceTable->commit();
    }
}

static void setInstancing(QQuick3DNode *node, QQuick3DInstancing *instanceTable, float bias)
{
    auto *asModel = qobject_cast<QQuick3DModel *>(node);
    if (asModel) {
        asModel->setInstancing(instanceTable);
        asModel->setDepthBias(bias);
    }
    const auto children = node->childItems();
    for (auto *child : children) {
        auto *childNode = qobject_cast<QQuick3DNode *>(child);
        if (childNode)
            setInstancing(childNode, instanceTable, bias);
    }
}

void QQuick3DParticleModelParticle::updateDepthBias(float bias)
{
    setInstancing(m_node, m_instanceTable, bias);
}

void QQuick3DParticleModelParticle::regenerate()
{
    delete m_node;
    m_node = nullptr;

    if (!isComponentComplete())
        return;

    if (!m_instanceTable) {
        m_instanceTable = new QQuick3DParticleInstanceTable();
        m_instanceTable->setParent(this);
        m_instanceTable->setParentItem(this);
        emit instanceTableChanged();
    } else {
        m_instanceTable->clear();
    }

    if (m_delegate.isNull())
        return;

    auto *obj = m_delegate->create(m_delegate->creationContext());

    m_node = qobject_cast<QQuick3DNode *>(obj);
    if (m_node) {
        setInstancing(m_node, m_instanceTable, depthBias());
        auto *particleSystem = system();
        m_node->setParent(particleSystem);
        m_node->setParentItem(particleSystem);
    } else {
        delete obj;
    }
}

void QQuick3DParticleModelParticle::componentComplete()
{
    if (!system() && qobject_cast<QQuick3DParticleSystem *>(parentItem()))
        setSystem(qobject_cast<QQuick3DParticleSystem *>(parentItem()));

    QQuick3DParticle::componentComplete();
    regenerate();
}

void QQuick3DParticleModelParticle::itemChange(QQuick3DObject::ItemChange change, const QQuick3DObject::ItemChangeData &value)
{
    QQuick3DObject::itemChange(change, value);
    if (change == ItemParentHasChanged)
        regenerate();
}

QT_END_NAMESPACE
