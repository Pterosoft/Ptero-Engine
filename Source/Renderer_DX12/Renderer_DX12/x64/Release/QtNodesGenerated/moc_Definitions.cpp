/****************************************************************************
** Meta object code from reading C++ file 'Definitions.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../../SDKs/nodeeditor/include/QtNodes/internal/Definitions.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'Definitions.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN7QtNodesE_t {};
} // unnamed namespace

template <> constexpr inline auto QtNodes::qt_create_metaobjectdata<qt_meta_tag_ZN7QtNodesE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QtNodes",
        "NodeRole",
        "Type",
        "Position",
        "Size",
        "CaptionVisible",
        "Caption",
        "Style",
        "InternalData",
        "InPortCount",
        "OutPortCount",
        "Widget",
        "ValidationState",
        "ProcessingStatus",
        "NodeFlags",
        "NodeFlag",
        "NoFlags",
        "Resizable",
        "Locked",
        "PortRole",
        "Data",
        "DataType",
        "ConnectionPolicyRole",
        "ConnectionPolicy",
        "One",
        "Many",
        "PortType",
        "In",
        "Out",
        "None"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
        // enum 'NodeRole'
        QtMocHelpers::EnumData<NodeRole>(1, 1, QMC::EnumIsScoped).add({
            {    2, NodeRole::Type },
            {    3, NodeRole::Position },
            {    4, NodeRole::Size },
            {    5, NodeRole::CaptionVisible },
            {    6, NodeRole::Caption },
            {    7, NodeRole::Style },
            {    8, NodeRole::InternalData },
            {    9, NodeRole::InPortCount },
            {   10, NodeRole::OutPortCount },
            {   11, NodeRole::Widget },
            {   12, NodeRole::ValidationState },
            {   13, NodeRole::ProcessingStatus },
        }),
        // flag 'NodeFlags'
        QtMocHelpers::EnumData<NodeFlags>(14, 15, QMC::EnumIsFlag).add({
            {   16, NodeFlag::NoFlags },
            {   17, NodeFlag::Resizable },
            {   18, NodeFlag::Locked },
        }),
        // enum 'PortRole'
        QtMocHelpers::EnumData<PortRole>(19, 19, QMC::EnumIsScoped).add({
            {   20, PortRole::Data },
            {   21, PortRole::DataType },
            {   22, PortRole::ConnectionPolicyRole },
            {    5, PortRole::CaptionVisible },
            {    6, PortRole::Caption },
        }),
        // enum 'ConnectionPolicy'
        QtMocHelpers::EnumData<ConnectionPolicy>(23, 23, QMC::EnumIsScoped).add({
            {   24, ConnectionPolicy::One },
            {   25, ConnectionPolicy::Many },
        }),
        // enum 'PortType'
        QtMocHelpers::EnumData<PortType>(26, 26, QMC::EnumIsScoped).add({
            {   27, PortType::In },
            {   28, PortType::Out },
            {   29, PortType::None },
        }),
    };
    return QtMocHelpers::metaObjectData<void, qt_meta_tag_ZN7QtNodesE_t>(QMC::PropertyAccessInStaticMetaCall, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}

static constexpr auto qt_staticMetaObjectContent_ZN7QtNodesE =
    QtNodes::qt_create_metaobjectdata<qt_meta_tag_ZN7QtNodesE_t>();
static constexpr auto qt_staticMetaObjectStaticContent_ZN7QtNodesE =
    qt_staticMetaObjectContent_ZN7QtNodesE.staticData;
static constexpr auto qt_staticMetaObjectRelocatingContent_ZN7QtNodesE =
    qt_staticMetaObjectContent_ZN7QtNodesE.relocatingData;

Q_CONSTINIT const QMetaObject QtNodes::staticMetaObject = { {
    nullptr,
    qt_staticMetaObjectStaticContent_ZN7QtNodesE.stringdata,
    qt_staticMetaObjectStaticContent_ZN7QtNodesE.data,
    nullptr,
    nullptr,
    qt_staticMetaObjectRelocatingContent_ZN7QtNodesE.metaTypes,
    nullptr
} };

QT_WARNING_POP
