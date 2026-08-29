#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class ObjectRegistry final : public QObject {
    Q_OBJECT

public:
    explicit ObjectRegistry(QObject* root, QObject* parent = nullptr);

    [[nodiscard]] QObject* root() const noexcept { return m_root.data(); }
    [[nodiscard]] quint64 idFor(QObject* object);
    [[nodiscard]] QObject* byId(quint64 id) const;
    [[nodiscard]] QObject* byObjectName(const QString& name) const;

    [[nodiscard]] QJsonObject summary(QObject* object);
    [[nodiscard]] QJsonObject describe(QObject* object, bool includeValues = true);
    [[nodiscard]] QJsonObject tree(QObject* root = nullptr, int maxDepth = 8);
    [[nodiscard]] QJsonArray flatList(QObject* root = nullptr);

    [[nodiscard]] static QJsonValue variantToJson(const QVariant& value);
    [[nodiscard]] static bool jsonToPropertyValue(const QJsonValue& json,
                                                  const QMetaProperty& property,
                                                  QVariant& value,
                                                  QString& error);

private:
    [[nodiscard]] QJsonObject treeNode(QObject* object, int depth, int maxDepth);
    void collectFlat(QObject* object, QJsonArray& output);

    QPointer<QObject> m_root;
    quint64 m_nextId = 1;
    QHash<QObject*, quint64> m_objectToId;
    QHash<quint64, QPointer<QObject>> m_idToObject;
};
