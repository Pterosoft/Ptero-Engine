/****************************************************************************
** Meta object code from reading C++ file 'AbstractGraphModel.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../../SDKs/nodeeditor/include/QtNodes/internal/AbstractGraphModel.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'AbstractGraphModel.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN7QtNodes18AbstractGraphModelE_t {};
} // unnamed namespace

template <> constexpr inline auto QtNodes::AbstractGraphModel::qt_create_metaobjectdata<qt_meta_tag_ZN7QtNodes18AbstractGraphModelE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QtNodes::AbstractGraphModel",
        "connectionCreated",
        "",
        "ConnectionId",
        "connectionId",
        "connectionDeleted",
        "nodeCreated",
        "NodeId",
        "nodeId",
        "nodeDeleted",
        "nodeUpdated",
        "nodeFlagsUpdated",
        "nodePositionUpdated",
        "modelReset"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'connectionCreated'
        QtMocHelpers::SignalData<void(ConnectionId const)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'connectionDeleted'
        QtMocHelpers::SignalData<void(ConnectionId const)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'nodeCreated'
        QtMocHelpers::SignalData<void(NodeId const)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 7, 8 },
        }}),
        // Signal 'nodeDeleted'
        QtMocHelpers::SignalData<void(NodeId const)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 7, 8 },
        }}),
        // Signal 'nodeUpdated'
        QtMocHelpers::SignalData<void(NodeId const)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 7, 8 },
        }}),
        // Signal 'nodeFlagsUpdated'
        QtMocHelpers::SignalData<void(NodeId const)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 7, 8 },
        }}),
        // Signal 'nodePositionUpdated'
        QtMocHelpers::SignalData<void(NodeId const)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 7, 8 },
        }}),
        // Signal 'modelReset'
        QtMocHelpers::SignalData<void()>(13, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<AbstractGraphModel, qt_meta_tag_ZN7QtNodes18AbstractGraphModelE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject QtNodes::AbstractGraphModel::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes18AbstractGraphModelE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes18AbstractGraphModelE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN7QtNodes18AbstractGraphModelE_t>.metaTypes,
    nullptr
} };

void QtNodes::AbstractGraphModel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<AbstractGraphModel *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->connectionCreated((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1]))); break;
        case 1: _t->connectionDeleted((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1]))); break;
        case 2: _t->nodeCreated((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 3: _t->nodeDeleted((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 4: _t->nodeUpdated((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 5: _t->nodeFlagsUpdated((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 6: _t->nodePositionUpdated((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 7: _t->modelReset(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (AbstractGraphModel::*)(ConnectionId const )>(_a, &AbstractGraphModel::connectionCreated, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (AbstractGraphModel::*)(ConnectionId const )>(_a, &AbstractGraphModel::connectionDeleted, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (AbstractGraphModel::*)(NodeId const )>(_a, &AbstractGraphModel::nodeCreated, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (AbstractGraphModel::*)(NodeId const )>(_a, &AbstractGraphModel::nodeDeleted, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (AbstractGraphModel::*)(NodeId const )>(_a, &AbstractGraphModel::nodeUpdated, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (AbstractGraphModel::*)(NodeId const )>(_a, &AbstractGraphModel::nodeFlagsUpdated, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (AbstractGraphModel::*)(NodeId const )>(_a, &AbstractGraphModel::nodePositionUpdated, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (AbstractGraphModel::*)()>(_a, &AbstractGraphModel::modelReset, 7))
            return;
    }
}

const QMetaObject *QtNodes::AbstractGraphModel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QtNodes::AbstractGraphModel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes18AbstractGraphModelE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int QtNodes::AbstractGraphModel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 8)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 8;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 8)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 8;
    }
    return _id;
}

// SIGNAL 0
void QtNodes::AbstractGraphModel::connectionCreated(ConnectionId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void QtNodes::AbstractGraphModel::connectionDeleted(ConnectionId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void QtNodes::AbstractGraphModel::nodeCreated(NodeId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void QtNodes::AbstractGraphModel::nodeDeleted(NodeId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void QtNodes::AbstractGraphModel::nodeUpdated(NodeId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void QtNodes::AbstractGraphModel::nodeFlagsUpdated(NodeId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1);
}

// SIGNAL 6
void QtNodes::AbstractGraphModel::nodePositionUpdated(NodeId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}

// SIGNAL 7
void QtNodes::AbstractGraphModel::modelReset()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}
QT_WARNING_POP
