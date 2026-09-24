/****************************************************************************
** Meta object code from reading C++ file 'NodeDelegateModel.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../../SDKs/nodeeditor/include/QtNodes/internal/NodeDelegateModel.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'NodeDelegateModel.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN7QtNodes17NodeDelegateModelE_t {};
} // unnamed namespace

template <> constexpr inline auto QtNodes::NodeDelegateModel::qt_create_metaobjectdata<qt_meta_tag_ZN7QtNodes17NodeDelegateModelE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QtNodes::NodeDelegateModel",
        "dataUpdated",
        "",
        "PortIndex",
        "index",
        "dataInvalidated",
        "computingStarted",
        "computingFinished",
        "embeddedWidgetSizeUpdated",
        "requestNodeUpdate",
        "portsAboutToBeDeleted",
        "PortType",
        "portType",
        "first",
        "last",
        "portsDeleted",
        "portsAboutToBeInserted",
        "portsInserted",
        "inputConnectionCreated",
        "ConnectionId",
        "inputConnectionDeleted",
        "outputConnectionCreated",
        "outputConnectionDeleted"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'dataUpdated'
        QtMocHelpers::SignalData<void(PortIndex const)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'dataInvalidated'
        QtMocHelpers::SignalData<void(PortIndex const)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'computingStarted'
        QtMocHelpers::SignalData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'computingFinished'
        QtMocHelpers::SignalData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'embeddedWidgetSizeUpdated'
        QtMocHelpers::SignalData<void()>(8, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'requestNodeUpdate'
        QtMocHelpers::SignalData<void()>(9, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'portsAboutToBeDeleted'
        QtMocHelpers::SignalData<void(PortType const, PortIndex const, PortIndex const)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 11, 12 }, { 0x80000000 | 3, 13 }, { 0x80000000 | 3, 14 },
        }}),
        // Signal 'portsDeleted'
        QtMocHelpers::SignalData<void()>(15, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'portsAboutToBeInserted'
        QtMocHelpers::SignalData<void(PortType const, PortIndex const, PortIndex const)>(16, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 11, 12 }, { 0x80000000 | 3, 13 }, { 0x80000000 | 3, 14 },
        }}),
        // Signal 'portsInserted'
        QtMocHelpers::SignalData<void()>(17, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'inputConnectionCreated'
        QtMocHelpers::SlotData<void(ConnectionId const &)>(18, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 19, 2 },
        }}),
        // Slot 'inputConnectionDeleted'
        QtMocHelpers::SlotData<void(ConnectionId const &)>(20, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 19, 2 },
        }}),
        // Slot 'outputConnectionCreated'
        QtMocHelpers::SlotData<void(ConnectionId const &)>(21, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 19, 2 },
        }}),
        // Slot 'outputConnectionDeleted'
        QtMocHelpers::SlotData<void(ConnectionId const &)>(22, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 19, 2 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<NodeDelegateModel, qt_meta_tag_ZN7QtNodes17NodeDelegateModelE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject QtNodes::NodeDelegateModel::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes17NodeDelegateModelE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes17NodeDelegateModelE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN7QtNodes17NodeDelegateModelE_t>.metaTypes,
    nullptr
} };

void QtNodes::NodeDelegateModel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<NodeDelegateModel *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->dataUpdated((*reinterpret_cast<std::add_pointer_t<PortIndex>>(_a[1]))); break;
        case 1: _t->dataInvalidated((*reinterpret_cast<std::add_pointer_t<PortIndex>>(_a[1]))); break;
        case 2: _t->computingStarted(); break;
        case 3: _t->computingFinished(); break;
        case 4: _t->embeddedWidgetSizeUpdated(); break;
        case 5: _t->requestNodeUpdate(); break;
        case 6: _t->portsAboutToBeDeleted((*reinterpret_cast<std::add_pointer_t<PortType>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<PortIndex>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<PortIndex>>(_a[3]))); break;
        case 7: _t->portsDeleted(); break;
        case 8: _t->portsAboutToBeInserted((*reinterpret_cast<std::add_pointer_t<PortType>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<PortIndex>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<PortIndex>>(_a[3]))); break;
        case 9: _t->portsInserted(); break;
        case 10: _t->inputConnectionCreated((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1]))); break;
        case 11: _t->inputConnectionDeleted((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1]))); break;
        case 12: _t->outputConnectionCreated((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1]))); break;
        case 13: _t->outputConnectionDeleted((*reinterpret_cast<std::add_pointer_t<ConnectionId>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)(PortIndex const )>(_a, &NodeDelegateModel::dataUpdated, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)(PortIndex const )>(_a, &NodeDelegateModel::dataInvalidated, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)()>(_a, &NodeDelegateModel::computingStarted, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)()>(_a, &NodeDelegateModel::computingFinished, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)()>(_a, &NodeDelegateModel::embeddedWidgetSizeUpdated, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)()>(_a, &NodeDelegateModel::requestNodeUpdate, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)(PortType const , PortIndex const , PortIndex const )>(_a, &NodeDelegateModel::portsAboutToBeDeleted, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)()>(_a, &NodeDelegateModel::portsDeleted, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)(PortType const , PortIndex const , PortIndex const )>(_a, &NodeDelegateModel::portsAboutToBeInserted, 8))
            return;
        if (QtMocHelpers::indexOfMethod<void (NodeDelegateModel::*)()>(_a, &NodeDelegateModel::portsInserted, 9))
            return;
    }
}

const QMetaObject *QtNodes::NodeDelegateModel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QtNodes::NodeDelegateModel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes17NodeDelegateModelE_t>.strings))
        return static_cast<void*>(this);
    if (!strcmp(_clname, "Serializable"))
        return static_cast< Serializable*>(this);
    return QObject::qt_metacast(_clname);
}

int QtNodes::NodeDelegateModel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 14)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 14;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 14)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 14;
    }
    return _id;
}

// SIGNAL 0
void QtNodes::NodeDelegateModel::dataUpdated(PortIndex const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void QtNodes::NodeDelegateModel::dataInvalidated(PortIndex const _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void QtNodes::NodeDelegateModel::computingStarted()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void QtNodes::NodeDelegateModel::computingFinished()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void QtNodes::NodeDelegateModel::embeddedWidgetSizeUpdated()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void QtNodes::NodeDelegateModel::requestNodeUpdate()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void QtNodes::NodeDelegateModel::portsAboutToBeDeleted(PortType const _t1, PortIndex const _t2, PortIndex const _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1, _t2, _t3);
}

// SIGNAL 7
void QtNodes::NodeDelegateModel::portsDeleted()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void QtNodes::NodeDelegateModel::portsAboutToBeInserted(PortType const _t1, PortIndex const _t2, PortIndex const _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 8, nullptr, _t1, _t2, _t3);
}

// SIGNAL 9
void QtNodes::NodeDelegateModel::portsInserted()
{
    QMetaObject::activate(this, &staticMetaObject, 9, nullptr);
}
QT_WARNING_POP
