// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <memory>
#include <vector>

#include <QtCore/QString>

#include <core/resource/resource_fwd.h>
#include <nx/vms/client/desktop/resource_views/entity_item_model/entity/abstract_entity.h>
#include <nx/vms/client/desktop/resource_views/entity_item_model/item/abstract_item.h>

class QnResourcePool;

namespace nx::vms::client::desktop {

class LocalSettings;

namespace entity_resource_tree {

class ResourceTreeItemFactory;

class MediaFolderTreeEntity: public entity_item_model::AbstractEntity
{
    using base_type = entity_item_model::AbstractEntity;
    using AbstractItemPtr = entity_item_model::AbstractItemPtr;

public:
    /**
     * @param itemFactory Factory for creating tree items.
     * @param resourcePool Resource pool for creating local media resources.
     * @param localSettings Local settings containing media folder paths.
     * @param dirPath Optional directory path for non-root entities. Empty for root.
     */
    MediaFolderTreeEntity(
        ResourceTreeItemFactory* itemFactory,
        const QnUserResourcePtr& user,
        QnResourcePool* resourcePool,
        LocalSettings* localSettings,
        const QString& dirPath = QString());

    virtual ~MediaFolderTreeEntity() override;

    virtual int rowCount() const override;
    virtual AbstractEntity* childEntity(int row) const override;
    virtual int childEntityRow(const AbstractEntity* entity) const override;
    virtual QVariant data(int row, int role = Qt::DisplayRole) const override;
    virtual Qt::ItemFlags flags(int row) const override;
    virtual bool isPlanar() const override;

private:
    struct Entry;
    struct SignalHelper;

    void rebuildTree();
    bool isVideoFile(const QString& fileName) const;

    ResourceTreeItemFactory* const m_itemFactory;
    QnResourcePool* const m_resourcePool;
    LocalSettings* const m_localSettings;
    QString m_dirPath; // Empty for root entity, set directory path for child entities

    std::vector<std::unique_ptr<Entry>> m_entries;
    std::unique_ptr<SignalHelper> m_signalHelper;
    bool m_rebuilding = false; // Prevents concurrent rebuild calls
};

} // namespace entity_resource_tree
} // namespace nx::vms::client::desktop
