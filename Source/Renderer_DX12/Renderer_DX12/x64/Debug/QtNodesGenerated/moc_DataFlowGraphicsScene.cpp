/****************************************************************************
** Meta object code from reading C++ file 'DataFlowGraphicsScene.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../../SDKs/nodeeditor/include/QtNodes/internal/DataFlowGraphicsScene.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'DataFlowGraphicsScene.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN7QtNodes21DataFlowGraphicsSceneE_t {};
} // unnamed namespace

template <> constexpr inline auto QtNodes::DataFlowGraphicsScene::qt_create_metaobjectdata<qt_meta_tag_ZN7QtNodes21DataFlowGraphicsSceneE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QtNodes::DataFlowGraphicsScene",
        "sceneLoaded",
        "",
        "save",
        "load"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'sceneLoaded'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'save'
        QtMocHelpers::SlotData<bool() const>(3, 2, QMC::AccessPublic, QMetaType::Bool),
        // Slot 'load'
        QtMocHelpers::SlotData<bool()>(4, 2, QMC::AccessPublic, QMetaType::Bool),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<DataFlowGraphicsScene, qt_meta_tag_ZN7QtNodes21DataFlowGraphicsSceneE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject QtNodes::DataFlowGraphicsScene::staticMetaObject = { {
    QMetaObject::SuperData::link<BasicGraphicsScene::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes21DataFlowGraphicsSceneE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes21DataFlowGraphicsSceneE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN7QtNodes21DataFlowGraphicsSceneE_t>.metaTypes,
    nullptr
} };

void QtNodes::DataFlowGraphicsScene::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<DataFlowGraphicsScene *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->sceneLoaded(); break;
        case 1: { bool _r = _t->save();
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 2: { bool _r = _t->load();
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (DataFlowGraphicsScene::*)()>(_a, &DataFlowGraphicsScene::sceneLoaded, 0))
            return;
    }
}

const QMetaObject *QtNodes::DataFlowGraphicsScene::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QtNodes::DataFlowGraphicsScene::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7QtNodes21DataFlowGraphicsSceneE_t>.strings))
        return static_cast<void*>(this);
    return BasicGraphicsScene::qt_metacast(_clname);
}

int QtNodes::DataFlowGraphicsScene::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = BasicGraphicsScene::qt_metacall(_c, _id, _a);
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
void QtNodes::DataFlowGraphicsScene::sceneLoaded()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}
QT_WARNING_POP
