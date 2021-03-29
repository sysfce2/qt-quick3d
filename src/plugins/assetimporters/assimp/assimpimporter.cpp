/****************************************************************************
**
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

#include "assimpimporter.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/Logger.hpp>
#include <assimp/DefaultLogger.hpp>
#include <assimp/postprocess.h>
#include <assimp/pbrmaterial.h>
#include <assimp/importerdesc.h>

#include <QtQuick3DAssetImport/private/qssgmesh_p.h>

#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtGui/QQuaternion>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QEasingCurve>

#include <qmath.h>

#include <algorithm>
#include <limits>

QT_BEGIN_NAMESPACE

static const char *getShortFilename(const char *filename)
{
    const char *lastSlash = strrchr(filename, '/');
    if (!lastSlash)
        lastSlash = strrchr(filename, '\\');
    return lastSlash ? lastSlash + 1 : filename;
}

static int textureNameToInt(const char *filename)
{
    if (!filename || *filename == '\0' || *filename != '*')
        return -1;
    bool ok = false;
    const uint number = QVariant(filename + 1).toUInt(&ok);
    return ok ? int(number) : -1;
}

// Returns the full path of the texture file in the 'maps' directory
static QString getEmbeddedTexturePath(const char *filename, QString fallback)
{
    QString imageName;

    const int asInt = textureNameToInt(filename);
    if (asInt != -1) {
        // Embedded texture is a number of the form '*123'
        imageName = QString::number(asInt);
    } else if (filename && *filename != '\0') {
        // Embedded texture is a filename, strip the stem
        imageName = getShortFilename(filename);
    } else {
        // Not a number nor a filename
        imageName = fallback;
    }

    return QStringLiteral("maps/") + imageName + QStringLiteral(".png");
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// QTextStream functions are moved to a namespace in Qt6
using Qt::endl;
#endif

#define demonPostProcessPresets ( \
    aiProcess_CalcTangentSpace              |  \
    aiProcess_GenSmoothNormals              |  \
    aiProcess_JoinIdenticalVertices         |  \
    aiProcess_ImproveCacheLocality          |  \
    aiProcess_RemoveRedundantMaterials      |  \
    aiProcess_SplitLargeMeshes              |  \
    aiProcess_Triangulate                   |  \
    aiProcess_GenUVCoords                   |  \
    aiProcess_SortByPType                   |  \
    aiProcess_FindDegenerates               |  \
    aiProcess_FindInvalidData               |  \
    0 )

AssimpImporter::AssimpImporter()
{
    QFile optionFile(":/assimpimporter/options.json");
    optionFile.open(QIODevice::ReadOnly);
    QByteArray options = optionFile.readAll();
    optionFile.close();
    auto optionsDocument = QJsonDocument::fromJson(options);
    m_options = optionsDocument.object().toVariantMap();
    m_postProcessSteps = aiPostProcessSteps(demonPostProcessPresets);

    m_importer = new Assimp::Importer();
    // Remove primatives that are not Triangles
    m_importer->SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
}

AssimpImporter::~AssimpImporter()
{
    for (auto *animation : m_animations)
        delete animation;
    delete m_importer;
}

const QString AssimpImporter::name() const
{
    return QStringLiteral("assimp");
}

const QStringList AssimpImporter::inputExtensions() const
{
    QStringList extensions;
    extensions.append(QStringLiteral("fbx"));
    extensions.append(QStringLiteral("dae"));
    extensions.append(QStringLiteral("obj"));
    extensions.append(QStringLiteral("gltf"));
    extensions.append(QStringLiteral("glb"));
    extensions.append(QStringLiteral("stl"));
    return extensions;
}

const QString AssimpImporter::outputExtension() const
{
    return QStringLiteral(".qml");
}

const QString AssimpImporter::type() const
{
    return QStringLiteral("Scene");
}

const QString AssimpImporter::typeDescription() const
{
    return QObject::tr("3D Scene");
}

const QVariantMap AssimpImporter::importOptions() const
{
    return m_options;
}
namespace {
bool fuzzyCompare(const aiVector3D &v1, const aiVector3D &v2)
{
    return qFuzzyCompare(v1.x, v2.x) && qFuzzyCompare(v1.y, v2.y)
        && qFuzzyCompare(v1.z, v2.z);
}

bool fuzzyCompare(const aiQuaternion &q1, const aiQuaternion &q2)
{
    return qFuzzyCompare(q1.x, q2.x) && qFuzzyCompare(q1.y, q2.y)
        && qFuzzyCompare(q1.z, q2.z) && qFuzzyCompare(q1.w, q2.w);
}
}

const QString AssimpImporter::import(const QString &sourceFile, const QDir &savePath, const QVariantMap &options, QStringList *generatedFiles)
{
    Q_UNUSED(options);

    QString errorString;
    m_savePath = savePath;
    m_sourceFile = QFileInfo(sourceFile);

    // Create savePath if it doesn't exist already
    m_savePath.mkdir(".");

    // There is special handling needed for GLTF assets
    const auto extension = m_sourceFile.suffix().toLower();
    if (extension == QStringLiteral("gltf") || extension == QStringLiteral("glb")) {
        // assimp bug #3009
        // Currently meshOffsets are not cleared for GLTF files
        // If a GLTF file is imported, we just reset the importer before reading a new gltf file
        if (m_gltfUsed) { // it means that one of previous imported files is gltf format
            for (auto *animation : m_animations)
                delete animation;
            m_animations.clear();
            m_cameras.clear();
            m_lights.clear();
            m_uniqueIds.clear();
            m_nodeIdMap.clear();
            m_nodeTypeMap.clear();
            delete m_importer;
            m_scene = nullptr;
            m_importer = new Assimp::Importer();
            // Remove primatives that are not Triangles
            m_importer->SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
            m_gltfUsed = false;
        } else {
            m_gltfUsed = true;
        }
        m_gltfMode = true;
    } else {
        m_gltfMode = false;
    }

    processOptions(options);

    m_scene = m_importer->ReadFile(sourceFile.toStdString(), m_postProcessSteps);
    if (!m_scene) {
        // Scene failed to load, use logger to get the reason
        return QString::fromLocal8Bit(m_importer->GetErrorString());
    }

    if (m_gltfMode) {
        // gltf 1.x version's material will use DefaultMaterial
        int impIndex = m_importer->GetPropertyInteger("importerIndex");
        const aiImporterDesc *impInfo = m_importer->GetImporterInfo(impIndex);

        // It's a very tricky method but pretty simple.
        // The name must be either "glTF Importer" or "glTF2 Importer"
        if (impInfo->mName[4] != '2')
            m_gltfMode = false;
    }

    // Generate Embedded Texture Sources
    if (m_scene->mNumTextures)
        m_savePath.mkdir(QStringLiteral("./maps"));
    for (uint i = 0; i < m_scene->mNumTextures; ++i) {
        aiTexture *texture = m_scene->mTextures[i];
        QImage image;

        // Note: if texture->mFilename is not set the texture could still be referenced by its index so we use that as a fallback.
        const QString imagePath = getEmbeddedTexturePath(texture->mFilename.C_Str(), QString::number(i));

        if (texture->mHeight == 0) {
            // compressed format, try to load with Image Loader
            QByteArray data(reinterpret_cast<char *>(texture->pcData), texture->mWidth);
            QBuffer readBuffer(&data);
            QByteArray format = texture->achFormatHint;
            QImageReader imageReader(&readBuffer, format);
            image = imageReader.read();
            if (image.isNull()) {
                qWarning() << imageReader.errorString();
                continue;
            }
        } else {
            // Raw format, just convert data to QImage
            image = QImage(reinterpret_cast<uchar *>(texture->pcData), texture->mWidth, texture->mHeight, QImage::Format_RGBA8888);
        }

        const QString saveFileName = savePath.absolutePath() + "/" + imagePath;
        image.save(saveFileName);

        // Note: We need to store both the index and the imagePath since the texture can be referenced later either by index or by filename.
        m_embeddedTextureSources.insert(i, imagePath);
        if (generatedFiles)
            generatedFiles->push_back(saveFileName);
    }

    // Check for Cameras
    if (m_scene->HasCameras()) {
        for (uint i = 0; i < m_scene->mNumCameras; ++i) {
            aiCamera *camera = m_scene->mCameras[i];
            aiNode *node = m_scene->mRootNode->FindNode(camera->mName);
            if (camera && node)
                m_cameras.insert(node, camera);
        }
    }

    // Check for Lights
    if (m_scene->HasLights()) {
        for (uint i = 0; i < m_scene->mNumLights; ++i) {
            aiLight *light = m_scene->mLights[i];
            aiNode *node = m_scene->mRootNode->FindNode(light->mName);
            if (light && node)
                m_lights.insert(node, light);
        }
    }

    // Check for Bones
    if (m_scene->HasMeshes()) {
        for (uint i = 0; i < m_scene->mNumMeshes; ++i) {
            aiMesh *mesh = m_scene->mMeshes[i];
            if (mesh->HasBones()) {
                for (uint j = 0; j < mesh->mNumBones; ++j) {
                    aiBone *bone = mesh->mBones[j];
                    Q_ASSERT(bone);
                    aiNode *node = m_scene->mRootNode->FindNode(bone->mName);
                    if (node) {
                        QString boneName = QString::fromUtf8(bone->mName.C_Str());
                        m_bones.insert(boneName, node);
                    }
                }
            }
        }
        // make m_skeletonIdxMap
        for (uint i = 0; i < m_scene->mNumMeshes; ++i) {
            aiMesh *mesh = m_scene->mMeshes[i];
            if (mesh->HasBones()) {
                aiBone *bone = mesh->mBones[0];
                Q_ASSERT(bone);
                aiNode *node = m_scene->mRootNode->FindNode(bone->mName);
                if (m_skeletonIdxMap.contains(node))
                    continue;

                aiNode *boneRootNode = node->mParent;
                while (isBone(boneRootNode))
                    boneRootNode = boneRootNode->mParent;

                QString id = generateUniqueId(QSSGQmlUtilities::sanitizeQmlId(QStringLiteral("qmlskeleton")));
                quint32 skeletonIdx = m_skeletonIds.size();
                m_skeletonIds.append(id);
                quint32 numBones = 0;

                for (uint j = 0; j < boneRootNode->mNumChildren; ++j) {
                    aiNode *cNode = boneRootNode->mChildren[j];
                    // assumes that all the Joints have children which are Joints
                    if (!isBone(cNode)) {
                        QString boneName = QString::fromUtf8(cNode->mName.C_Str());
                        m_bones.insert(boneName, cNode);
                    }
                    generateSkeletonIdxMap(cNode, skeletonIdx, numBones);
                }
                m_numBonesInSkeleton.append(numBones);
            }
        }
     }

    // Materials

    // Traverse Node Tree

    // Animations (timeline based)
    if (m_scene->HasAnimations()) {
        for (uint i = 0; i < m_scene->mNumAnimations; ++i) {
            aiAnimation *animation = m_scene->mAnimations[i];
            if (!animation)
                continue;
            m_animations.push_back(new QHash<aiNode *, aiNodeAnim *>());
            for (uint j = 0; j < animation->mNumChannels; ++j) {
                aiNodeAnim *channel = animation->mChannels[j];
                aiNode *node = m_scene->mRootNode->FindNode(channel->mNodeName);
                if (channel && node) {
                    // remove redundant animations
                    // assimp generates animation keys with the transformation
                    // of a current node.
                    aiMatrix4x4 transformMatrix = node->mTransformation;
                    aiVector3D scaling;
                    aiQuaternion rotation;
                    aiVector3D translation;
                    if (channel->mNumPositionKeys == 1 ||
                            channel->mNumRotationKeys == 1 ||
                            channel->mNumScalingKeys == 1)
                        transformMatrix.Decompose(scaling, rotation, translation);
                    if (channel->mNumPositionKeys == 1 &&
                            fuzzyCompare(translation, channel->mPositionKeys[0].mValue))
                        channel->mNumPositionKeys = 0;

                    if (channel->mNumRotationKeys == 1 &&
                            fuzzyCompare(rotation, channel->mRotationKeys[0].mValue))
                        channel->mNumRotationKeys = 0;

                    if (channel->mNumScalingKeys == 1 &&
                            fuzzyCompare(scaling, channel->mScalingKeys[0].mValue))
                        channel->mNumScalingKeys = 0;

                    if (channel->mNumPositionKeys == 0 &&
                            channel->mNumRotationKeys == 0 &&
                            channel->mNumScalingKeys == 0)
                        continue;

                    m_animations.back()->insert(node, channel);
                }
            }
            m_morphAnimations.push_back(new QHash<aiNode *, aiMeshMorphAnim *>());
            for (uint j = 0; j < animation->mNumMorphMeshChannels; ++j) {
                aiMeshMorphAnim *channel = animation->mMorphMeshChannels[j];
                aiNode *node = m_scene->mRootNode->FindNode(channel->mName);
                if (channel && node)
                    m_morphAnimations.back()->insert(node, channel);
            }
        }
        // Morph Animations (timeline based)
    }

    // Create QML Component
    QFileInfo sourceFileInfo(sourceFile);

    QString targetFileName = savePath.absolutePath() + QDir::separator() +
            QSSGQmlUtilities::qmlComponentName(sourceFileInfo.completeBaseName()) +
            QStringLiteral(".qml");
    QFile targetFile(targetFileName);
    if (!targetFile.open(QIODevice::WriteOnly)) {
        errorString += QString("Could not write to file: ") + targetFileName;
    } else {
        QTextStream output(&targetFile);

        // Imports header
        writeHeader(output);

        // Component Code
        processNode(m_scene->mRootNode, output);

        targetFile.close();
        if (generatedFiles) {
            *generatedFiles += targetFileName;
            generatedFiles->append(m_generatedFiles);
        }
    }

    return errorString;
}

void AssimpImporter::generateSkeletonIdxMap(aiNode *node, quint32 skeletonIdx, quint32 &boneIdx)
{
    Q_ASSERT(node != nullptr);
    m_skeletonIdxMap.insert(node, skeletonIdx);
    m_nodeTypeMap.insert(node, QSSGQmlUtilities::PropertyMap::Joint);
    QString boneName = QString::fromUtf8(node->mName.C_Str());
    m_boneIdxMap.insert(boneName, boneIdx++);
    for (uint i = 0; i < node->mNumChildren; ++i) {
        aiNode *cNode = node->mChildren[i];
        // assumes that all the Joints have children which are Joints
        if (!isBone(cNode)) {
            QString boneName = QString::fromUtf8(cNode->mName.C_Str());
            m_bones.insert(boneName, cNode);
        }
        generateSkeletonIdxMap(cNode, skeletonIdx, boneIdx);
    }
}

void AssimpImporter::writeHeader(QTextStream &output)
{
    output << "import QtQuick\n";
    output << "import QtQuick3D\n";
    if (m_scene->HasAnimations())
        output << "import QtQuick.Timeline\n";

}

void AssimpImporter::processNode(aiNode *node, QTextStream &output, int tabLevel)
{
    aiNode *currentNode = node;
    if (currentNode) {
        // Figure out what kind of node this is
        if (isModel(currentNode)) {
            // Model
            int numMeshes = currentNode->mNumMeshes;

            // The following code is to merge submeshes
            // but assimp makes submeshes with primitives in GLTF2
            // It means that they could be merged always.
            // After checking it for other formats, then remove
            // this checking processes
            // Now, we will merge submeshes without checking for morphing
            QVector<bool> visited(numMeshes, false);
            const QVector<bool> visitedAll(numMeshes, true);

            while (true) {
                output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("Model {\n");
                generateModelProperties(currentNode, visited, output, tabLevel + 1);
                if (visited == visitedAll)
                    break;
                else
                    output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("}\n");
            }
            m_nodeTypeMap.insert(node, QSSGQmlUtilities::PropertyMap::Model);
        } else if (isLight(currentNode)) {
            // Light
            // Light property name will be produced in the function,
            // and then tabLevel will be increased.
            auto type = generateLightProperties(currentNode, output, tabLevel);
            m_nodeTypeMap.insert(node, type);
        } else if (isCamera(currentNode)) {
            // Camera
            auto type = generateCameraProperties(currentNode, output, tabLevel);
            m_nodeTypeMap.insert(node, type);
        } else if (isBone(currentNode)) {
            if (m_generatedBones.contains(currentNode))
                return;

            quint32 skeletonIdx = m_skeletonIdxMap[currentNode];
            QString id = m_skeletonIds[skeletonIdx];

            output << QSSGQmlUtilities::insertTabs(tabLevel)
                   << QStringLiteral("Skeleton {\n");
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
                   << QStringLiteral("id: ") << id << QStringLiteral("\n");

            generateSkeleton(currentNode->mParent, skeletonIdx, output, tabLevel + 1);
        } else {
            // Transform Node

            // ### Make empty transform node removal optional
            // Check if the node actually does something before generating it
            // and return early without processing the rest of the branch
            if (!containsNodesOfConsequence(node))
                return;

            output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("Node {\n");
            generateNodeProperties(currentNode, output, tabLevel + 1);
            m_nodeTypeMap.insert(node, QSSGQmlUtilities::PropertyMap::Node);
        }

        // Process All Children Nodes
        for (uint i = 0; i < currentNode->mNumChildren; ++i)
            processNode(currentNode->mChildren[i], output, tabLevel + 1);

        if (tabLevel == 0)
            processAnimations(output);

        // Write the QML Footer
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("}\n");
    }
}

void AssimpImporter::generateModelProperties(aiNode *modelNode, QVector<bool> &visited, QTextStream &output, int tabLevel)
{
    generateNodeProperties(modelNode, output, tabLevel);

    // source
    // Combine all the meshes referenced by this model into a single MultiMesh file
    // For the morphing, the target mesh must have the same AnimMeshes.
    // It means if only one mesh has a morphing animation, the other sub-meshes will
    // get null target attributes. However this case might not be common.
    // These submeshes will animate with the same morphing weight!
    QVector<aiMesh *> meshes;
    QVector<aiMaterial *> materials;
    QVector<aiMatrix4x4 *> inverseBindPoses;

    // First, check skinning
    aiBone *bone = nullptr;
    for (uint i = 0; i < modelNode->mNumMeshes; ++i) {
        if (visited[i])
            continue;

        aiMesh *mesh = m_scene->mMeshes[modelNode->mMeshes[i]];
        if (mesh->HasBones()) {
            bone = mesh->mBones[0];
            visited[i] = true;
            meshes.append(mesh);
            aiMaterial *material = m_scene->mMaterials[mesh->mMaterialIndex];
            materials.append(material);
            break;
        }
    }

    // skeletonRoot
    quint32 skeletonIdx = 0xffffffff;
    if (bone != nullptr) {
        QString id;
        QString boneName = QString::fromUtf8(bone->mName.C_Str());
        aiNode *boneNode = m_bones[boneName];
        Q_ASSERT(m_skeletonIdxMap.contains(boneNode));
        skeletonIdx = m_skeletonIdxMap[boneNode];
        id = m_skeletonIds[skeletonIdx];
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("skeleton: ")
               << id << QStringLiteral("\n");

        inverseBindPoses.resize(m_numBonesInSkeleton[skeletonIdx]);
        aiMesh *mesh = meshes[0];
        for (uint i = 0; i < mesh->mNumBones; ++i) {
            QString boneName = QString::fromUtf8(mesh->mBones[i]->mName.C_Str());
            Q_ASSERT(m_boneIdxMap.contains(boneName));
            qint32 boneIndex = m_boneIdxMap[boneName];
            inverseBindPoses[boneIndex] = &(mesh->mBones[i]->mOffsetMatrix);
        }
    }

    for (uint i = 0; i < modelNode->mNumMeshes; ++i) {
        if (visited[i])
            continue;
        aiMesh *mesh = m_scene->mMeshes[modelNode->mMeshes[i]];
        if (mesh->HasBones()) {
            bone = mesh->mBones[0];
            QString boneName = QString::fromUtf8(bone->mName.C_Str());
            aiNode *boneNode = m_bones[boneName];
            Q_ASSERT(m_skeletonIdxMap.contains(boneNode));
            // check this skinned mesh can be merged with previous one
            if (skeletonIdx != m_skeletonIdxMap[boneNode]) {
                // This node will be processed at the next time.
                continue;
            }
            bool canBeMerged = true;
            for (uint i = 0; i < mesh->mNumBones; ++i) {
                QString boneName = QString::fromUtf8(mesh->mBones[i]->mName.C_Str());
                Q_ASSERT(m_boneIdxMap.contains(boneName));
                qint32 boneIndex = m_boneIdxMap[boneName];
                if (inverseBindPoses[boneIndex] != nullptr
                        && *(inverseBindPoses[boneIndex]) != mesh->mBones[i]->mOffsetMatrix) {
                    canBeMerged = false;
                    break;
                }
            }
            if (!canBeMerged)
                continue;

            // Add additional inverseBindPoses
            for (uint i = 0; i < mesh->mNumBones; ++i) {
                QString boneName = QString::fromUtf8(mesh->mBones[i]->mName.C_Str());
                qint32 boneIndex = m_boneIdxMap[boneName];
                inverseBindPoses[boneIndex] = &(mesh->mBones[i]->mOffsetMatrix);
            }
        }
        meshes.append(mesh);
        aiMaterial *material = m_scene->mMaterials[mesh->mMaterialIndex];
        materials.append(material);

        visited[i] = true;
    }

    if (inverseBindPoses.size() > 0) {
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("inverseBindPoses: [\n");
        for (uint i = 0; i < inverseBindPoses.size(); ++i) {
            aiMatrix4x4 *osMat = inverseBindPoses[i];
            if (osMat) {
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
                       << QStringLiteral("Qt.matrix4x4(")
                       << QString("%1, %2, %3, %4, ").arg((*osMat)[0][0]).arg((*osMat)[0][1]).arg((*osMat)[0][2]).arg((*osMat)[0][3])
                       << QString("%1, %2, %3, %4, ").arg((*osMat)[1][0]).arg((*osMat)[1][1]).arg((*osMat)[1][2]).arg((*osMat)[1][3])
                       << QString("%1, %2, %3, %4, ").arg((*osMat)[2][0]).arg((*osMat)[2][1]).arg((*osMat)[2][2]).arg((*osMat)[2][3])
                       << QString("%1, %2, %3, %4)").arg((*osMat)[3][0]).arg((*osMat)[3][1]).arg((*osMat)[3][2]).arg((*osMat)[3][3]);
            } else {
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
                       << QStringLiteral("Qt.matrix4x4()");
            }

            if (i != inverseBindPoses.size() - 1)
                output << ",\n";
            else
                output << "\n" << QSSGQmlUtilities::insertTabs(tabLevel) << "]\n";
        }
    }

    // Model name can contain invalid characters for filename, so just to be safe, convert the name
    // into qml id first.
    QString modelName = QString::fromUtf8(modelNode->mName.C_Str());
    modelName = QSSGQmlUtilities::sanitizeQmlId(modelName);

    QString outputMeshFile = QStringLiteral("meshes/") + modelName + QStringLiteral(".mesh");

    m_savePath.mkdir(QStringLiteral("./meshes"));
    QString meshFilePath = m_savePath.absolutePath() + QLatin1Char('/') + outputMeshFile;
    int index = 0;
    while (m_generatedFiles.contains(meshFilePath)) {
        outputMeshFile = QStringLiteral("meshes/%1_%2.mesh").arg(modelName).arg(++index);
        meshFilePath = m_savePath.absolutePath() + QLatin1Char('/') + outputMeshFile;
    }
    QFile meshFile(meshFilePath);
    const QString errMsg = generateMeshFile(modelNode, meshFile, meshes);
    if (errMsg.isEmpty())
        m_generatedFiles << meshFilePath;
    else
        qWarning("%s", qPrintable(errMsg));
    meshFile.close();

    output << QSSGQmlUtilities::insertTabs(tabLevel) << "source: \"" << outputMeshFile
           << QStringLiteral("\"") << QStringLiteral("\n");

    // Morphing
    const QVector<QString> targets = generateMorphing(modelNode, meshes, output, tabLevel);

    if (targets.size() > 0) {
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("morphTargets: [\n");
        for (int i = 0; i < targets.size() - 1; ++i)
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << targets[i] << QStringLiteral(",\n");
        output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << targets.back() << QStringLiteral("\n");
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("]\n");
    }

    // materials
    // If there are any new materials, add them as children of the Model first
    for (int i = 0; i < materials.count(); ++i) {
        if (!m_materialIdMap.contains(materials[i])) {
            generateMaterial(materials[i], output, tabLevel);
            output << QStringLiteral("\n");
        }
    }

    // For each sub-mesh, generate a material reference for this list
    output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("materials: [\n");
    for (int i = 0; i < materials.count(); ++i) {
        output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << m_materialIdMap[materials[i]];
        if (i < materials.count() - 1)
            output << QStringLiteral(",");
        output << QStringLiteral("\n");
    }

    output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("]\n");
}

QSSGQmlUtilities::PropertyMap::Type AssimpImporter::generateLightProperties(aiNode *lightNode, QTextStream &output, int tabLevel)
{
    aiLight *light = m_lights.value(lightNode);
    // We assume that the direction vector for a light is (0, 0, -1)
    // so if the direction vector is non-null, but not (0, 0, -1) we
    // need to correct the translation
    aiMatrix4x4 correctionMatrix;
    bool needsCorrection = false;
    if (light->mDirection != aiVector3D(0, 0, 0)) {
        if (light->mDirection != aiVector3D(0, 0, -1)) {
            aiMatrix4x4::FromToMatrix(aiVector3D(0, 0, -1), light->mDirection, correctionMatrix);
            needsCorrection = true;
        }
    }


    // lightType
    QSSGQmlUtilities::PropertyMap::Type lightType;
    if (light->mType == aiLightSource_DIRECTIONAL || light->mType == aiLightSource_AMBIENT ) {
        lightType = QSSGQmlUtilities::PropertyMap::DirectionalLight;
        output << QSSGQmlUtilities::insertTabs(tabLevel++) << QStringLiteral("DirectionalLight {\n");
    } else if (light->mType == aiLightSource_POINT) {
        lightType = QSSGQmlUtilities::PropertyMap::PointLight;
        output << QSSGQmlUtilities::insertTabs(tabLevel++) << QStringLiteral("PointLight {\n");
    } else if (light->mType == aiLightSource_SPOT) {
        lightType = QSSGQmlUtilities::PropertyMap::SpotLight;
        output << QSSGQmlUtilities::insertTabs(tabLevel++) << QStringLiteral("SpotLight {\n");
    } else {
        // We dont know what it is, assume its a point light
        lightType = QSSGQmlUtilities::PropertyMap::PointLight;
        output << QSSGQmlUtilities::insertTabs(tabLevel++) << QStringLiteral("PointLight {\n");
    }

    if (needsCorrection)
        generateNodeProperties(lightNode, output, tabLevel, &correctionMatrix, true);
    else
        generateNodeProperties(lightNode, output, tabLevel, nullptr, true);

    // brightness
    // Assimp has no property related to brightness or intensity.
    // They are multiplied to diffuse, ambient and specular colors.
    // For extracting the property value, we will check the maximum value of them.
    // (In most cases, Assimp uses the same specular values with diffuse values,
    // so we will compare just components of the diffuse and the ambient)
    float brightness = qMax(qMax(1.0f, light->mColorDiffuse.r),
                            qMax(light->mColorDiffuse.g, light->mColorDiffuse.b));

    // ambientColor
    if (light->mType == aiLightSource_AMBIENT) {
        brightness = qMax(qMax(brightness, light->mColorAmbient.r),
                          qMax(light->mColorAmbient.g, light->mColorAmbient.b));

        // We only want ambient light color if it is explicit
        QColor ambientColor = QColor::fromRgbF(light->mColorAmbient.r / brightness,
                                               light->mColorAmbient.g / brightness,
                                               light->mColorAmbient.b / brightness);
        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, lightType, QStringLiteral("ambientColor"), ambientColor);
    }

    // diffuseColor
    QColor diffuseColor = QColor::fromRgbF(light->mColorDiffuse.r / brightness,
                                           light->mColorDiffuse.g / brightness,
                                           light->mColorDiffuse.b / brightness);
    QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, lightType, QStringLiteral("color"), diffuseColor);

    // describe brightness here
    QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, lightType, QStringLiteral("brightness"), brightness);

    if (light->mType == aiLightSource_POINT || light->mType == aiLightSource_SPOT) {
        // constantFade
        // Some assets have this constant attenuation value as 0.0f and it makes light attenuation makes infinite at distance 0.
        // In that case, we will use the default constant attenuation, 1.0f.
        if (light->mAttenuationConstant != 0.0f)
            QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, lightType, QStringLiteral("constantFade"), light->mAttenuationConstant);

        // linearFade
        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, lightType, QStringLiteral("linearFade"), light->mAttenuationLinear * 100);

        // exponentialFade
        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, lightType, QStringLiteral("quadraticFade"), light->mAttenuationQuadratic * 10000);

        if (light->mType == aiLightSource_SPOT) {
            // coneAngle
            QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, lightType, QStringLiteral("coneAngle"), qRadiansToDegrees(light->mAngleOuterCone) * 2);

            // innerConeAngle
            QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, lightType, QStringLiteral("innerConeAngle"), qRadiansToDegrees(light->mAngleInnerCone) * 2);
        }
    }
    // castShadow

    // shadowBias

    // shadowFactor

    // shadowMapResolution

    // shadowMapFar

    // shadowMapFieldOfView

    // shadowFilter

    return lightType;
}

QSSGQmlUtilities::PropertyMap::Type AssimpImporter::generateCameraProperties(aiNode *cameraNode, QTextStream &output, int tabLevel)
{
    QSSGQmlUtilities::PropertyMap::Type type;

    aiCamera *camera = m_cameras.value(cameraNode);

    // assimp does not have a camera type but it works for gltf2 format.
    if (camera->mHorizontalFOV == 0.0) {
        type = QSSGQmlUtilities::PropertyMap::OrthographicCamera;
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("OrthographicCamera {\n");
    } else {
        type = QSSGQmlUtilities::PropertyMap::PerspectiveCamera;
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("PerspectiveCamera {\n");
    }

    // We assume these default forward and up vectors, so if this isn't
    // the case we have to do additional transform
    aiMatrix4x4 correctionMatrix;
    bool needsCorrection = false;
    if (camera->mLookAt != aiVector3D(0, 0, -1))
    {
        aiMatrix4x4 lookAtCorrection;
        aiMatrix4x4::FromToMatrix(aiVector3D(0, 0, -1), camera->mLookAt, lookAtCorrection);
        correctionMatrix *= lookAtCorrection;
        needsCorrection = true;
    }
    if (camera->mUp != aiVector3D(0, 1, 0)) {
        aiMatrix4x4 upCorrection;
        aiMatrix4x4::FromToMatrix(aiVector3D(0, 1, 0), camera->mUp, upCorrection);
        correctionMatrix *= upCorrection;
        needsCorrection = true;
    }

    if (needsCorrection)
        generateNodeProperties(cameraNode, output, tabLevel + 1, &correctionMatrix, true);
    else
        generateNodeProperties(cameraNode, output, tabLevel + 1, nullptr, true);

    // clipNear
    QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel + 1, type, QStringLiteral("clipNear"), camera->mClipPlaneNear);

    // clipFar
    QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel + 1, type, QStringLiteral("clipFar"), camera->mClipPlaneFar);

    if (type == QSSGQmlUtilities::PropertyMap::PerspectiveCamera) {
        // fieldOfView
        // mHorizontalFOV is defined as a half horizontal fov
        // in the assimp header but it seems not half now.
        float fov = qRadiansToDegrees(camera->mHorizontalFOV);
        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel + 1, type, QStringLiteral("fieldOfView"), fov);

        // isFieldOfViewHorizontal
        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel + 1, type, QStringLiteral("fieldOfViewOrientation"), "PerspectiveCamera.Horizontal");
    } else { //OrthographicCamera
        float width = camera->mOrthographicWidth * 2;
        float height = width / camera->mAspect;
        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel + 1, type, QStringLiteral("horizontalMagnification"), width);
        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel + 1, type, QStringLiteral("verticalMagnification"), height);
    }
    // projectionMode

    // scaleMode

    // scaleAnchor

    // frustomScaleX

    // frustomScaleY


    return type;
}

void AssimpImporter::generateNodeProperties(aiNode *node, QTextStream &output, int tabLevel, aiMatrix4x4 *transformCorrection, bool skipScaling)
{
    // id
    QString name = QString::fromUtf8(node->mName.C_Str());
    if (!name.isEmpty()) {
        // ### we may need to account of non-unique and empty names
        QString id = generateUniqueId(QSSGQmlUtilities::sanitizeQmlId(name));
        m_nodeIdMap.insert(node, id);
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("id: ") << id << QStringLiteral("\n");
    }

    aiMatrix4x4 transformMatrix = node->mTransformation;

    // Decompose Transform Matrix to get properties
    aiVector3D scaling;
    aiQuaternion rotation;
    aiVector3D translation;
    transformMatrix.Decompose(scaling, rotation, translation);

    // Apply correction if necessary
    // transformCorrection is just for cameras and lights
    // and its factor just contains rotation.
    // In this case, this rotation will replace previous rotation.
    if (transformCorrection) {
        aiVector3D dummyTrans;
        transformCorrection->DecomposeNoScaling(rotation, dummyTrans);
    }

    // translate
    QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, QSSGQmlUtilities::PropertyMap::Node, QStringLiteral("x"), translation.x);
    QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, QSSGQmlUtilities::PropertyMap::Node, QStringLiteral("y"), translation.y);
    QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, QSSGQmlUtilities::PropertyMap::Node, QStringLiteral("z"), translation.z);

    // rotation
    QQuaternion rot(rotation.w, rotation.x, rotation.y, rotation.z);
    QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, QSSGQmlUtilities::PropertyMap::Node, QStringLiteral("rotation"), rot);

    // scale
    if (!skipScaling) {
        // Apply the global scale for a root node
        if (tabLevel == 1)
            scaling *= m_globalScaleValue;

        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, QSSGQmlUtilities::PropertyMap::Node, QStringLiteral("scale.x"), scaling.x);
        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, QSSGQmlUtilities::PropertyMap::Node, QStringLiteral("scale.y"), scaling.y);
        QSSGQmlUtilities::writeQmlPropertyHelper(output, tabLevel, QSSGQmlUtilities::PropertyMap::Node, QStringLiteral("scale.z"), scaling.z);
    }
    // pivot

    // opacity

    // boneid

    // visible

}

QString AssimpImporter::generateMeshFile(aiNode *, QFile &file, const QVector<aiMesh *> &meshes)
{
    if (!file.open(QIODevice::WriteOnly))
        return QStringLiteral("Could not open device to write mesh file");

    // Check if we need placeholders in certain channels
    bool needsPositionData = false;
    bool needsNormalData = false;
    bool needsUV0Data = false;
    bool needsUV1Data = false;
    bool needsTangentData = false;
    bool needsVertexColorData = false;
    unsigned uv0Components = 0;
    unsigned uv1Components = 0;
    unsigned totalVertices = 0;
    bool needsBones = false;

    // GLTF should support at least 8 attributes for morphing.
    // The supported combinations are the followings.
    // 1. 8 targets having only positions.
    // 2. 4 targets having both positions and normals.
    // 3. 2 targets having positions, normals, and tangents(with binormals)
    //
    // 4. 2 targets having only positions and 3 targets having both positions
    //   and normals,
    // 5. ....
    //
    // Handling the same types is simple but let's think about 4.
    // In this case, animMeshes should be sorted by descending order of the
    // number of input attributes. It means that we need to process 3 targets
    // having more attributes first and then 2 remaining targets.
    // However, we will assume the asset is made by this correct order.

    quint32 numMorphTargets = 0;
    QVector<bool> needsTargetPosition;
    QVector<bool> needsTargetNormal;
    QVector<bool> needsTargetTangent;
    QVector<float> targetWeight;

    for (const auto *mesh : meshes) {
        totalVertices += mesh->mNumVertices;
        uv0Components = qMax(mesh->mNumUVComponents[0], uv0Components);
        uv1Components = qMax(mesh->mNumUVComponents[1], uv1Components);
        needsPositionData |= mesh->HasPositions();
        needsNormalData |= mesh->HasNormals();
        needsUV0Data |= mesh->HasTextureCoords(0);
        needsUV1Data |= mesh->HasTextureCoords(1);
        needsTangentData |= mesh->HasTangentsAndBitangents();
        needsVertexColorData |=mesh->HasVertexColors(0);
        needsBones |= mesh->HasBones();
        if (mesh->mNumAnimMeshes && mesh->mAnimMeshes) {
            if (mesh->mNumAnimMeshes > 8)
                qWarning() << "QtQuick3D supports maximum 8 morph targets, remains will be ignored\n";
            const quint32 numAnimMeshes = qMin(8U, mesh->mNumAnimMeshes);
            if (numMorphTargets < numAnimMeshes) {
                numMorphTargets = numAnimMeshes;
                needsTargetPosition.resize(numMorphTargets);
                needsTargetNormal.resize(numMorphTargets);
                needsTargetTangent.resize(numMorphTargets);
                targetWeight.resize(numMorphTargets);
            }
            for (uint i = 0; i < numAnimMeshes; ++i) {
                auto animMesh = mesh->mAnimMeshes[i];
                needsTargetPosition[i] |= animMesh->HasPositions();
                needsTargetNormal[i] |= animMesh->HasNormals();
                needsTargetTangent[i] |= animMesh->HasTangentsAndBitangents();
                targetWeight[i] = animMesh->mWeight;
            }
        }
    }

    QByteArray positionData;
    QByteArray normalData;
    QByteArray uv0Data;
    QByteArray uv1Data;
    QByteArray tangentData;
    QByteArray binormalData;
    QByteArray vertexColorData;
    QByteArray indexBufferData;
    QByteArray boneIndexData;
    QByteArray boneWeightData;
    QByteArray targetPositionData[8];
    QByteArray targetNormalData[8];
    QByteArray targetTangentData[8];
    QByteArray targetBinormalData[8];
    QVector<SubsetEntryData> subsetData;
    quint32 baseIndex = 0;

    // Always use 32-bit indices. Metal has a requirement of 4 byte alignment
    // for index buffer offsets, and we cannot risk hitting that.
    const QSSGMesh::Mesh::ComponentType indexType = QSSGMesh::Mesh::ComponentType::UnsignedInt32;

    const quint32 float32ByteSize = QSSGMesh::MeshInternal::byteSizeForComponentType(QSSGMesh::Mesh::ComponentType::Float32);
    for (const auto *mesh : meshes) {
        // Position
        if (mesh->HasPositions())
            positionData += QByteArray(reinterpret_cast<char*>(mesh->mVertices),
                                       mesh->mNumVertices * 3 * float32ByteSize);
        else if (needsPositionData)
            positionData += QByteArray(mesh->mNumVertices * 3 * float32ByteSize, '\0');

        // Normal
        if (mesh->HasNormals())
            normalData += QByteArray(reinterpret_cast<char*>(mesh->mNormals),
                                     mesh->mNumVertices * 3 * float32ByteSize);
        else if (needsNormalData)
            normalData += QByteArray(mesh->mNumVertices * 3 * float32ByteSize, '\0');

        // UV0
        if (mesh->HasTextureCoords(0)) {
            QVector<float> uvCoords;
            uvCoords.resize(uv0Components * mesh->mNumVertices);
            for (uint i = 0; i < mesh->mNumVertices; ++i) {
                int offset = i * uv0Components;
                aiVector3D *textureCoords = mesh->mTextureCoords[0];
                uvCoords[offset] = textureCoords[i].x;
                uvCoords[offset + 1] = textureCoords[i].y;
                if (uv0Components == 3)
                    uvCoords[offset + 2] = textureCoords[i].z;
            }
            uv0Data += QByteArray(reinterpret_cast<const char*>(uvCoords.constData()), uvCoords.size() * sizeof(float));
        } else {
            uv0Data += QByteArray(mesh->mNumVertices * uv0Components * float32ByteSize, '\0');
        }

        // UV1
        if (mesh->HasTextureCoords(1)) {
            QVector<float> uvCoords;
            uvCoords.resize(uv1Components * mesh->mNumVertices);
            for (uint i = 0; i < mesh->mNumVertices; ++i) {
                int offset = i * uv1Components;
                aiVector3D *textureCoords = mesh->mTextureCoords[1];
                uvCoords[offset] = textureCoords[i].x;
                uvCoords[offset + 1] = textureCoords[i].y;
                if (uv1Components == 3)
                    uvCoords[offset + 2] = textureCoords[i].z;
            }
            uv1Data += QByteArray(reinterpret_cast<const char*>(uvCoords.constData()), uvCoords.size() * sizeof(float));
        } else {
            uv1Data += QByteArray(mesh->mNumVertices * uv1Components * float32ByteSize, '\0');
        }

        if (mesh->HasTangentsAndBitangents()) {
            // Tangents
            tangentData += QByteArray(reinterpret_cast<char*>(mesh->mTangents),
                                      mesh->mNumVertices * 3 * float32ByteSize);
            // Binormals (They are actually supposed to be Bitangents despite what they are called)
            binormalData += QByteArray(reinterpret_cast<char*>(mesh->mBitangents),
                                       mesh->mNumVertices * 3 * float32ByteSize);
        } else if (needsTangentData) {
            tangentData += QByteArray(mesh->mNumVertices * 3 * float32ByteSize, '\0');
            binormalData += QByteArray(mesh->mNumVertices * 3 * float32ByteSize, '\0');
        }

        // ### Bones + Weights
        QVector<qint32> boneIndexes;
        QVector<float> fBoneIndexes;
        QVector<float> weights;
        if (mesh->HasBones()) {
            weights.resize(mesh->mNumVertices * 4, 0.0f);
            if (m_useFloatJointIndices)
                fBoneIndexes.resize(mesh->mNumVertices * 4, 0);
            else
                boneIndexes.resize(mesh->mNumVertices * 4, 0);

            for (uint i = 0; i < mesh->mNumBones; ++i) {
                QString boneName = QString::fromUtf8(mesh->mBones[i]->mName.C_Str());

                const auto boneIdxItr = m_boneIdxMap.constFind(boneName);
                if (boneIdxItr == m_boneIdxMap.cend()) {
                    qWarning() << "Joint " << boneName << " is not included in pre-defined skeleton.";
                    continue;
                }

                for (uint j = 0; j < mesh->mBones[i]->mNumWeights; ++j) {
                    quint32 vertexId = mesh->mBones[i]->mWeights[j].mVertexId;
                    float weight = mesh->mBones[i]->mWeights[j].mWeight;

                    // skip a bone transform having small weight
                    if (weight <= 0.01f)
                        continue;

                    //  if any vertex has more weights than 4, it will be ignored
                    for (uint ii = 0; ii < 4; ++ii) {
                        if (weights[vertexId * 4 + ii] == 0.0f) {
                            if (m_useFloatJointIndices)
                                fBoneIndexes[vertexId * 4 + ii] = (float)*boneIdxItr;
                            else
                                boneIndexes[vertexId * 4 + ii] = *boneIdxItr;
                            weights[vertexId * 4 + ii] = weight;
                            break;
                        } else if (ii == 3) {
                            qWarning("vertexId %d has already 4 weights and index %d's weight %f will be ignored.", vertexId, *boneIdxItr, weight);
                        }
                    }
                }
            }
            // Bone Indexes
            if (m_useFloatJointIndices)
                boneIndexData += QByteArray(reinterpret_cast<const char *>(fBoneIndexes.constData()), fBoneIndexes.size() * sizeof(float));
            else
                boneIndexData += QByteArray(reinterpret_cast<const char *>(boneIndexes.constData()), boneIndexes.size() * sizeof(qint32));
            // Bone Weights
            boneWeightData += QByteArray(reinterpret_cast<const char *>(weights.constData()), weights.size() * sizeof(float));
        } else if (needsBones) {
            // Bone Indexes
            boneIndexData += QByteArray(mesh->mNumVertices * 4 * QSSGMesh::MeshInternal::byteSizeForComponentType(QSSGMesh::Mesh::ComponentType::Int32), '\0');
            // Bone Weights
            boneWeightData += QByteArray(mesh->mNumVertices * 4 * float32ByteSize, '\0');
        }

        // Color
        if (mesh->HasVertexColors(0))
            vertexColorData += QByteArray(reinterpret_cast<char*>(mesh->mColors[0]), mesh->mNumVertices * 4 * float32ByteSize);
        else if (needsVertexColorData)
            vertexColorData += QByteArray(mesh->mNumVertices * 4 * float32ByteSize, '\0');

        for (uint i = 0; i < numMorphTargets; ++i) {
            aiAnimMesh *animMesh = nullptr;
            if (mesh->mNumAnimMeshes > i)
                animMesh = mesh->mAnimMeshes[i];

            if (needsTargetPosition[i]) {
                if (animMesh && animMesh->HasPositions())
                    targetPositionData[i] += QByteArray(reinterpret_cast<char*>(animMesh->mVertices), animMesh->mNumVertices * 3 * float32ByteSize);
                else
                    targetPositionData[i] += QByteArray(animMesh->mNumVertices * 3 * float32ByteSize, '\0');
            }
            if (needsTargetNormal[i]) {
                if (animMesh && animMesh->HasNormals())
                    targetNormalData[i] += QByteArray(reinterpret_cast<char*>(animMesh->mNormals), animMesh->mNumVertices * 3 * float32ByteSize);
                else
                    targetNormalData[i] += QByteArray(animMesh->mNumVertices * 3 * float32ByteSize, '\0');
            }
            if (needsTargetTangent[i]) {
                if (animMesh && animMesh->HasTangentsAndBitangents()) {
                    targetTangentData[i] += QByteArray(reinterpret_cast<char*>(animMesh->mTangents), animMesh->mNumVertices * 3 * float32ByteSize);
                    targetBinormalData[i] += QByteArray(reinterpret_cast<char*>(animMesh->mBitangents), animMesh->mNumVertices * 3 * float32ByteSize);
                } else {
                    targetTangentData[i] += QByteArray(animMesh->mNumVertices * 3 * float32ByteSize, '\0');
                    targetBinormalData[i] += QByteArray(animMesh->mNumVertices * 3 * float32ByteSize, '\0');
                }
            }
        }

        // Index Buffer
        QVector<quint32> indexes;
        indexes.reserve(mesh->mNumFaces * 3);

        for (unsigned int faceIndex = 0;faceIndex < mesh->mNumFaces; ++faceIndex) {
            const auto face = mesh->mFaces[faceIndex];
            // Faces should always have 3 indicides
            Q_ASSERT(face.mNumIndices == 3);
            indexes.append(quint32(face.mIndices[0]) + baseIndex);
            indexes.append(quint32(face.mIndices[1]) + baseIndex);
            indexes.append(quint32(face.mIndices[2]) + baseIndex);
        }
        // Since we might be combining multiple meshes together, we also need to change the index offset
        baseIndex = *std::max_element(indexes.constBegin(), indexes.constEnd()) + 1;

        SubsetEntryData subsetEntry;
        subsetEntry.indexOffset = indexBufferData.length() / QSSGMesh::MeshInternal::byteSizeForComponentType(indexType);
        subsetEntry.indexLength = indexes.length();
        if (indexType == QSSGMesh::Mesh::ComponentType::UnsignedInt32) {
            indexBufferData += QByteArray(reinterpret_cast<const char *>(indexes.constData()),
                                          indexes.length() * QSSGMesh::MeshInternal::byteSizeForComponentType(indexType));
        } else {
            // convert data to quint16
            QVector<quint16> shortIndexes;
            shortIndexes.resize(indexes.length());
            for (int i = 0; i < shortIndexes.length(); ++i)
                shortIndexes[i] = quint16(indexes[i]);
            indexBufferData += QByteArray(reinterpret_cast<const char *>(shortIndexes.constData()),
                                          shortIndexes.length() * QSSGMesh::MeshInternal::byteSizeForComponentType(indexType));
        }

        // Subset
        subsetEntry.name = QString::fromUtf8(m_scene->mMaterials[mesh->mMaterialIndex]->GetName().C_Str());
        subsetData.append(subsetEntry);
    }

    // Vertex Buffer Entries
    QVector<QSSGMesh::AssetVertexEntry> entries;
    if (positionData.length() > 0) {
        entries.append({
                           QSSGMesh::MeshInternal::getPositionAttrName(),
                           positionData,
                           QSSGMesh::Mesh::ComponentType::Float32,
                           3
                       });
    }
    if (normalData.length() > 0) {
        entries.append({
                           QSSGMesh::MeshInternal::getNormalAttrName(),
                           normalData,
                           QSSGMesh::Mesh::ComponentType::Float32,
                           3
                       });
    }
    if (uv0Data.length() > 0) {
        entries.append({
                           QSSGMesh::MeshInternal::getUV0AttrName(),
                           uv0Data,
                           QSSGMesh::Mesh::ComponentType::Float32,
                           uv0Components
                       });
    }
    if (uv1Data.length() > 0) {
        entries.append({
                           QSSGMesh::MeshInternal::getUV1AttrName(),
                           uv1Data,
                           QSSGMesh::Mesh::ComponentType::Float32,
                           uv1Components
                       });
    }

    if (tangentData.length() > 0) {
        entries.append({
                           QSSGMesh::MeshInternal::getTexTanAttrName(),
                           tangentData,
                           QSSGMesh::Mesh::ComponentType::Float32,
                           3
                       });
    }

    if (binormalData.length() > 0) {
        entries.append({
                           QSSGMesh::MeshInternal::getTexBinormalAttrName(),
                           binormalData,
                           QSSGMesh::Mesh::ComponentType::Float32,
                           3
                       });
    }

    if (vertexColorData.length() > 0) {
        entries.append({
                           QSSGMesh::MeshInternal::getColorAttrName(),
                           vertexColorData,
                           QSSGMesh::Mesh::ComponentType::Float32,
                           4
                       });
    }

    if (boneIndexData.length() > 0) {
        entries.append({
                           QSSGMesh::MeshInternal::getJointAttrName(),
                           boneIndexData,
                           QSSGMesh::Mesh::ComponentType::Int32,
                           4
                       });
        entries.append({
                           QSSGMesh::MeshInternal::getWeightAttrName(),
                           boneWeightData,
                           QSSGMesh::Mesh::ComponentType::Float32,
                           4
                       });
    }
    for (uint i = 0; i < numMorphTargets; ++i) {
        if (targetPositionData[i].length() > 0) {
            entries.append({
                               QSSGMesh::MeshInternal::getTargetPositionAttrName(i),
                               targetPositionData[i],
                               QSSGMesh::Mesh::ComponentType::Float32,
                               3
                           });
        }
        if (targetNormalData[i].length() > 0) {
            entries.append({
                               QSSGMesh::MeshInternal::getTargetNormalAttrName(i),
                               targetNormalData[i],
                               QSSGMesh::Mesh::ComponentType::Float32,
                               3
                           });
        }
        if (targetTangentData[i].length() > 0) {
            entries.append({
                               QSSGMesh::MeshInternal::getTargetTangentAttrName(i),
                               targetTangentData[i],
                               QSSGMesh::Mesh::ComponentType::Float32,
                               3
                           });
        }
        if (targetBinormalData[i].length() > 0) {
            entries.append({
                               QSSGMesh::MeshInternal::getTargetBinormalAttrName(i),
                               targetBinormalData[i],
                               QSSGMesh::Mesh::ComponentType::Float32,
                               3
                           });
        }
    }

    QVector<QSSGMesh::AssetMeshSubset> subsets;
    for (const SubsetEntryData &subset : subsetData) {
        subsets.append({
                           subset.name,
                           quint32(subset.indexLength),
                           quint32(subset.indexOffset),
                           0 // the builder will calculate bounds from the position data
                       });
    }

    const QSSGMesh::Mesh mesh = QSSGMesh::Mesh::fromAssetData(entries, indexBufferData, indexType, subsets);
    if (mesh.isValid()) {
        if (!mesh.save(&file))
            return QString::asprintf("Failed to serialize mesh to %s", qPrintable(file.fileName()));
    } else {
        return QString::asprintf("Mesh building failed for %s", qPrintable(file.fileName()));
    }

    return QString();
}

QVector<QString> AssimpImporter::generateMorphing(aiNode *, const QVector<aiMesh *> &meshes, QTextStream &output, int tabLevel)
{
    QVector<QString> targets;
    quint32 numMorphTargets = 0;
    QVector<bool> needsTargetPosition;
    QVector<bool> needsTargetNormal;
    QVector<bool> needsTargetTangent;
    QVector<float> targetWeights;
    QVector<QString> targetNames;
    unsigned int morphingMethod = UINT_MAX;
    for (const auto *mesh : meshes) {
        if (mesh->mNumAnimMeshes && mesh->mAnimMeshes) {
            // According to the gltf2 spec, numMorphTargets should be the same
            // for all the submeshes. Other formats?
            const quint32 numAnimMeshes = qMin(8U, mesh->mNumAnimMeshes);
            if (numMorphTargets < numAnimMeshes) {
                numMorphTargets = numAnimMeshes;
                needsTargetPosition.resize(numMorphTargets);
                needsTargetNormal.resize(numMorphTargets);
                needsTargetTangent.resize(numMorphTargets);
                targetWeights.resize(numMorphTargets);
                targetNames.resize(numMorphTargets);
            }
            if (morphingMethod == UINT_MAX) {
                // These values for all the submeshes should be the same.
                morphingMethod = mesh->mMethod;
                for (quint32 i = 0; i < numAnimMeshes; ++i) {
                    auto animMesh = mesh->mAnimMeshes[i];
                    targetWeights[i] = animMesh->mWeight;
                    targetNames[i] = QString::fromUtf8(animMesh->mName.C_Str());
                }
            }
            for (quint32 i = 0; i < numAnimMeshes; ++i) {
                auto animMesh = mesh->mAnimMeshes[i];
                needsTargetPosition[i] |= animMesh->HasPositions();
                needsTargetNormal[i] |= animMesh->HasNormals();
                needsTargetTangent[i] |= animMesh->HasTangentsAndBitangents();
            }
        }
    }

    // Meshes do not have any morphing targets
    if (numMorphTargets == 0)
        return targets;

    // We will support gltf's morphing method now.
    // If we need to support collada's morphing,
    // we need to check the morphing methods and implement them in backend
    //QSSGQmlUtilities::writeQmlPropertyHelper(output,
    //                                         tabLevel,
    //                                         QSSGQmlUtilities::PropertyMap::Model,
    //                                         QStringLiteral("morphingMode"),
    //                                         morphingMethod);

    QString id;
    for (unsigned i = 0; i < numMorphTargets; ++i) {
        id = generateUniqueId(QSSGQmlUtilities::sanitizeQmlId(targetNames[i]));
        targets.push_back(id);
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("MorphTarget {\n");
        output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("id: ")
               << id << QStringLiteral("\n");
        output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("weight: ")
               << targetWeights[i] << QStringLiteral("\n");
        output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("attributes: ");
        bool needsOring = false;
        if (needsTargetPosition[i]) {
            output << QStringLiteral("MorphTarget.Position");
            needsOring = true;
        }
        if (needsTargetNormal[i]) {
            if (needsOring)
                output << QStringLiteral(" | ");
            output << QStringLiteral("MorphTarget.Normal");
            needsOring = true;
        }
        if (needsTargetTangent[i]) {
            if (needsOring)
                output << QStringLiteral(" | ");
            // assimp always has tangent and binormal together.
            output << QStringLiteral("MorphTarget.Tangent | MorphTarget.Binormal\n");
        } else {
            output << QStringLiteral("\n");
        }
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("}\n");
    }
    return targets;
}

namespace {

QColor aiColorToQColor(const aiColor3D &color)
{
    return QColor::fromRgbF(double(color.r), double(color.g), double(color.b));
}

QColor aiColorToQColor(const aiColor4D &color)
{
    QColor qtColor;
    qtColor.setRedF(double(color.r));
    qtColor.setGreenF(double(color.g));
    qtColor.setBlueF(double(color.b));
    qtColor.setAlphaF(double(color.a));
    return qtColor;
}

}

void AssimpImporter::generateMaterial(aiMaterial *material, QTextStream &output, int tabLevel)
{
    output << QStringLiteral("\n");
    if (!m_gltfMode)
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("DefaultMaterial {\n");
    else
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("PrincipledMaterial {\n");

    // id
    QString id = generateUniqueId(QSSGQmlUtilities::sanitizeQmlId(material->GetName().C_Str() + QStringLiteral("_material")));
    output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("id: ") << id << QStringLiteral("\n");
    m_materialIdMap.insert(material, id);

    aiReturn result;

    if (!m_gltfMode) {

        int shadingModel = 0;
        result = material->Get(AI_MATKEY_SHADING_MODEL, shadingModel);
        // lighting
        if (result == aiReturn_SUCCESS) {
            if (shadingModel == aiShadingMode_NoShading)
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("lighting: DefaultMaterial.NoLighting\n");
        }


        QString diffuseMapImage = generateImage(material, aiTextureType_DIFFUSE, 0, tabLevel + 1);
        if (!diffuseMapImage.isNull())
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("diffuseMap: ") << diffuseMapImage << QStringLiteral("\n");

        // For some reason the normal behavior is that either you have a diffuseMap[s] or a diffuse color
        // but no a mix of both... So only set the diffuse color if none of the diffuse maps are set:
        if (diffuseMapImage.isNull()) {
            aiColor3D diffuseColor;
            result = material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuseColor);
            if (result == aiReturn_SUCCESS) {
                QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                         tabLevel + 1,
                                                         QSSGQmlUtilities::PropertyMap::DefaultMaterial,
                                                         QStringLiteral("diffuseColor"),
                                                         aiColorToQColor(diffuseColor));
            }
        }

        QString emissiveMapImage = generateImage(material, aiTextureType_EMISSIVE, 0, tabLevel + 1);
        if (!emissiveMapImage.isNull())
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("emissiveMap: ") << emissiveMapImage << QStringLiteral("\n");

        // emissiveColor AI_MATKEY_COLOR_EMISSIVE
        aiColor3D emissiveColor;
        result = material->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColor);
        if (result == aiReturn_SUCCESS) {
            // ### set emissive color
        }
        // specularReflectionMap

        QString specularMapImage = generateImage(material, aiTextureType_SPECULAR, 0, tabLevel + 1);
        if (!specularMapImage.isNull())
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("specularMap: ") << specularMapImage << QStringLiteral("\n");

        // specularModel AI_MATKEY_SHADING_MODEL

        // specularTint AI_MATKEY_COLOR_SPECULAR
        aiColor3D specularTint;
        result = material->Get(AI_MATKEY_COLOR_SPECULAR, specularTint);
        if (result == aiReturn_SUCCESS) {
            // ### set specular color
        }

        // indexOfRefraction AI_MATKEY_REFRACTI

        // fresnelPower

        // specularAmount

        // specularRoughness

        // roughnessMap

        // opacity AI_MATKEY_OPACITY
        ai_real opacity;
        result = material->Get(AI_MATKEY_OPACITY, opacity);
        if (result == aiReturn_SUCCESS) {
            QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                     tabLevel + 1,
                                                     QSSGQmlUtilities::PropertyMap::DefaultMaterial,
                                                     QStringLiteral("opacity"),
                                                     opacity);
        }

        // opacityMap aiTextureType_OPACITY 0
        QString opacityMapImage = generateImage(material, aiTextureType_OPACITY, 0, tabLevel + 1);
        if (!opacityMapImage.isNull())
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("opacityMap: ") << opacityMapImage;

        // bumpMap aiTextureType_HEIGHT 0
        QString bumpMapImage = generateImage(material, aiTextureType_HEIGHT, 0, tabLevel + 1);
        if (!bumpMapImage.isNull())
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("bumpMap: ") << bumpMapImage;

        // bumpAmount AI_MATKEY_BUMPSCALING

        // normalMap aiTextureType_NORMALS 0
        QString normalMapImage = generateImage(material, aiTextureType_NORMALS, 0, tabLevel + 1);
        if (!normalMapImage.isNull())
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("normalMap: ") << normalMapImage;

        // translucencyMap

        // translucentFalloff AI_MATKEY_TRANSPARENCYFACTOR

        // diffuseLightWrap

        // (enable) vertexColors
    } else {
        // GLTF Mode
        {
            aiColor4D baseColorFactor;
            result = material->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_BASE_COLOR_FACTOR, baseColorFactor);
            if (result == aiReturn_SUCCESS)
                QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                         tabLevel + 1,
                                                         QSSGQmlUtilities::PropertyMap::PrincipledMaterial,
                                                         QStringLiteral("baseColor"),
                                                         aiColorToQColor(baseColorFactor));

            QString baseColorImage = generateImage(material, AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_BASE_COLOR_TEXTURE, tabLevel + 1);
            if (!baseColorImage.isNull()) {
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("baseColorMap: ") << baseColorImage << QStringLiteral("\n");
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("opacityChannel: Material.A\n");
            }
        }

        {
            QString metalicRoughnessImage = generateImage(material, AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE, tabLevel + 1);
            if (!metalicRoughnessImage.isNull()) {
                // there are two fields now for this, so just use it twice for now
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("metalnessMap: ") << metalicRoughnessImage << QStringLiteral("\n");
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("metalnessChannel: Material.B\n");
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("roughnessMap: ") << metalicRoughnessImage << QStringLiteral("\n");
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("roughnessChannel: Material.G\n");
            }

            ai_real metallicFactor;
            result = material->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLIC_FACTOR, metallicFactor);
            if (result == aiReturn_SUCCESS) {
                QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                         tabLevel + 1,
                                                         QSSGQmlUtilities::PropertyMap::PrincipledMaterial,
                                                         QStringLiteral("metalness"),
                                                         metallicFactor);
            }

            ai_real roughnessFactor;
            result = material->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_ROUGHNESS_FACTOR, roughnessFactor);
            if (result == aiReturn_SUCCESS) {
                QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                         tabLevel + 1,
                                                         QSSGQmlUtilities::PropertyMap::PrincipledMaterial,
                                                         QStringLiteral("roughness"),
                                                         roughnessFactor);
            }
        }

        {
            QString normalTextureImage = generateImage(material, aiTextureType_NORMALS, 0, tabLevel + 1);
            if (!normalTextureImage.isNull())
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("normalMap: ") << normalTextureImage << QStringLiteral("\n");
        }

        // Occlusion Textures are not implimented (yet)
        {
            QString occlusionTextureImage = generateImage(material, aiTextureType_LIGHTMAP, 0, tabLevel + 1);
            if (!occlusionTextureImage.isNull()) {
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("occlusionMap: ") << occlusionTextureImage << QStringLiteral("\n");
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("occlusionChannel: Material.R\n");
            }
        }

        {
            QString emissiveTextureImage = generateImage(material, aiTextureType_EMISSIVE, 0, tabLevel + 1);
            if (!emissiveTextureImage.isNull())
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("emissiveMap: ") << emissiveTextureImage << QStringLiteral("\n");
        }

        {
            aiColor3D emissiveColorFactor;
            result = material->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColorFactor);
            if (result == aiReturn_SUCCESS) {
                QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                         tabLevel + 1,
                                                         QSSGQmlUtilities::PropertyMap::PrincipledMaterial,
                                                         QStringLiteral("emissiveColor"),
                                                         aiColorToQColor(emissiveColorFactor));
            }
        }

        {
            bool isDoubleSided;
            result = material->Get(AI_MATKEY_TWOSIDED, isDoubleSided);
            if (result == aiReturn_SUCCESS && isDoubleSided)
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("cullMode: Material.NoCulling\n");
        }

        {
            aiString alphaMode;
            result = material->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode);
            if (result == aiReturn_SUCCESS) {
                const QString mode = QString::fromUtf8(alphaMode.C_Str()).toLower();
                QString qtMode;
                if (mode == QStringLiteral("opaque"))
                    qtMode = QStringLiteral("PrincipledMaterial.Opaque");
                else if (mode == QStringLiteral("mask"))
                    qtMode = QStringLiteral("PrincipledMaterial.Mask");
                else if (mode == QStringLiteral("blend"))
                    qtMode = QStringLiteral("PrincipledMaterial.Blend");

                if (!qtMode.isNull())
                    QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                             tabLevel + 1,
                                                             QSSGQmlUtilities::PropertyMap::PrincipledMaterial,
                                                             QStringLiteral("alphaMode"),
                                                             qtMode);

            }
        }

        {
            ai_real alphaCutoff;
            result = material->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alphaCutoff);
            if (result == aiReturn_SUCCESS) {
                QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                         tabLevel + 1,
                                                         QSSGQmlUtilities::PropertyMap::PrincipledMaterial,
                                                         QStringLiteral("alphaCutoff"),
                                                         alphaCutoff);
            }
        }

        {
            bool isUnlit;
            result = material->Get(AI_MATKEY_GLTF_UNLIT, isUnlit);
            if (result == aiReturn_SUCCESS && isUnlit)
                output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("lighting: PrincipledMaterial.NoLighting\n");
        }

        // SpecularGlossiness Properties
        bool hasSpecularGlossiness;
        result = material->Get(AI_MATKEY_GLTF_PBRSPECULARGLOSSINESS, hasSpecularGlossiness);
        if (result == aiReturn_SUCCESS && hasSpecularGlossiness) {

            // diffuseFactor (color) // not used (yet), but ends up being diffuseColor
//            {
//                aiColor4D diffuseColor;
//                result = material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuseColor);
//                if (result == aiReturn_SUCCESS)
//                    QSSGQmlUtilities::writeQmlPropertyHelper(output,
//                                                             tabLevel + 1,
//                                                             QSSGQmlUtilities::PropertyMap::PrincipledMaterial,
//                                                             QStringLiteral("diffuseColor"),
//                                                             aiColorToQColor(diffuseColor));
//            }

            // specularColor (color) (our property is a float?)
//            {
//                aiColor3D specularColor;
//                result = material->Get(AI_MATKEY_COLOR_SPECULAR, specularColor);
//                if (result == aiReturn_SUCCESS)
//                    QSSGQmlUtilities::writeQmlPropertyHelper(output,
//                                                             tabLevel + 1,
//                                                             QSSGQmlUtilities::PropertyMap::PrincipledMaterial,
//                                                             QStringLiteral("specularTint"),
//                                                             aiColorToQColor(specularColor));
//            }

            // glossinessFactor (float)
            {
                ai_real glossiness;
                result = material->Get(AI_MATKEY_GLTF_PBRSPECULARGLOSSINESS_GLOSSINESS_FACTOR, glossiness);
                if (result == aiReturn_SUCCESS)
                    QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                             tabLevel + 1,
                                                             QSSGQmlUtilities::PropertyMap::PrincipledMaterial,
                                                             QStringLiteral("specularAmount"),
                                                             glossiness);
            }

            // diffuseTexture // not used (yet), but ends up being diffuseMap(1)
//            {
//                QString diffuseMapImage = generateImage(material, aiTextureType_DIFFUSE, 0, tabLevel + 1);
//                if (!diffuseMapImage.isNull())
//                    output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("diffuseMap: ") << diffuseMapImage << QStringLiteral("\n");
//            }

            // specularGlossinessTexture
            {
                QString specularMapImage = generateImage(material, aiTextureType_SPECULAR, 0, tabLevel + 1);
                if (!specularMapImage.isNull())
                    output << QSSGQmlUtilities::insertTabs(tabLevel + 1) << QStringLiteral("specularMap: ") << specularMapImage << QStringLiteral("\n");
            }
        }
    }

    output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("}");
}

namespace  {
QString aiTilingMode(int tilingMode) {
    if (tilingMode == aiTextureMapMode_Wrap)
        return QStringLiteral("Texture.Repeat");
    if (tilingMode == aiTextureMapMode_Mirror)
        return QStringLiteral("Texture.MirroredRepeat");
    if (tilingMode == aiTextureMapMode_Clamp)
        return QStringLiteral("Texture.ClampToEdge");

    return QStringLiteral("Texture.Repeat");
}
}

QString AssimpImporter::generateImage(aiMaterial *material, aiTextureType textureType, unsigned index, int tabLevel)
{
    // Figure out if there is actually something to generate
    aiString texturePath;
    aiTextureMapping textureMapping = aiTextureMapping::aiTextureMapping_OTHER;
    uint uvIndex = 0;
    aiTextureMapMode modes[3];
    aiReturn result = material->GetTexture(textureType, index,
                                           &texturePath,
                                           &textureMapping,
                                           &uvIndex,
                                           nullptr,
                                           nullptr,
                                           modes);
    if (result != aiReturn_SUCCESS)
        return QString();

    // If there is no texture, then there is nothing to generate
    if (texturePath.length == 0)
        return QString();
    QString textureName = QString::fromUtf8(texturePath.C_Str());
    const QString embeddedTexturePath = getEmbeddedTexturePath(texturePath.C_Str(), "");

    // Replace Windows separator to Unix separator
    // so that assets including Windows relative path can be converted on Unix.
    textureName.replace("\\","/");
    QString targetFileName;

    const int texId = textureNameToInt(texturePath.C_Str());

    // Is this an embedded texture or a file
    if (m_embeddedTextureSources.contains(texId)) {
        targetFileName = m_embeddedTextureSources.value(texId);
    } else if (m_embeddedTextureSources.key(embeddedTexturePath, -1) != -1) {
        targetFileName = embeddedTexturePath;
    } else {
        // File Reference (needs to be copied into component)
        // Check that this file exists
        QString sourcePath(m_sourceFile.absolutePath() + "/" + textureName);
        QFileInfo sourceFile(sourcePath);
        // If it doesn't exist, there is nothing to generate
        if (!sourceFile.exists()) {
            qWarning() << sourcePath << " (a.k.a. " << sourceFile.absoluteFilePath() << ")"
                       << " does not exist, skipping";
            return QString();
        }
        targetFileName = QStringLiteral("maps/") + sourceFile.fileName();
        // Copy the file to the maps directory
        m_savePath.mkdir(QStringLiteral("./maps"));
        QFileInfo targetFile(QString(m_savePath.absolutePath() + QDir::separator() + targetFileName));
        if (QFile::copy(sourceFile.absoluteFilePath(), targetFile.absoluteFilePath()))
            m_generatedFiles += targetFile.absoluteFilePath();
    }
    // Start QML generation
    QString outputString;
    QTextStream output(&outputString, QIODevice::WriteOnly);
    output << QStringLiteral("Texture {\n");

    output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
           << QStringLiteral("source: \"")
           << targetFileName << QStringLiteral("\"\n");

    if (m_gltfMode) {
        uint gltfUvIndex = 0;
        result = material->Get(AI_MATKEY_GLTF_TEXTURE_TEXCOORD(textureType, index), gltfUvIndex);
        if (result == aiReturn_SUCCESS)
            uvIndex = gltfUvIndex;
    }

    if (uvIndex > 0) {
        // Quick3D supports 2 tex coords.
        // According to gltf's khronos default implementation,
        // the index will be selected to the nearest one.
        output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
               << QStringLiteral("indexUV: 1\n");
    }

    // mapping
    if (textureMapping == aiTextureMapping_UV) {
        // So we should be able to always hit this case by passing the right flags
        // at import.
        QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                   tabLevel + 1,
                                                   QSSGQmlUtilities::PropertyMap::Texture,
                                                   QStringLiteral("mappingMode"),
                                                   QStringLiteral("Texture.UV"));
        // It would be possible to use another channel than UV0 to map texture data
        // but for now we force everything to use UV0
        //int uvSource;
        //material->Get(AI_MATKEY_UVWSRC(textureType, index), uvSource);
    } else if (textureMapping == aiTextureMapping_SPHERE) {
        // (not supported)
    } else if (textureMapping == aiTextureMapping_CYLINDER) {
        // (not supported)
    } else if (textureMapping == aiTextureMapping_BOX) {
        // (not supported)
    } else if (textureMapping == aiTextureMapping_PLANE) {
        // (not supported)
    } else {
        // other... (not supported)
    }

    // mapping mode U
    QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                               tabLevel + 1,
                                               QSSGQmlUtilities::PropertyMap::Texture,
                                               QStringLiteral("tilingModeHorizontal"),
                                               aiTilingMode(modes[0]));

    // mapping mode V
    QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                               tabLevel + 1,
                                               QSSGQmlUtilities::PropertyMap::Texture,
                                               QStringLiteral("tilingModeVertical"),
                                               aiTilingMode(modes[1]));

    aiUVTransform transforms;
    result = material->Get(AI_MATKEY_UVTRANSFORM(textureType, index), transforms);
    if (result == aiReturn_SUCCESS) {
        // UV origins -
        //      glTF: 0, 1 (top left of texture)
        //      Assimp, Collada?, FBX?: 0.5, 0.5
        //      Quick3D: 0, 0 (bottom left of texture)
        // Assimp already tries to fix it but it's not correct.
        // So, we restore original values and then use pivot
        float rotation = -transforms.mRotation;
        float rotationUV = qRadiansToDegrees(rotation);
        float posU = transforms.mTranslation.x;
        float posV = transforms.mTranslation.y;
        if (m_gltfUsed) {
            float rcos = std::cos(rotation);
            float rsin = std::sin(rotation);
            posU -= 0.5 * transforms.mScaling.x * (-rcos + rsin + 1);
            posV -= (0.5 * transforms.mScaling.y * (rcos + rsin - 1) + 1 - transforms.mScaling.y);

            output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
                   << QStringLiteral("pivotV: 1\n");
        } else {
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
                   << QStringLiteral("pivotU: 0.5\n");
            output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
                   << QStringLiteral("pivotV: 0.5\n");
        }

        QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                 tabLevel + 1,
                                                 QSSGQmlUtilities::PropertyMap::Texture,
                                                 QStringLiteral("positionU"),
                                                 posU);
        QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                 tabLevel + 1,
                                                 QSSGQmlUtilities::PropertyMap::Texture,
                                                 QStringLiteral("positionV"),
                                                 posV);
        QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                 tabLevel + 1,
                                                 QSSGQmlUtilities::PropertyMap::Texture,
                                                 QStringLiteral("rotationUV"),
                                                 rotationUV);
        QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                 tabLevel + 1,
                                                 QSSGQmlUtilities::PropertyMap::Texture,
                                                 QStringLiteral("scaleU"),
                                                 transforms.mScaling.x);
        QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                 tabLevel + 1,
                                                 QSSGQmlUtilities::PropertyMap::Texture,
                                                 QStringLiteral("scaleV"),
                                                 transforms.mScaling.y);
    }
    // We don't make use of the data here, but there are additional flags
    // available for example the usage of the alpha channel
    // texture flags
    //int textureFlags;
    //material->Get(AI_MATKEY_TEXFLAGS(textureType, index), textureFlags);

    // Always generate and use mipmaps for imported assets
    if (m_forceMipMapGeneration) {
        QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                 tabLevel + 1,
                                                 QSSGQmlUtilities::PropertyMap::Texture,
                                                 QStringLiteral("generateMipmaps"),
                                                 true);
        QSSGQmlUtilities::writeQmlPropertyHelper(output,
                                                 tabLevel + 1,
                                                 QSSGQmlUtilities::PropertyMap::Texture,
                                                 QStringLiteral("mipFilter"),
                                                 QStringLiteral("Texture.Linear"));
    }

    output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("}");

    return outputString;
}

void AssimpImporter::generateSkeleton(aiNode *node, quint32 idx, QTextStream &output, int tabLevel)
{
    if (!node)
        return;

    QString nodeName = QString::fromUtf8(node->mName.C_Str());

    if (isBone(node) && !m_generatedBones.contains(node)) {
        m_generatedBones.insert(node);
        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("Joint {\n");
        generateNodeProperties(node, output, tabLevel + 1);

        qint32 boneIdx = m_boneIdxMap[nodeName];

        output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
               << QStringLiteral("index: ") << QString::number(boneIdx)
               << QStringLiteral("\n");
        output << QSSGQmlUtilities::insertTabs(tabLevel + 1)
               << QStringLiteral("skeletonRoot: ") << m_skeletonIds[idx]
               << QStringLiteral("\n");
        for (uint i = 0; i < node->mNumChildren; ++i)
            generateSkeleton(node->mChildren[i], idx, output, tabLevel + 1);

        output << QSSGQmlUtilities::insertTabs(tabLevel) << QStringLiteral("}\n");
    }
    for (uint i = 0; i < node->mNumChildren; ++i)
        generateSkeleton(node->mChildren[i], idx, output, tabLevel);
}

void AssimpImporter::processAnimations(QTextStream &output)
{
    for (int idx = 0; idx < m_animations.size(); ++idx) {
        QHash<aiNode *, aiNodeAnim *> *animation = m_animations[idx];
        QHash<aiNode *, aiMeshMorphAnim *> *morphAnimation = m_morphAnimations[idx];
        // skip empty animations
        if (animation->count() == 0 && morphAnimation->count() == 0)
            continue;
        output << QStringLiteral("\n");
        output << QSSGQmlUtilities::insertTabs(1) << "Timeline {\n";
        output << QSSGQmlUtilities::insertTabs(2) << "id: timeline" << idx << "\n";
        output << QSSGQmlUtilities::insertTabs(2) << "startFrame: 0\n";

        QString keyframeString;
        QTextStream keyframeStream(&keyframeString);
        qreal endFrameTime = 0;

        for (auto itr = morphAnimation->begin(); itr != morphAnimation->end(); ++itr) {
            aiNode *node = itr.key();

            // We cannot set keyframes to nodes which do not have id.
            const auto idItr = m_nodeIdMap.constFind(node);
            if (idItr == m_nodeIdMap.cend())
                continue;

            // We can set morph animation only on Model.
            const auto typeItr = m_nodeTypeMap.constFind(node);
            if (typeItr == m_nodeTypeMap.cend())
                continue;
            if (*typeItr != QSSGQmlUtilities::PropertyMap::Model)
                continue;

            aiMeshMorphAnim *morphAnim = itr.value();
            generateMorphKeyframes(*idItr, morphAnim->mNumKeys, morphAnim->mKeys,
                                   keyframeStream, endFrameTime);
        }
        for (auto itr = animation->begin(); itr != animation->end(); ++itr) {
            aiNode *node = itr.key();

            // We cannot set keyframes to nodes which do not have id.
            const auto idItr = m_nodeIdMap.constFind(node);
            if (idItr == m_nodeIdMap.cend())
                continue;
            // We can set animation only on Node, Model, Camera or Light.
            const auto typeItr = m_nodeTypeMap.constFind(node);
            if (typeItr == m_nodeTypeMap.cend())
                continue;
            const auto type = typeItr.value();
            if (type != QSSGQmlUtilities::PropertyMap::Node
                && type != QSSGQmlUtilities::PropertyMap::Model
                && type != QSSGQmlUtilities::PropertyMap::Joint
                && type != QSSGQmlUtilities::PropertyMap::PerspectiveCamera
                && type != QSSGQmlUtilities::PropertyMap::OrthographicCamera
                && type != QSSGQmlUtilities::PropertyMap::DirectionalLight
                && type != QSSGQmlUtilities::PropertyMap::PointLight
                && type != QSSGQmlUtilities::PropertyMap::SpotLight)
                continue;

            aiNodeAnim *nodeAnim = itr.value();
            if (nodeAnim->mNumPositionKeys > 0) {
                generateKeyframes(*idItr, "position", nodeAnim->mNumPositionKeys,
                                  nodeAnim->mPositionKeys,
                                  keyframeStream, endFrameTime);
            }
            if (nodeAnim->mNumRotationKeys > 0) {
                generateKeyframes(*idItr, "rotation", nodeAnim->mNumRotationKeys,
                                  nodeAnim->mRotationKeys,
                                  keyframeStream, endFrameTime);
            }
            if (nodeAnim->mNumScalingKeys > 0) {
                generateKeyframes(*idItr, "scale", nodeAnim->mNumScalingKeys,
                                  nodeAnim->mScalingKeys,
                                  keyframeStream, endFrameTime);
            }
        }

        int endFrameTimeInt = qCeil(endFrameTime);
        output << QSSGQmlUtilities::insertTabs(2) << QStringLiteral("endFrame: ") << endFrameTimeInt << QStringLiteral("\n");
        output << QSSGQmlUtilities::insertTabs(2) << QStringLiteral("currentFrame: 0\n");
        // all animations are enabled for now.
        output << QSSGQmlUtilities::insertTabs(2) << QStringLiteral("enabled: true\n");
        output << QSSGQmlUtilities::insertTabs(2) << QStringLiteral("animations: [\n");
        output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("TimelineAnimation {\n");
        output << QSSGQmlUtilities::insertTabs(4) << QStringLiteral("duration: ") << endFrameTimeInt << QStringLiteral("\n");
        output << QSSGQmlUtilities::insertTabs(4) << QStringLiteral("from: 0\n");
        output << QSSGQmlUtilities::insertTabs(4) << QStringLiteral("to: ") << endFrameTimeInt << QStringLiteral("\n");
        output << QSSGQmlUtilities::insertTabs(4) << QStringLiteral("running: true\n");
        output << QSSGQmlUtilities::insertTabs(4) << QStringLiteral("loops: Animation.Infinite\n");
        output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("}\n");
        output << QSSGQmlUtilities::insertTabs(2) << QStringLiteral("]\n");

        output << keyframeString;

        output << QSSGQmlUtilities::insertTabs(1) << QStringLiteral("}\n");
    }
}

namespace {
QString convertToQString(const aiVector3D &vec)
{
    return QString("Qt.vector3d(%1, %2, %3)").arg(vec.x).arg(vec.y).arg(vec.z);
}

QString convertToQString(const aiQuaternion &q)
{
    return QString("Qt.quaternion(%1, %2, %3, %4)").arg(q.w).arg(q.x).arg(q.y).arg(q.z);
}

// Add Vector3D into CBOR
void appendData(QCborStreamWriter &writer, const aiVector3D &vec)
{
    writer.append(vec.x);
    writer.append(vec.y);
    writer.append(vec.z);
}

// Add Quaternion into CBOR
void appendData(QCborStreamWriter &writer, const aiQuaternion &q)
{
    writer.append(q.w);
    writer.append(q.x);
    writer.append(q.y);
    writer.append(q.z);
}

// Add weight into CBOR
void appendData(QCborStreamWriter &writer, const double &data)
{
    writer.append(data);
}

int getTypeValue(const aiVector3D &vec)
{
    Q_UNUSED(vec)
    return int(QMetaType::QVector3D);
}

int getTypeValue(const aiQuaternion &q)
{
    Q_UNUSED(q)
    return int(QMetaType::QQuaternion);
}

int getTypeValue(const double &data)
{
    Q_UNUSED(data)
    return int(QMetaType::Double);
}

}

template <typename T>
void AssimpImporter::generateKeyframes(const QString &id, const QString &propertyName, uint numKeys, const T *keys,
                                       QTextStream &output, qreal &maxKeyframeTime)
{
    output << QStringLiteral("\n");
    output << QSSGQmlUtilities::insertTabs(2) << QStringLiteral("KeyframeGroup {\n");
    output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("target: ") << id << QStringLiteral("\n");
    output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("property: \"") << propertyName << QStringLiteral("\"\n");

    QList<T> keyframes;
    keyframes.push_back(keys[0]);
    for (uint i = 1; i < numKeys; ++i) {
        if (fuzzyCompare(keyframes.back().mValue, keys[i].mValue)) {
            if (i == numKeys - 1 || fuzzyCompare(keys[i].mValue, keys[i+1].mValue))
                continue;
        }
        keyframes.push_back(keys[i]);
    }

    if (numKeys > 0)
        maxKeyframeTime = qMax(maxKeyframeTime, keys[numKeys - 1].mTime);


    if (!keyframes.isEmpty()) {
        // If keyframe values are fixed, it will not be generated into binary
        if (m_binaryKeyframes && keyframes.size() != 1) {
            // Generate animations file
            QString outputAnimationFile = QStringLiteral("animations/") + id + QStringLiteral("_")
                    + propertyName + QStringLiteral(".qad");
            m_savePath.mkdir(QStringLiteral("./animations"));
            QString animationFilePath = m_savePath.absolutePath() + QLatin1Char('/') + outputAnimationFile;
            int index = 0;
            while (m_generatedFiles.contains(animationFilePath)) {
                outputAnimationFile = QStringLiteral("animations/") + id
                    + QStringLiteral("_") + propertyName + QStringLiteral("_")
                    + QString::number(index++) + QStringLiteral(".qad");
                animationFilePath = m_savePath.absolutePath() + QLatin1Char('/') + outputAnimationFile;
            }
            QFile animationFile(animationFilePath);
            // Write the binary content
            if (generateAnimationFile(animationFile, keyframes))
                m_generatedFiles << animationFilePath;

            output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("keyframeSource: \"")
                   << outputAnimationFile << QStringLiteral("\"\n");

        } else {
            // Output all the Keyframes except similar ones.
            for (int i = 0; i < keyframes.size(); ++i) {
                output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("Keyframe {\n");
                output << QSSGQmlUtilities::insertTabs(4) << QStringLiteral("frame: ") << keyframes[i].mTime << QStringLiteral("\n");
                output << QSSGQmlUtilities::insertTabs(4) << QStringLiteral("value: ")
                       << convertToQString(keyframes[i].mValue) << QStringLiteral("\n");
                output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("}\n");
            }
        }
    }
    output << QSSGQmlUtilities::insertTabs(2) << QStringLiteral("}\n");
}

// Generates binary keyframes
// For format specification, see Qt Quick Timeline module.
template<typename T>
bool AssimpImporter::generateAnimationFile(QFile &file, const QList<T> &keyframes)
{
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Could not open keyframes file:" << file.fileName();
        return false;
    }

    QCborStreamWriter writer(&file);
    // Start root array
    writer.startArray();
    // header name
    writer.append("QTimelineKeyframes");
    // file version. Increase this if the format changes.
    const int keyframesDataVersion = 1;
    writer.append(keyframesDataVersion);
    // property type (here Vector3D or Quaternion)
    writer.append(getTypeValue(keyframes[0].mValue));

    // Start Keyframes array
    writer.startArray();
    for (int i = 0; i < keyframes.size(); ++i) {
        writer.append(keyframes[i].mTime);
        // Easing always linear
        writer.append(QEasingCurve::Linear);
        appendData(writer, keyframes[i].mValue);
    }
    // End Keyframes array
    writer.endArray();
    // End root array
    writer.endArray();
    file.close();

    return true;
}

// This function is made based on GLTF2
void AssimpImporter::generateMorphKeyframes(const QString &id,
                                            uint numKeys, const aiMeshMorphKey *keys,
                                            QTextStream &output, qreal &maxKeyframeTime)
{
    Q_ASSERT(numKeys > 0);

    const uint numMorphTargets = (keys[0].mNumValuesAndWeights > 8) ? 8: keys[0].mNumValuesAndWeights;

    output << QStringLiteral("\n");
    for (uint i = 0; i < numMorphTargets; ++i) {
        output << QSSGQmlUtilities::insertTabs(2) << QStringLiteral("KeyframeGroup {\n");
        output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("target: ") << id
               << QStringLiteral(".morphTargets[") << QString::number(i)
               << QStringLiteral("]\n");
        output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("property: \"weight\"\n");
        QList<weightKey> keyframes;
        keyframes.push_back(weightKey(keys[0].mTime, keys[0].mWeights[i]));
        for (uint j = 1; j < numKeys; ++j) {
            if (qFuzzyCompare(keyframes.back().mValue, keys[j].mWeights[i])) {
                if (j == numKeys - 1 || qFuzzyCompare(keys[j].mWeights[i], keys[j+1].mWeights[i]))
                    continue;
            }

            keyframes.push_back(weightKey(keys[j].mTime, keys[j].mWeights[i]));
        }
        if (numKeys > 0)
            maxKeyframeTime = qMax(maxKeyframeTime, keys[numKeys - 1].mTime);

        if (!keyframes.isEmpty()) {
            if (m_binaryKeyframes && keyframes.size() != 1) {
                // Generate animations file
                QString outputAnimationFile = QStringLiteral("animations/") + id
                        + QStringLiteral("_morphTarget_") + QString::number(i)
                        + QStringLiteral("_weight.qad");
                m_savePath.mkdir(QStringLiteral("./animations"));
                QString animationFilePath = m_savePath.absolutePath() + QLatin1Char('/') + outputAnimationFile;
                QFile animationFile(animationFilePath);
                int index = 0;
                while (m_generatedFiles.contains(animationFilePath)) {
                    outputAnimationFile = QStringLiteral("animations/") + id
                        + QStringLiteral("_morphTarget_%1_%2").arg(i).arg(index++)
                        + QStringLiteral(".qad");
                    animationFilePath = m_savePath.absolutePath() + QLatin1Char('/') + outputAnimationFile;
                }
                // Write the binary content
                if (generateAnimationFile(animationFile, keyframes))
                    m_generatedFiles << animationFilePath;

                output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("keyframeSource: \"")
                       << outputAnimationFile << QStringLiteral("\"\n");

            } else {
                // Output all the Keyframes except similar ones.
                for (int j = 0; j < keyframes.size(); ++j) {
                    output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("Keyframe {\n");
                    output << QSSGQmlUtilities::insertTabs(4) << QStringLiteral("frame: ") << keyframes[j].mTime << QStringLiteral("\n");
                    output << QSSGQmlUtilities::insertTabs(4) << QStringLiteral("value: ")
                           << QString::number(keyframes[j].mValue) << QStringLiteral("\n");
                    output << QSSGQmlUtilities::insertTabs(3) << QStringLiteral("}\n");
                }
            }
        }
        output << QSSGQmlUtilities::insertTabs(2) << QStringLiteral("}\n");
    }
}

bool AssimpImporter::isModel(aiNode *node)
{
    return node && node->mNumMeshes > 0;
}

bool AssimpImporter::isLight(aiNode *node)
{
    return node && m_lights.contains(node);
}

bool AssimpImporter::isCamera(aiNode *node)
{
    return node && m_cameras.contains(node);
}

bool AssimpImporter::isBone(aiNode *node)
{
    if (!node)
        return false;
    QString boneName = QString::fromUtf8(node->mName.C_Str());
    return m_bones.contains(boneName);
}

QString AssimpImporter::generateUniqueId(const QString &id)
{
    int index = 0;
    QString uniqueID = id;
    while (m_uniqueIds.contains(uniqueID))
        uniqueID = id + QStringLiteral("_") + QString::number(++index);
    m_uniqueIds.insert(uniqueID);
    return uniqueID;
}

// This method is used to walk a subtree to see if any of the nodes actually
// add any state to the scene.  A branch of empty transform nodes would only be
// useful if they were being used somewhere else (like where to aim a camera),
// but the general case is that they can be safely culled
bool AssimpImporter::containsNodesOfConsequence(aiNode *node)
{
    bool isUseful = false;

    isUseful |= isLight(node);
    isUseful |= isModel(node);
    isUseful |= isCamera(node);
    isUseful |= (isBone(node) && !m_generatedBones.contains(node));

    // Return early if we know already
    if (isUseful)
        return true;

    for (uint i = 0; i < node->mNumChildren; ++i)
        isUseful |= containsNodesOfConsequence(node->mChildren[i]);

    return isUseful;
}

void AssimpImporter::processOptions(const QVariantMap &options)
{
    // Setup import settings based given options
    // You can either pass the whole options object, or just the "options" object
    // so get the right scope.
    QJsonObject optionsObject = QJsonObject::fromVariantMap(options);
    if (optionsObject.contains(QStringLiteral("options")))
        optionsObject = optionsObject.value(QStringLiteral("options")).toObject();

    if (optionsObject.isEmpty())
        return;

    // parse the options list for values
    // We always need to triangulate and remove non triangles
    m_postProcessSteps = aiPostProcessSteps(aiProcess_Triangulate | aiProcess_SortByPType);

    if (checkBooleanOption(QStringLiteral("calculateTangentSpace"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_CalcTangentSpace);

    if (checkBooleanOption(QStringLiteral("joinIdenticalVertices"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_JoinIdenticalVertices);

    if (checkBooleanOption(QStringLiteral("generateNormals"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_GenNormals);

    if (checkBooleanOption(QStringLiteral("generateSmoothNormals"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_GenSmoothNormals);

    if (checkBooleanOption(QStringLiteral("splitLargeMeshes"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_SplitLargeMeshes);

    if (checkBooleanOption(QStringLiteral("preTransformVertices"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_PreTransformVertices);

    if (checkBooleanOption(QStringLiteral("improveCacheLocality"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_ImproveCacheLocality);

    if (checkBooleanOption(QStringLiteral("removeRedundantMaterials"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_RemoveRedundantMaterials);

    if (checkBooleanOption(QStringLiteral("fixInfacingNormals"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_FixInfacingNormals);

    if (checkBooleanOption(QStringLiteral("findDegenerates"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_FindDegenerates);

    if (checkBooleanOption(QStringLiteral("findInvalidData"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_FindInvalidData);

    if (checkBooleanOption(QStringLiteral("transformUVCoordinates"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_TransformUVCoords);

    if (checkBooleanOption(QStringLiteral("findInstances"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_FindInstances);

    if (checkBooleanOption(QStringLiteral("optimizeMeshes"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_OptimizeMeshes);

    if (checkBooleanOption(QStringLiteral("optimizeGraph"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_OptimizeGraph);

    if (checkBooleanOption(QStringLiteral("globalScale"), optionsObject)) {
        m_globalScaleValue = getRealOption(QStringLiteral("globalScaleValue"), optionsObject);
        if (m_globalScaleValue == 0.0)
            m_globalScaleValue = 1.0;
    }

    if (checkBooleanOption(QStringLiteral("dropNormals"), optionsObject))
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_DropNormals);

    aiComponent removeComponents = aiComponent(0);

    if (checkBooleanOption(QStringLiteral("removeComponentNormals"), optionsObject))
        removeComponents = aiComponent(removeComponents | aiComponent_NORMALS);

    if (checkBooleanOption(QStringLiteral("removeComponentTangentsAndBitangents"), optionsObject))
        removeComponents = aiComponent(removeComponents | aiComponent_TANGENTS_AND_BITANGENTS);

    if (checkBooleanOption(QStringLiteral("removeComponentColors"), optionsObject))
        removeComponents = aiComponent(removeComponents | aiComponent_COLORS);

    if (checkBooleanOption(QStringLiteral("removeComponentUVs"), optionsObject))
        removeComponents = aiComponent(removeComponents | aiComponent_TEXCOORDS);

    if (checkBooleanOption(QStringLiteral("removeComponentBoneWeights"), optionsObject))
        removeComponents = aiComponent(removeComponents | aiComponent_BONEWEIGHTS);

    if (checkBooleanOption(QStringLiteral("removeComponentAnimations"), optionsObject))
        removeComponents = aiComponent(removeComponents | aiComponent_ANIMATIONS);

    if (checkBooleanOption(QStringLiteral("removeComponentTextures"), optionsObject))
        removeComponents = aiComponent(removeComponents | aiComponent_TEXTURES);

    if (removeComponents != aiComponent(0)) {
        m_postProcessSteps = aiPostProcessSteps(m_postProcessSteps | aiProcess_RemoveComponent);
        m_importer->SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, removeComponents);
    }

    bool preservePivots = checkBooleanOption(QStringLiteral("fbxPreservePivots"), optionsObject);
    m_importer->SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, preservePivots);

    m_useFloatJointIndices = checkBooleanOption(QStringLiteral("useFloatJointIndices"), optionsObject);
    m_forceMipMapGeneration = checkBooleanOption(QStringLiteral("generateMipMaps"), optionsObject);
    m_binaryKeyframes = checkBooleanOption(QStringLiteral("useBinaryKeyframes"), optionsObject);
}

bool AssimpImporter::checkBooleanOption(const QString &optionName, const QJsonObject &options)
{
    if (!options.contains(optionName))
        return false;

    QJsonObject option = options.value(optionName).toObject();
    return option.value(QStringLiteral("value")).toBool();
}

qreal AssimpImporter::getRealOption(const QString &optionName, const QJsonObject &options)
{
    if (!options.contains(optionName))
        return false;

    QJsonObject option = options.value(optionName).toObject();
    return option.value(QStringLiteral("value")).toDouble();
}

QT_END_NAMESPACE
