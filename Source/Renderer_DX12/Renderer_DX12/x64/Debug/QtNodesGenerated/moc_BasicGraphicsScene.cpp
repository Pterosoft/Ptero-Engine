/****************************************************************************
** Meta object code from reading C++ file 'BasicGraphicsScene.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../../SDKs/nodeeditor/include/QtNodes/internal/BasicGraphicsScene.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'BasicGraphicsScene.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN7QtNodes18BasicGraphicsSceneE_t {};
} // unnamed namespace

template <> constexpr inline auto QtNodes::BasicGraphicsScene::qt_create_metaobjectdata<qt_meta_tag_ZN7QtNodes18BasicGraphicsSceneE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QtNodes::BasicGraphicsScene",
        "modified",
        "",
        "BasicGraphicsScene*",
        "nodeMoved",
        "NodeId",
        "nodeId",
        "QPointF",
        "newLocation",
        "nodeClicked",
        "nodeSelected",
        "nodeDoubleClicked",
        "nodeHovered",
        "QPoint",
        "screenPos",
        "nodeHoverLeft",
        "connectionHovered",
        "ConnectionId",
        "connectionId",
        "connectionHoverLeft",
        "nodeContextMenu",
        "pos",
        "zoomFitAllClicked",
        "zoomFitSelectedClicked",
        "onConnectionDeleted",
        "onConnectionCreated",
        "onNodeDeleted",
        "onNodeCreated",
        "onNodePositionUpdated",
        "onNodeUpdated",
        "onNodeClicked",
        "onModelReset"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'modified'
        QtMocHelpers::SignalData<void(BasicGraphicsScene *)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 2 },
        }}),
        // Signal 'nodeMoved'
        QtMocHelpers::SignalData<void(NodeId const, QPointF const &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 }, { 0x80000000 | 7, 8 },
        }}),
        // Signal 'nodeClicked'
        QtMocHelpers::SignalData<void(NodeId const)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Signal 'nodeSelected'
        QtMocHelpers::SignalData<void(NodeId const)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Signal 'nodeDoubleClicked'
        QtMocHelpers::SignalData<void(NodeId const)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Signal 'nodeHovered'
        QtMocHelpers::SignalData<void(NodeId const, QPoint const)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 }, { 0x80000000 | 13, 14 },
        }}),
        // Signal 'nodeHoverLeft'
        QtMocHelpers::SignalData<void(NodeId const)>(15, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Signal 'connectionHovered'
        QtMocHelpers::SignalData<void(ConnectionId const, QPoint const)>(16, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 17, 18 }, { 0x80000000 | 13, 14 },
        }}),
        // Signal 'connectionHoverLeft'
        QtMocHelpers::SignalData<void(ConnectionId const)>(19, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 17, 18 },
        }}),
        // Signal 'nodeContextMenu'
        QtMocHelpers::SignalData<void(NodeId const, QPointF const)>(20, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 }, { 0x80000000 | 7, 21 },
        }}),
        // Signal 'zoomFitAllClicked'
        QtMocHelpers::SignalData<void()>(22, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'zoomFitSelectedClicked'
        QtMocHelpers::SignalData<void()>(23, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onConnectionDeleted'
        QtMocHelpers::SlotData<void(ConnectionId const)>(24, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 17, 18 },
        }}),
        // Slot 'onConnectionCreated'
        QtMocHelpers::SlotData<void(ConnectionId const)>(25, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 17, 18 },
        }}),
        // Slot 'onNodeDeleted'
        QtMocHelpers::SlotData<void(NodeId const)>(26, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Slot 'onNodeCreated'
        QtMocHelpers::SlotData<void(NodeId const)>(27, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Slot 'onNodePositionUpdated'
        QtMocHelpers::SlotData<void(NodeId const)>(28, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Slot 'onNodeUpdated'
        QtMocHelpers::SlotData<void(NodeId const)>(29, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Slot 'onNodeClicked'
        QtMocHelpers::SlotData<void(NodeId const)>(30, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Slot 'onModelReset'
        QtMocHelpers::SlotData<void()>(31, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<BasicGraphicsScene, qt_meta_tag_ZN7QtNodes18BasicGraphicsSceneE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject QtNodes::BasicGraphicsScene::staticMetaObject = { {
    QMetaObject::SuperData::link<QGraphicsScene::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes18BasicGraphicsSceneE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes18BasicGraphicsSceneE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN7QtNodes18BasicGraphicsSceneE_t>.metaTypes,
    nullptr
} };

void QtNodes::BasicGraphicsScene::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<BasicGraphicsScene *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->modified((*reinterpret_cast<std::add_pointer_t<BasicGraphicsScene*>>(_a[1]))); break;
        case 1: _t->nodeMoved((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QPointF>>(_a[2]))); break;
        case 2: _t->nodeClicked((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 3: _t->nodeSelected((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 4: _t->nodeDoubleClicked((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 5: _t->nodeHovered((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QPoint>>(_a[2]))); break;
        case 6: _t->nodeHoverLeft((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 7: _t->connectionHovered((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QPoint>>(_a[2]))); break;
        case 8: _t->connectionHoverLeft((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1]))); break;
        case 9: _t->nodeContextMenu((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QPointF>>(_a[2]))); break;
        case 10: _t->zoomFitAllClicked(); break;
        case 11: _t->zoomFitSelectedClicked(); break;
        case 12: _t->onConnectionDeleted((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1]))); break;
        case 13: _t->onConnectionCreated((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1]))); break;
        case 14: _t->onNodeDeleted((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 15: _t->onNodeCreated((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 16: _t->onNodePositionUpdated((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 17: _t->onNodeUpdated((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 18: _t->onNodeClicked((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1]))); break;
        case 19: _t->onModelReset(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
        case 0:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< BasicGraphicsScene* >(); break;
            }
            break;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(BasicGraphicsScene * )>(_a, &BasicGraphicsScene::modified, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(NodeId const , QPointF const & )>(_a, &BasicGraphicsScene::nodeMoved, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(NodeId const )>(_a, &BasicGraphicsScene::nodeClicked, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(NodeId const )>(_a, &BasicGraphicsScene::nodeSelected, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(NodeId const )>(_a, &BasicGraphicsScene::nodeDoubleClicked, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(NodeId const , QPoint const )>(_a, &BasicGraphicsScene::nodeHovered, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(NodeId const )>(_a, &BasicGraphicsScene::nodeHoverLeft, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(ConnectionId const , QPoint const )>(_a, &BasicGraphicsScene::connectionHovered, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(ConnectionId const )>(_a, &BasicGraphicsScene::connectionHoverLeft, 8))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)(NodeId const , QPointF const )>(_a, &BasicGraphicsScene::nodeContextMenu, 9))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)()>(_a, &BasicGraphicsScene::zoomFitAllClicked, 10))
            return;
        if (QtMocHelpers::indexOfMethod<void (BasicGraphicsScene::*)()>(_a, &BasicGraphicsScene::zoomFitSelectedClicked, 11))
            return;
    }
}

const QMetaObject *QtNodes::BasicGraphicsScene::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QtNodes::BasicGraphicsScene::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes18BasicGraphicsSceneE_t>.strings))
        return static_cast<void*>(this);
    return QGraphicsScene::qt_metacast(_clname);
}

int QtNodes::BasicGraphicsScene::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QGraphicsScene::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 20)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 20;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 20)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 20;
    }
    return _id;
}

// SIGNAL 0
void QtNodes::BasicGraphicsScene::modified(BasicGraphicsScene * _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void QtNodes::BasicGraphicsScene::nodeMoved(NodeId const _t1, QPointF const & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2);
}

// SIGNAL 2
void QtNodes::BasicGraphicsScene::nodeClicked(NodeId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void QtNodes::BasicGraphicsScene::nodeSelected(NodeId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void QtNodes::BasicGraphicsScene::nodeDoubleClicked(NodeId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void QtNodes::BasicGraphicsScene::nodeHovered(NodeId const _t1, QPoint const _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1, _t2);
}

// SIGNAL 6
void QtNodes::BasicGraphicsScene::nodeHoverLeft(NodeId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}

// SIGNAL 7
void QtNodes::BasicGraphicsScene::connectionHovered(ConnectionId const _t1, QPoint const _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1, _t2);
}

// SIGNAL 8
void QtNodes::BasicGraphicsScene::connectionHoverLeft(ConnectionId const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 8, nullptr, _t1);
}

// SIGNAL 9
void QtNodes::BasicGraphicsScene::nodeContextMenu(NodeId const _t1, QPointF const _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 9, nullptr, _t1, _t2);
}

// SIGNAL 10
void QtNodes::BasicGraphicsScene::zoomFitAllClicked()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void QtNodes::BasicGraphicsScene::zoomFitSelectedClicked()
{
    QMetaObject::activate(this, &staticMetaObject, 11, nullptr);
}
QT_WARNING_POP
