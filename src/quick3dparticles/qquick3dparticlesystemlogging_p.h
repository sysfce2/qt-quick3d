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

#ifndef QQUICK3DPARTICLESYSTEMLOGGING_H
#define QQUICK3DPARTICLESYSTEMLOGGING_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QObject>
#include <QQmlEngine>

#include <QtQuick3DParticles/qtquick3dparticlesglobal.h>

QT_BEGIN_NAMESPACE

class Q_QUICK3DPARTICLES_EXPORT QQuick3DParticleSystemLogging : public QObject
{
    Q_OBJECT
    // The frequency in ms how often logging data is updated.
    // Default value 1000.
    Q_PROPERTY(int loggingInterval READ loggingInterval WRITE setLoggingInterval NOTIFY loggingIntervalChanged)
    // How many times particlesystem was updated since last refresh.
    // This is basically fps when updating logs once per second.
    Q_PROPERTY(int updates READ updates NOTIFY updatesChanged)
    // How many particles have been allocated.
    Q_PROPERTY(int particlesMax READ particlesMax NOTIFY particlesMaxChanged)
    // How many particles are currently used / visible.
    // If this value doesn't ever reach particlesMax, consider allocating less particles.
    Q_PROPERTY(int particlesUsed READ particlesUsed NOTIFY particlesUsedChanged)
    // Time in ms used for emitting and animating particles.
    Q_PROPERTY(float time READ time NOTIFY timeChanged)
    // Longer time average of total time used for emitting & animating particles.
    Q_PROPERTY(float timeAverage READ timeAverage NOTIFY timeAverageChanged)
    QML_NAMED_ELEMENT(ParticleSystem3DLogging)
    QML_UNCREATABLE("ParticleSystem3DLogging is abstract")

public:
    QQuick3DParticleSystemLogging(QObject *parent = nullptr);

    int loggingInterval() const;
    int updates() const;
    int particlesMax() const;
    int particlesUsed() const;
    float time() const;
    float timeAverage() const;

public Q_SLOTS:
    void setLoggingInterval(int interval);

Q_SIGNALS:
    void loggingIntervalChanged();
    void updatesChanged();
    void particlesMaxChanged();
    void particlesUsedChanged();
    void timeChanged();
    void timeAverageChanged();

private:
    void updateTimes(qint64 time);
    void resetData();

private:
    friend class QQuick3DParticleSystem;
    int m_loggingInterval = 1000;
    int m_updates = 0;
    int m_particlesMax = 0;
    int m_particlesUsed = 0;
    float m_time = 0.0f;
    float m_timeAverage = 0.0f;
    QList<float> m_totalTimesList;
};

QT_END_NAMESPACE

#endif // QQUICK3DPARTICLESYSTEMLOGGING_H
