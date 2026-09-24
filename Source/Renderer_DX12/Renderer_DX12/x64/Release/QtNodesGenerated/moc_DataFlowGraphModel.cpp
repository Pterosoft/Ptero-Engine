/****************************************************************************
** Meta object code from reading C++ file 'DataFlowGraphModel.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../../SDKs/nodeeditor/include/QtNodes/internal/DataFlowGraphModel.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'DataFlowGraphModel.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN7QtNodes18DataFlowGraphModelE_t {};
} // unnamed namespace

template <> constexpr inline auto QtNodes::DataFlowGraphModel::qt_create_metaobjectdata<qt_meta_tag_ZN7QtNodes18DataFlowGraphModelE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QtNodes::DataFlowGraphModel",
        "inPortDataWasSet",
        "",
        "NodeId",
        "PortType",
        "PortIndex",
        "onOutPortDataUpdated",
        "nodeId",
        "portIndex",
        "propagateEmptyDataTo"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'inPortDataWasSet'
        QtMocHelpers::SignalData<void(NodeId const, PortType const, PortIndex const)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 2 }, { 0x80000000 | 4, 2 }, { 0x80000000 | 5, 2 },
        }}),
        // Slot 'onOutPortDataUpdated'
        QtMocHelpers::SlotData<void(NodeId const, PortIndex const)>(6, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 3, 7 }, { 0x80000000 | 5, 8 },
        }}),
        // Slot 'propagateEmptyDataTo'
        QtMocHelpers::SlotData<void(NodeId const, PortIndex const)>(9, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 3, 7 }, { 0x80000000 | 5, 8 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<DataFlowGraphModel, qt_meta_tag_ZN7QtNodes18DataFlowGraphModelE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject QtNodes::DataFlowGraphModel::staticMetaObject = { {
    QMetaObject::SuperData::link<AbstractGraphModel::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes18DataFlowGraphModelE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes18DataFlowGraphModelE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN7QtNodes18DataFlowGraphModelE_t>.metaTypes,
    nullptr
} };

void QtNodes::DataFlowGraphModel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<DataFlowGraphModel *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->inPortDataWasSet((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<PortType>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<PortIndex>>(_a[3]))); break;
        case 1: _t->onOutPortDataUpdated((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<PortIndex>>(_a[2]))); break;
        case 2: _t->propagateEmptyDataTo((*reinterpret_cast<std::add_pointer_t<NodeId>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<PortIndex>>(_a[2]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (DataFlowGraphModel::*)(NodeId const , PortType const , PortIndex const )>(_a, &DataFlowGraphModel::inPortDataWasSet, 0))
            return;
    }
}

const QMetaObject *QtNodes::DataFlowGraphModel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QtNodes::DataFlowGraphModel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes18DataFlowGraphModelE_t>.strings))
        return static_cast<void*>(this);
    if (!strcmp(_clname, "Serializable"))
        return static_cast< Serializable*>(this);
    return AbstractGraphModel::qt_metacast(_clname);
}

int QtNodes::DataFlowGraphModel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = AbstractGraphModel::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 3)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 3;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 3)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 3;
    }
    return _id;
}

// SIGNAL 0
void QtNodes::DataFlowGraphModel::inPortDataWasSet(NodeId const _t1, PortType const _t2, PortIndex const _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2, _t3);
}
QT_WARNING_POP
