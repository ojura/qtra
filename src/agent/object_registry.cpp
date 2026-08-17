#include "agent/object_registry.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QThread>
#include <QVariant>

namespace {

QString methodTypeName(const QMetaMethod::MethodType type)
{
    switch (type) {
    case QMetaMethod::Method: return QStringLiteral("method");
    case QMetaMethod::Signal: return QStringLiteral("signal");
    case QMetaMethod::Slot: return QStringLiteral("slot");
    case QMetaMethod::Constructor: return QStringLiteral("constructor");
    }
    return QStringLiteral("unknown");
}

QString accessName(const QMetaMethod::Access access)
{
    switch (access) {
    case QMetaMethod::Private: return QStringLiteral("private");
    case QMetaMethod::Protected: return QStringLiteral("protected");
    case QMetaMethod::Public: return QStringLiteral("public");
    }
    return QStringLiteral("unknown");
}

QString pointerString(const void* pointer)
{
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(pointer), 0, 16);
}

} // namespace

ObjectRegistry::ObjectRegistry(QObject* root, QObject* parent)
    : QObject(parent)
    , m_root(root)
{
    if (m_root != nullptr) {
        (void)idFor(m_root);
    }
}

quint64 ObjectRegistry::idFor(QObject* object)
{
    if (object == nullptr) {
        return 0;
    }
    if (const auto iterator = m_objectToId.constFind(object);
        iterator != m_objectToId.constEnd()) {
        return iterator.value();
    }

    const quint64 id = m_nextId++;
    m_objectToId.insert(object, id);
    m_idToObject.insert(id, object);
    connect(object, &QObject::destroyed, this, [this, object, id] {
        m_objectToId.remove(object);
        m_idToObject.remove(id);
    });
    return id;
}

QObject* ObjectRegistry::byId(const quint64 id) const
{
    const auto iterator = m_idToObject.constFind(id);
    return iterator == m_idToObject.constEnd() ? nullptr : iterator.value().data();
}

QObject* ObjectRegistry::byObjectName(const QString& name) const
{
    if (name.isEmpty() || m_root == nullptr) {
        return nullptr;
    }
    if (m_root->objectName() == name) {
        return m_root;
    }
    return m_root->findChild<QObject*>(name, Qt::FindChildrenRecursively);
}

QJsonObject ObjectRegistry::summary(QObject* object)
{
    if (object == nullptr) {
        return {};
    }

    QJsonObject result{
        {QStringLiteral("id"), QString::number(idFor(object))},
        {QStringLiteral("address"), pointerString(object)},
        {QStringLiteral("class"), QString::fromLatin1(object->metaObject()->className())},
        {QStringLiteral("objectName"), object->objectName()},
        {QStringLiteral("threadAddress"), pointerString(object->thread())},
        {QStringLiteral("childCount"), object->children().size()},
    };
    if (object->parent() != nullptr) {
        result.insert(QStringLiteral("parentId"), QString::number(idFor(object->parent())));
    }
    return result;
}

QJsonObject ObjectRegistry::describe(QObject* object, const bool includeValues)
{
    if (object == nullptr) {
        return {};
    }

    QJsonObject result = summary(object);
    const QMetaObject* metaObject = object->metaObject();

    QJsonArray inheritance;
    for (const QMetaObject* current = metaObject; current != nullptr; current = current->superClass()) {
        inheritance.append(QString::fromLatin1(current->className()));
    }
    result.insert(QStringLiteral("inheritance"), inheritance);

    QJsonArray properties;
    for (int index = 0; index < metaObject->propertyCount(); ++index) {
        const QMetaProperty property = metaObject->property(index);
        const char* propertyType = property.typeName();
        QJsonObject item{
            {QStringLiteral("name"), QString::fromLatin1(property.name())},
            {QStringLiteral("type"), QString::fromLatin1(
                propertyType != nullptr ? propertyType : "")},
            {QStringLiteral("readable"), property.isReadable()},
            {QStringLiteral("writable"), property.isWritable()},
            {QStringLiteral("resettable"), property.isResettable()},
            {QStringLiteral("constant"), property.isConstant()},
            {QStringLiteral("final"), property.isFinal()},
        };
        if (includeValues && property.isReadable()) {
            item.insert(QStringLiteral("value"), variantToJson(property.read(object)));
        }
        properties.append(item);
    }
    result.insert(QStringLiteral("properties"), properties);

    QJsonArray methods;
    for (int index = 0; index < metaObject->methodCount(); ++index) {
        const QMetaMethod method = metaObject->method(index);
        const char* returnType = method.typeName();
        QJsonArray parameterTypes;
        for (const QByteArray& type : method.parameterTypes()) {
            parameterTypes.append(QString::fromLatin1(type));
        }
        methods.append(QJsonObject{
            {QStringLiteral("index"), index},
            {QStringLiteral("name"), QString::fromLatin1(method.name())},
            {QStringLiteral("signature"), QString::fromLatin1(method.methodSignature())},
            {QStringLiteral("returnType"), QString::fromLatin1(
                returnType != nullptr ? returnType : "")},
            {QStringLiteral("parameterTypes"), parameterTypes},
            {QStringLiteral("kind"), methodTypeName(method.methodType())},
            {QStringLiteral("access"), accessName(method.access())},
        });
    }
    result.insert(QStringLiteral("methods"), methods);

    QJsonObject dynamicProperties;
    for (const QByteArray& name : object->dynamicPropertyNames()) {
        dynamicProperties.insert(QString::fromLatin1(name),
                                 variantToJson(object->property(name.constData())));
    }
    result.insert(QStringLiteral("dynamicProperties"), dynamicProperties);
    return result;
}

QJsonObject ObjectRegistry::tree(QObject* root, const int maxDepth)
{
    if (root == nullptr) {
        root = m_root;
    }
    return treeNode(root, 0, qBound(0, maxDepth, 64));
}

QJsonArray ObjectRegistry::flatList(QObject* root)
{
    if (root == nullptr) {
        root = m_root;
    }
    QJsonArray output;
    collectFlat(root, output);
    return output;
}

QJsonValue ObjectRegistry::variantToJson(const QVariant& value)
{
    if (!value.isValid()) {
        return QJsonValue();
    }

    QJsonValue json = QJsonValue::fromVariant(value);
    if (!json.isUndefined()) {
        return json;
    }

    QString representation;
    QDebug debug(&representation);
    debug.noquote().nospace() << value;
    return QJsonObject{
        {QStringLiteral("type"), QString::fromLatin1(value.typeName() != nullptr ? value.typeName() : "unknown")},
        {QStringLiteral("repr"), representation},
    };
}

bool ObjectRegistry::jsonToPropertyValue(const QJsonValue& json,
                                         const QMetaProperty& property,
                                         QVariant& value,
                                         QString& error)
{
    value = json.toVariant();
    const QMetaType targetType = property.metaType();
    if (!targetType.isValid()) {
        error = QStringLiteral("property %1 has no valid metatype")
                    .arg(QString::fromLatin1(property.name()));
        return false;
    }

    if (value.metaType() != targetType && !value.convert(targetType)) {
        error = QStringLiteral("cannot convert JSON value to %1")
                    .arg(QString::fromLatin1(property.typeName()));
        return false;
    }
    return true;
}

QJsonObject ObjectRegistry::treeNode(QObject* object, const int depth, const int maxDepth)
{
    if (object == nullptr) {
        return {};
    }
    QJsonObject result = summary(object);
    if (depth >= maxDepth) {
        result.insert(QStringLiteral("childrenTruncated"), !object->children().isEmpty());
        return result;
    }

    QJsonArray children;
    for (QObject* child : object->children()) {
        children.append(treeNode(child, depth + 1, maxDepth));
    }
    result.insert(QStringLiteral("children"), children);
    return result;
}

void ObjectRegistry::collectFlat(QObject* object, QJsonArray& output)
{
    if (object == nullptr) {
        return;
    }
    output.append(summary(object));
    for (QObject* child : object->children()) {
        collectFlat(child, output);
    }
}
