// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "media_folder_tree_entity.h"

#include <algorithm>

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>

#include <core/resource/avi/avi_resource.h>
#include <core/resource/file_processor.h>
#include <core/resource_management/resource_pool.h>
#include <nx/vms/client/desktop/resource_views/data/resource_tree_globals.h>
#include <nx/vms/client/desktop/resource_views/entity_resource_tree/resource_tree_item_factory.h>
#include <nx/vms/client/desktop/resource_views/entity_item_model/entity_model_mapping.h>
#include <nx/vms/client/desktop/settings/local_settings.h>

namespace nx::vms::client::desktop {
namespace entity_resource_tree {

using namespace entity_item_model;

// --------------------------------------------------------------------------
// Entry: represents a single row (subdirectory or media file)

struct MediaFolderTreeEntity::Entry
{
    bool isDirectory = false;
    QString dirPath; // Full path for directories
    QString filePath; // Full file path for files
    AbstractItemPtr headerItem;
    std::unique_ptr<MediaFolderTreeEntity> childEntity; // Only for directories.
};

// --------------------------------------------------------------------------
// SignalHelper: QObject that monitors data sources for the root entity

struct MediaFolderTreeEntity::SignalHelper
{
    std::unique_ptr<QObject> watcher;

    SignalHelper(
        MediaFolderTreeEntity* owner,
        LocalSettings* localSettings)
        :
        watcher(std::make_unique<QObject>())
    {
        const auto ctx = watcher.get();

        // Monitor media folder configuration changes.
        if (localSettings)
        {
            QObject::connect(&localSettings->mediaFolders,
                &nx::utils::property_storage::BaseProperty::changed, ctx,
                [owner]() { owner->rebuildTree(); });
        }
    }
};

// --------------------------------------------------------------------------
// MediaFolderTreeEntity implementation

MediaFolderTreeEntity::MediaFolderTreeEntity(
    ResourceTreeItemFactory* itemFactory,
    const QnUserResourcePtr& /*user*/,
    QnResourcePool* resourcePool,
    LocalSettings* localSettings,
    const QString& dirPath)
    :
    base_type(),
    m_itemFactory(itemFactory),
    m_resourcePool(resourcePool),
    m_localSettings(localSettings),
    m_dirPath(dirPath)
{
    if (localSettings && dirPath.isEmpty())
        m_signalHelper = std::make_unique<SignalHelper>(this, localSettings);

    rebuildTree();
}

MediaFolderTreeEntity::~MediaFolderTreeEntity()
{
    m_signalHelper.reset();
}

int MediaFolderTreeEntity::rowCount() const
{
    return static_cast<int>(m_entries.size());
}

AbstractEntity* MediaFolderTreeEntity::childEntity(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
        return nullptr;

    const auto& entry = m_entries[row];
    return entry->isDirectory ? entry->childEntity.get() : nullptr;
}

int MediaFolderTreeEntity::childEntityRow(const AbstractEntity* entity) const
{
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
    {
        if (m_entries[i]->isDirectory && m_entries[i]->childEntity.get() == entity)
            return i;
    }
    return -1;
}

QVariant MediaFolderTreeEntity::data(int row, int role) const
{
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
        return QVariant();

    const auto& entry = m_entries[row];
    return entry->headerItem ? entry->headerItem->data(role) : QVariant();
}

Qt::ItemFlags MediaFolderTreeEntity::flags(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
        return Qt::NoItemFlags;

    const auto& entry = m_entries[row];
    if (!entry->isDirectory)
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

bool MediaFolderTreeEntity::isPlanar() const
{
    return false;
}

// --------------------------------------------------------------------------
// Tree building helpers - scan filesystem directly

void MediaFolderTreeEntity::rebuildTree()
{
    // Prevent concurrent rebuild calls
    if (m_rebuilding)
        return;
    m_rebuilding = true;

    auto guard = qScopeGuard([this] { m_rebuilding = false; });

    auto mapping = modelMapping();
    if (!mapping)
        return;

    // Remove all existing rows from the model first
    const int oldRowCount = static_cast<int>(m_entries.size());
    if (oldRowCount > 0)
    {
        mapping->beginRemoveRows(0, oldRowCount - 1);
        m_entries.clear();
        mapping->endRemoveRows();
    }

    // Build new entries
    std::vector<std::unique_ptr<Entry>> newEntries;

    // Root entity: build from configured media folders
    if (m_dirPath.isEmpty())
    {
        if (!m_localSettings || !m_itemFactory)
            return;

        const QStringList mediaFolders = m_localSettings->mediaFolders();
        for (const auto& folderPath : mediaFolders)
        {
            const QFileInfo folderInfo(folderPath);
            if (!folderInfo.exists() || !folderInfo.isDir())
                continue;

            auto entry = std::make_unique<Entry>();
            entry->isDirectory = true;
            entry->dirPath = folderInfo.canonicalFilePath();
            entry->headerItem = m_itemFactory->createMediaFolderItem(
                folderInfo.canonicalFilePath());

            // Create child entity to scan this folder's contents
            auto childEntity = std::make_unique<MediaFolderTreeEntity>(
                m_itemFactory, QnUserResourcePtr(), m_resourcePool, nullptr, entry->dirPath);
            // Attach child to parent mapping so it can participate in the model
            if (childEntity->modelMapping())
                assignAsChildEntity(childEntity.get());
            entry->childEntity = std::move(childEntity);
            newEntries.push_back(std::move(entry));
        }
    }
    else
    {
        // Non-root entity: scan this directory for subdirs and files
        QDir dir(m_dirPath);
        if (!dir.exists())
            return;

        const auto entryInfos = dir.entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
            QDir::Name | QDir::IgnoreCase);

        for (const auto& info : entryInfos)
        {
            auto entry = std::make_unique<Entry>();

            if (info.isDir())
            {
                entry->isDirectory = true;
                entry->dirPath = info.canonicalFilePath();
                entry->headerItem = m_itemFactory->createSubdirectoryItem(info.fileName());

                auto childEntity = std::make_unique<MediaFolderTreeEntity>(
                    m_itemFactory, QnUserResourcePtr(), m_resourcePool, nullptr, entry->dirPath);
                if (childEntity->modelMapping())
                    assignAsChildEntity(childEntity.get());
                entry->childEntity = std::move(childEntity);
            }
            else if (isVideoFile(info.fileName()))
            {
                entry->isDirectory = false;
                entry->filePath = info.absoluteFilePath();
                entry->headerItem = m_itemFactory->createResourceItemForFile(
                    info.absoluteFilePath());
            }
            else
            {
                continue; // Skip non-video files
            }

            newEntries.push_back(std::move(entry));
        }
    }

    // Insert all new rows
    if (!newEntries.empty())
    {
        mapping->beginInsertRows(0, static_cast<int>(newEntries.size()) - 1);
        for (auto& entry : newEntries)
            m_entries.push_back(std::move(entry));
        mapping->endInsertRows();
    }
}

bool MediaFolderTreeEntity::isVideoFile(const QString& fileName) const
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    static const QStringList videoExtensions = {
        "avi", "mp4", "mkv", "mov", "wmv", "flv", "webm",
        "m4v", "mpg", "mpeg", "m2v", "3gp", "3g2", "f4v",
        "ts", "m2ts", "mts", "vob", "ogv", "dv", "mxf"
    };
    return videoExtensions.contains(suffix);
}

} // namespace entity_resource_tree
} // namespace nx::vms::client::desktop
