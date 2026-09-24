// The Qt half of the Node Graph: the window the user actually draws in.
//
// This is the only translation unit that knows about the QtNodes SDK. It builds a QtNodes
// scene out of a NodeGraphDocument and pushes every edit straight back into the document
// the engine holds, so there is never a moment where the window and the level disagree
// about what the graph is.
//
// Deliberately free of Q_OBJECT: every connection here is made with a lambda and a context
// object, which means MSBuild never has to run moc on engine code. Only the bundled SDK
// needs it, and QtNodes.props handles that.

#include "../../QtUi/QtUi.h"

#include "System/NodeGraphCatalog.h"
#include "System/NodeGraphDocument.h"
#include "System/NodeGraphEditor.h"
#include "System/NodeGraphRuntime.h"

#pragma warning(push)
#pragma warning(disable : 4996) // Qt 6.11 overrides its own deprecated event hook.
#include <QtWidgets/QtWidgets>
#include <QtCore/QJsonObject>
#include <QtCore/QPointF>
#include <QtGui/QUndoStack>
#pragma warning(pop)

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/internal/ConnectionIdHash.hpp>
#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/Definitions>
#include <QtNodes/GraphicsView>
#include <QtNodes/NodeDelegateModel>
#include <QtNodes/NodeDelegateModelRegistry>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/StyleCollection>
#include <QtNodes/UndoCommands>

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace
{
using QtNodes::ConnectionId;
using QtNodes::ConnectionPolicy;
using QtNodes::NodeDataType;
using QtNodes::NodeId;
using QtNodes::NodeRole;
using QtNodes::PortIndex;
using QtNodes::PortType;

// Drag payloads. The palette and the variable table are ordinary item views, so the only
// thing that distinguishes their drags is the format the canvas looks for.
constexpr const char* kNodeTypeMime = "application/x-ptero-nodetype";
constexpr const char* kVariableMime = "application/x-ptero-variable";

// Comment boxes are not nodes - the runtime never sees one - but they are offered in the
// palette and the node menu alongside real types, so they need an id to be dragged under.
constexpr const char* kCommentTypeId = "__comment";

// The open level's entities and the editor's selection, supplied through
// NodeGraphEditor::SetEntitySource. Either may be empty before the editor has set them.
std::function<std::vector<NodeGraphEntityInfo>()> gListEntities;
std::function<std::uint64_t()> gSelectedEntity;

// Everything the delegate models need but do not own. One instance lives in the window.
struct GraphContext
{
    std::vector<NodeGraphVariable> Variables;
    // Every function the graph defines, refreshed from the Function Entry nodes whenever
    // one is added, renamed or imported. Only the pickers read it.
    QStringList Functions;
    // Called by a delegate whenever the user changes something on a node body.
    std::function<void()> OnEdited;
};

QColor CategoryColor(const char* category)
{
    const QString name = QString::fromUtf8(category);
    if (name == "Events")
        return QColor(0x6b, 0x1f, 0x1f);
    if (name == "Flow")
        return QColor(0x33, 0x36, 0x3c);
    if (name == "UI")
        return QColor(0x1d, 0x3a, 0x5c);
    if (name == "Variables")
        return QColor(0x40, 0x2a, 0x5a);
    if (name == "Game")
        return QColor(0x4a, 0x37, 0x14);
    if (name == "Video")
        return QColor(0x14, 0x45, 0x4a);
    if (name == "Entity")
        return QColor(0x1a, 0x33, 0x55);
    if (name == "Camera")
        return QColor(0x2e, 0x2a, 0x52);
    if (name == "Audio")
        return QColor(0x4a, 0x1f, 0x3d);
    if (name == "Input")
        return QColor(0x3a, 0x3f, 0x1c);
    if (name == "Math" || name == "Logic")
        return QColor(0x1d, 0x3f, 0x35);
    return QColor(0x2b, 0x2d, 0x31);
}

// Fills a tree with the catalogue grouped by category, plus the comment box, which is not
// a catalogue entry but belongs in the same list. Shared by the palette dock and the node
// menu on Q so the two can never drift apart. Leaves carry their type id in Qt::UserRole,
// which is both what a click creates and what a drag carries.
void PopulateNodeTree(QTreeWidget* tree)
{
    QMap<QString, QTreeWidgetItem*> categories;

    auto category = [&](const QString& name) {
        QTreeWidgetItem*& parent = categories[name];
        if (parent == nullptr)
        {
            parent = new QTreeWidgetItem(tree);
            parent->setText(0, name);
            parent->setFlags(parent->flags() & ~Qt::ItemIsSelectable);
        }

        return parent;
    };

    auto leaf = [](QTreeWidgetItem* parent, const QString& caption, const QString& tooltip,
                   const QString& typeId) {
        auto* item = new QTreeWidgetItem(parent);
        item->setText(0, caption);
        item->setToolTip(0, tooltip);
        item->setData(0, Qt::UserRole, typeId);
    };

    std::size_t typeCount = 0;
    const NodeGraphNodeType* types = NodeGraphCatalog::Types(typeCount);
    for (std::size_t i = 0; i < typeCount; ++i)
    {
        const NodeGraphNodeType& type = types[i];
        leaf(category(QString::fromUtf8(type.Category)), QString::fromUtf8(type.Caption),
             QString::fromUtf8(type.Tooltip), QString::fromUtf8(type.Id));
    }

    leaf(category(QStringLiteral("Comments")), QStringLiteral("Comment Box"),
         QStringLiteral("A titled frame drawn behind the nodes. Dragging it carries whatever sits inside."),
         QString::fromUtf8(kCommentTypeId));

    tree->expandAll();
}

// Runs a stored default through its declared type, so what gets saved is always something
// that type could actually produce: "hello" in an Int becomes 0, 3.7 becomes 3. Routed
// through the runtime's own parser on purpose - the editor and the interpreter must agree
// on what a default means, and one implementation is the only way to guarantee that.
std::string NormalisedDefault(const std::string& text, NodeGraphVariableType type)
{
    return NodeGraphValue::Parse(text, type).AsString();
}

// ---------------------------------------------------------------------------------------
// Entity picker
// ---------------------------------------------------------------------------------------

// Picks a level entity for an Entity parameter. The value is the entity's id in decimal;
// the list shows names. It is rebuilt every time it drops down, so it always reflects the
// level as it is now, and the button next to it takes whatever is selected in the
// Outliner or the viewport - the quickest way to point at one entity among hundreds.
class EntityPicker : public QWidget
{
public:
    explicit EntityPicker(QWidget* parent, std::function<void(const QString&)> onChosen)
        : QWidget(parent)
        , mOnChosen(std::move(onChosen))
    {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(2);

        mCombo = new Combo(this);
        mCombo->setMinimumWidth(130);
        mCombo->setMaxVisibleItems(24);
        mCombo->BeforePopup = [this] { Rebuild(); };
        row->addWidget(mCombo, 1);

        auto* useSelection = new QToolButton(this);
        useSelection->setText(QStringLiteral("◎"));
        useSelection->setToolTip(QStringLiteral("Use the entity selected in the level"));
        row->addWidget(useSelection);

        QObject::connect(mCombo, &QComboBox::activated, this, [this](int index) {
            // No rebuild here: the list is current (it was rebuilt as it opened), and
            // clearing a combo from inside its own activated signal is asking for trouble.
            Choose(mCombo->itemData(index).toString());
            mCombo->setToolTip(mValue.isEmpty() ? QString() : QStringLiteral("Entity id %1").arg(mValue));
        });
        QObject::connect(useSelection, &QToolButton::clicked, this, [this] {
            const std::uint64_t selected = gSelectedEntity ? gSelectedEntity() : 0;
            if (selected == 0)
            {
                QToolTip::showText(QCursor::pos(), QStringLiteral("Select an entity in the level first."));
                return;
            }

            Choose(QString::number(selected));
            Rebuild();
        });
    }

    // Shows a stored value without reporting it as an edit.
    void SetValue(const QString& value)
    {
        mValue = value;
        Rebuild();
    }

    void Rebuild()
    {
        const QSignalBlocker blocker(mCombo);
        mCombo->clear();
        mCombo->addItem(QStringLiteral("(none)"), QString());

        std::vector<NodeGraphEntityInfo> entities;
        if (gListEntities)
        {
            entities = gListEntities();
        }

        std::stable_sort(entities.begin(), entities.end(), [](const auto& a, const auto& b) {
            return QString::fromStdString(a.Name).compare(QString::fromStdString(b.Name), Qt::CaseInsensitive) < 0;
        });

        int current = mValue.isEmpty() ? 0 : -1;
        for (const NodeGraphEntityInfo& entity : entities)
        {
            const QString id = QString::number(entity.Id);
            mCombo->addItem(QString::fromStdString(entity.Name), id);
            mCombo->setItemData(mCombo->count() - 1, QStringLiteral("Entity id %1").arg(id), Qt::ToolTipRole);
            if (id == mValue)
            {
                current = mCombo->count() - 1;
            }
        }

        if (current < 0)
        {
            // Keep a reference to a deleted entity visible rather than silently clearing
            // it: the graph still points there, and the log will say so at runtime.
            mCombo->addItem(QStringLiteral("<missing %1>").arg(mValue), mValue);
            current = mCombo->count() - 1;
        }

        mCombo->setCurrentIndex(current);
        mCombo->setToolTip(mValue.isEmpty() ? QString() : QStringLiteral("Entity id %1").arg(mValue));
    }

private:
    // showPopup is virtual, so overriding it needs no Q_OBJECT.
    class Combo : public QComboBox
    {
    public:
        using QComboBox::QComboBox;
        std::function<void()> BeforePopup;

        void showPopup() override
        {
            if (BeforePopup)
            {
                BeforePopup();
            }
            QComboBox::showPopup();
        }
    };

    void Choose(const QString& value)
    {
        mValue = value;
        mOnChosen(value);
    }

    Combo* mCombo = nullptr;
    QString mValue;
    std::function<void(const QString&)> mOnChosen;
};

// ---------------------------------------------------------------------------------------
// Delegate model
// ---------------------------------------------------------------------------------------

// One class serves every node type: ports, captions and the inline editors all come out of
// the catalogue row it was handed. Adding a node to the catalogue therefore needs no new
// Qt code at all.
class PteroNodeDelegate : public QtNodes::NodeDelegateModel
{
public:
    PteroNodeDelegate(const NodeGraphNodeType* type, GraphContext* context)
        : mType(type)
        , mContext(context)
    {
        setBackgroundColor(CategoryColor(type->Category));

        for (unsigned i = 0; i < mType->ParamCount; ++i)
        {
            const NodeGraphParam& param = mType->Params[i];
            mParams[QString::fromUtf8(param.Key)] = QString::fromUtf8(param.DefaultValue);
        }
    }

    const NodeGraphNodeType& Type() const { return *mType; }

    QString name() const override { return QString::fromUtf8(mType->Id); }
    QString caption() const override { return QString::fromUtf8(mType->Caption); }
    bool captionVisible() const override { return true; }

    unsigned int nPorts(PortType portType) const override
    {
        return portType == PortType::In ? mType->InputCount : mType->OutputCount;
    }

    NodeDataType dataType(PortType portType, PortIndex portIndex) const override
    {
        const NodeGraphPin* pin = PinAt(portType, portIndex);
        if (pin == nullptr)
        {
            return NodeDataType{ QStringLiteral("any"), QStringLiteral("Any") };
        }

        return NodeDataType{ QString::fromUtf8(NodeGraphCatalog::PinKindId(pin->Kind)),
                             QString::fromUtf8(NodeGraphCatalog::PinKindName(pin->Kind)) };
    }

    QString portCaption(PortType portType, PortIndex portIndex) const override
    {
        const NodeGraphPin* pin = PinAt(portType, portIndex);
        return pin == nullptr ? QString() : QString::fromUtf8(pin->Name);
    }

    bool portCaptionVisible(PortType, PortIndex) const override { return true; }

    ConnectionPolicy portConnectionPolicy(PortType portType, PortIndex portIndex) const override
    {
        const NodeGraphPin* pin = PinAt(portType, portIndex);
        if (pin != nullptr && pin->Kind == NodePinKind::Exec)
        {
            // Execution runs the other way round from data: one output fires exactly one
            // target, but any number of paths may converge on the same input.
            return portType == PortType::In ? ConnectionPolicy::Many : ConnectionPolicy::One;
        }

        return portType == PortType::In ? ConnectionPolicy::One : ConnectionPolicy::Many;
    }

    // The scene propagates data between nodes while editing; the graph is executed by
    // NodeGraphRuntime instead, so there is nothing to carry here.
    void setInData(std::shared_ptr<QtNodes::NodeData>, PortIndex) override {}
    std::shared_ptr<QtNodes::NodeData> outData(PortIndex) override { return nullptr; }

    QWidget* embeddedWidget() override
    {
        if (mWidget == nullptr && mType->ParamCount > 0)
        {
            BuildWidget();
        }

        return mWidget;
    }

    QJsonObject save() const override
    {
        QJsonObject nodeJson = QtNodes::NodeDelegateModel::save();

        QJsonObject paramsJson;
        for (auto it = mParams.constBegin(); it != mParams.constEnd(); ++it)
        {
            paramsJson[it.key()] = it.value();
        }
        nodeJson["params"] = paramsJson;

        return nodeJson;
    }

    void load(QJsonObject const& nodeJson) override
    {
        const QJsonObject paramsJson = nodeJson["params"].toObject();
        for (auto it = paramsJson.constBegin(); it != paramsJson.constEnd(); ++it)
        {
            mParams[it.key()] = it.value().toString();
        }

        PushParamsToEditors();
    }

    std::map<std::string, std::string> ParamsAsStdMap() const
    {
        std::map<std::string, std::string> result;
        for (auto it = mParams.constBegin(); it != mParams.constEnd(); ++it)
        {
            result[it.key().toStdString()] = it.value().toStdString();
        }

        return result;
    }

    void SetParams(const std::map<std::string, std::string>& params)
    {
        for (const auto& [key, value] : params)
        {
            mParams[QString::fromStdString(key)] = QString::fromStdString(value);
        }

        PushParamsToEditors();
    }

    // Called after the variable table or the set of functions changes, so every picker
    // offers the new set without the user having to recreate their nodes.
    void RefreshVariableChoices()
    {
        for (const auto& [key, editor] : mEditors)
        {
            auto* combo = qobject_cast<QComboBox*>(editor);
            const NodeGraphParam* param = NodeGraphCatalog::FindParam(*mType, key.toUtf8().constData());
            if (combo == nullptr || param == nullptr)
            {
                continue;
            }

            if (param->Kind != NodeParamKind::Variable && param->Kind != NodeParamKind::Function)
            {
                continue;
            }

            const QString current = mParams.value(key);
            const QSignalBlocker blocker(combo);
            combo->clear();

            if (param->Kind == NodeParamKind::Variable)
            {
                for (const NodeGraphVariable& variable : mContext->Variables)
                {
                    combo->addItem(QString::fromStdString(variable.Name));
                }
            }
            else
            {
                combo->addItems(mContext->Functions);
            }

            combo->setCurrentText(current);
        }
    }

    // Re-reads entity names, so a rename in the level shows on nodes already placed.
    void RefreshEntityPickers()
    {
        for (const auto& [key, editor] : mEditors)
        {
            if (auto* picker = dynamic_cast<EntityPicker*>(editor))
            {
                picker->Rebuild();
            }
        }
    }

private:
    const NodeGraphPin* PinAt(PortType portType, PortIndex portIndex) const
    {
        if (portType == PortType::In)
        {
            return portIndex < mType->InputCount ? &mType->Inputs[portIndex] : nullptr;
        }

        if (portType == PortType::Out)
        {
            return portIndex < mType->OutputCount ? &mType->Outputs[portIndex] : nullptr;
        }

        return nullptr;
    }

    void RecordEdit(const QString& key, const QString& value)
    {
        if (mParams.value(key) == value)
        {
            return;
        }

        mParams[key] = value;
        if (mContext != nullptr && mContext->OnEdited)
        {
            mContext->OnEdited();
        }
    }

    void BuildWidget()
    {
        mWidget = new QWidget();
        mWidget->setAttribute(Qt::WA_NoSystemBackground);
        auto* form = new QFormLayout(mWidget);
        form->setContentsMargins(4, 2, 4, 2);
        form->setSpacing(3);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        for (unsigned i = 0; i < mType->ParamCount; ++i)
        {
            const NodeGraphParam& param = mType->Params[i];
            const QString key = QString::fromUtf8(param.Key);
            QWidget* editor = nullptr;

            switch (param.Kind)
            {
            case NodeParamKind::Bool:
            {
                auto* check = new QCheckBox(mWidget);
                check->setChecked(mParams.value(key) == "true");
                QObject::connect(check, &QCheckBox::toggled, this, [this, key](bool checked) {
                    RecordEdit(key, checked ? "true" : "false");
                });
                editor = check;
                break;
            }
            case NodeParamKind::Number:
            {
                auto* spin = new QDoubleSpinBox(mWidget);
                spin->setDecimals(3);
                spin->setRange(-1.0e9, 1.0e9);
                spin->setValue(mParams.value(key).toDouble());
                spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
                spin->setMinimumWidth(70);
                QObject::connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double value) {
                    RecordEdit(key, QString::number(value, 'g', 10));
                });
                editor = spin;
                break;
            }
            case NodeParamKind::Enum:
            {
                auto* combo = new QComboBox(mWidget);
                combo->addItems(QString::fromUtf8(param.Items ? param.Items : "").split('|', Qt::SkipEmptyParts));
                combo->setCurrentText(mParams.value(key));
                QObject::connect(combo, &QComboBox::currentTextChanged, this, [this, key](const QString& text) {
                    RecordEdit(key, text);
                });
                editor = combo;
                break;
            }
            case NodeParamKind::Variable:
            {
                // Editable so a graph whose variable was renamed still shows what it was
                // pointing at rather than silently snapping to the first entry.
                auto* combo = new QComboBox(mWidget);
                combo->setEditable(true);
                combo->setMinimumWidth(110);
                for (const NodeGraphVariable& variable : mContext->Variables)
                {
                    combo->addItem(QString::fromStdString(variable.Name));
                }
                combo->setCurrentText(mParams.value(key));
                QObject::connect(combo, &QComboBox::currentTextChanged, this, [this, key](const QString& text) {
                    RecordEdit(key, text);
                });
                editor = combo;
                break;
            }
            case NodeParamKind::Function:
            {
                // Editable for the same reason as the variable picker, and because a
                // library may not have been imported yet when the call is wired up.
                auto* combo = new QComboBox(mWidget);
                combo->setEditable(true);
                combo->setMinimumWidth(140);
                for (const QString& function : mContext->Functions)
                {
                    combo->addItem(function);
                }
                combo->setCurrentText(mParams.value(key));
                QObject::connect(combo, &QComboBox::currentTextChanged, this, [this, key](const QString& text) {
                    RecordEdit(key, text);
                });
                editor = combo;
                break;
            }
            case NodeParamKind::Entity:
            {
                auto* picker = new EntityPicker(mWidget, [this, key](const QString& value) {
                    RecordEdit(key, value);
                });
                picker->SetValue(mParams.value(key));
                editor = picker;
                break;
            }
            case NodeParamKind::String:
            default:
            {
                auto* line = new QLineEdit(mWidget);
                line->setText(mParams.value(key));
                line->setMinimumWidth(120);
                QObject::connect(line, &QLineEdit::textChanged, this, [this, key](const QString& text) {
                    RecordEdit(key, text);
                });
                editor = line;
                break;
            }
            }

            form->addRow(QString::fromUtf8(param.Label), editor);
            mEditors.emplace_back(key, editor);
        }
    }

    void PushParamsToEditors()
    {
        for (const auto& [key, editor] : mEditors)
        {
            const QString value = mParams.value(key);
            const QSignalBlocker blocker(editor);

            // dynamic_cast: EntityPicker has no Q_OBJECT, so qobject_cast cannot see it.
            if (auto* picker = dynamic_cast<EntityPicker*>(editor))
            {
                picker->SetValue(value);
            }
            else if (auto* check = qobject_cast<QCheckBox*>(editor))
            {
                check->setChecked(value == "true");
            }
            else if (auto* spin = qobject_cast<QDoubleSpinBox*>(editor))
            {
                spin->setValue(value.toDouble());
            }
            else if (auto* combo = qobject_cast<QComboBox*>(editor))
            {
                combo->setCurrentText(value);
            }
            else if (auto* line = qobject_cast<QLineEdit*>(editor))
            {
                line->setText(value);
            }
        }
    }

    const NodeGraphNodeType* mType = nullptr;
    GraphContext* mContext = nullptr;
    QWidget* mWidget = nullptr;
    QMap<QString, QString> mParams;
    std::vector<std::pair<QString, QWidget*>> mEditors;
};

// ---------------------------------------------------------------------------------------
// Graph model
// ---------------------------------------------------------------------------------------

// DataFlowGraphModel is built for pure data flow, where a cycle would mean an endless
// propagation and types must match exactly. A blueprint-style graph needs the opposite on
// both counts: loops are the point of a Gate or a For Loop, and wildcard pins have to
// accept whatever the graph's variables turn out to be.
class PteroGraphModel : public QtNodes::DataFlowGraphModel
{
public:
    using QtNodes::DataFlowGraphModel::DataFlowGraphModel;

    bool loopsEnabled() const override { return true; }

    bool connectionPossible(ConnectionId const connectionId) const override
    {
        if (!nodeExists(connectionId.outNodeId) || !nodeExists(connectionId.inNodeId))
        {
            return false;
        }

        // A node wired into itself is never what was meant and makes the runtime's cycle
        // guard fire on the very first read.
        if (connectionId.outNodeId == connectionId.inNodeId)
        {
            return false;
        }

        const unsigned outCount = nodeData(connectionId.outNodeId, NodeRole::OutPortCount).toUInt();
        const unsigned inCount = nodeData(connectionId.inNodeId, NodeRole::InPortCount).toUInt();
        if (connectionId.outPortIndex >= outCount || connectionId.inPortIndex >= inCount)
        {
            return false;
        }

        const NodeDataType outType =
            portData(connectionId.outNodeId, PortType::Out, connectionId.outPortIndex,
                     QtNodes::PortRole::DataType).value<NodeDataType>();
        const NodeDataType inType =
            portData(connectionId.inNodeId, PortType::In, connectionId.inPortIndex,
                     QtNodes::PortRole::DataType).value<NodeDataType>();

        const bool outIsExec = outType.id == QLatin1String("exec");
        const bool inIsExec = inType.id == QLatin1String("exec");
        if (outIsExec != inIsExec)
        {
            return false;
        }

        if (!outIsExec && outType.id != inType.id &&
            outType.id != QLatin1String("any") && inType.id != QLatin1String("any"))
        {
            return false;
        }

        auto vacant = [&](PortType portType) {
            const NodeId nodeId = portType == PortType::Out ? connectionId.outNodeId : connectionId.inNodeId;
            const PortIndex portIndex =
                portType == PortType::Out ? connectionId.outPortIndex : connectionId.inPortIndex;
            const auto policy =
                portData(nodeId, portType, portIndex, QtNodes::PortRole::ConnectionPolicyRole)
                    .value<ConnectionPolicy>();

            return policy == ConnectionPolicy::Many || connections(nodeId, portType, portIndex).empty();
        };

        return vacant(PortType::Out) && vacant(PortType::In);
    }
};

// ---------------------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------------------

// Only reason to subclass: the stock context menu lists the registered model *names*,
// which for us are ids like "Flow.Delay". This one lists captions and keeps the id out of
// sight, which is what makes the menu readable.
class PteroNodeScene : public QtNodes::DataFlowGraphicsScene
{
public:
    using QtNodes::DataFlowGraphicsScene::DataFlowGraphicsScene;

    // Comment boxes are not registered node types, so the menu entry for one has to be
    // handed back to the window rather than pushed through the node registry.
    std::function<void(const QPointF&)> OnCreateComment;

    QMenu* createSceneMenu(QPointF const scenePos) override
    {
        auto* menu = new QMenu();

        auto* filter = new QLineEdit(menu);
        filter->setPlaceholderText(QStringLiteral("Filter"));
        filter->setClearButtonEnabled(true);
        auto* filterAction = new QWidgetAction(menu);
        filterAction->setDefaultWidget(filter);
        menu->addAction(filterAction);

        auto* tree = new QTreeWidget(menu);
        tree->header()->close();
        tree->setMinimumSize(260, 380);
        auto* treeAction = new QWidgetAction(menu);
        treeAction->setDefaultWidget(tree);
        menu->addAction(treeAction);

        PopulateNodeTree(tree);

        QObject::connect(tree, &QTreeWidget::itemClicked, menu,
                         [this, menu, scenePos](QTreeWidgetItem* item, int) {
                             const QString typeId = item->data(0, Qt::UserRole).toString();
                             if (typeId.isEmpty())
                             {
                                 return;
                             }

                             if (typeId == QLatin1String(kCommentTypeId))
                             {
                                 if (OnCreateComment)
                                 {
                                     OnCreateComment(scenePos);
                                 }
                             }
                             else
                             {
                                 undoStack().push(new QtNodes::CreateCommand(this, typeId, scenePos));
                             }

                             menu->close();
                         });

        QObject::connect(filter, &QLineEdit::textChanged, menu, [tree](const QString& text) {
            QTreeWidgetItemIterator categoryIt(tree, QTreeWidgetItemIterator::HasChildren);
            while (*categoryIt)
            {
                (*categoryIt++)->setHidden(true);
            }

            QTreeWidgetItemIterator it(tree, QTreeWidgetItemIterator::NoChildren);
            while (*it)
            {
                const bool match = (*it)->text(0).contains(text, Qt::CaseInsensitive);
                (*it)->setHidden(!match);
                if (match)
                {
                    for (QTreeWidgetItem* parent = (*it)->parent(); parent != nullptr; parent = parent->parent())
                    {
                        parent->setHidden(false);
                    }
                }

                ++it;
            }
        });

        filter->setFocus();
        return menu;
    }
};

// ---------------------------------------------------------------------------------------
// Comment box
// ---------------------------------------------------------------------------------------

// A titled frame that lives behind the nodes.
//
// Only the title bar and the resize grip are part of the item's shape(), so a click
// anywhere in the middle falls straight through to the scene. That is what lets a
// rubber-band selection be dragged across a comment without the box swallowing it, and it
// is why the box needs no "click-through" special case anywhere else.
class CommentBoxItem : public QGraphicsItem
{
public:
    CommentBoxItem(const NodeGraphComment& data, std::function<void()> onChanged)
        : mData(data)
        , mOnChanged(std::move(onChanged))
    {
        setZValue(-10.0); // Connections sit at -1 and nodes at 0.
        setAcceptHoverEvents(true);
        setFlag(QGraphicsItem::ItemIsSelectable, true);
        setPos(mData.X, mData.Y);
        mText = QString::fromStdString(mData.Text);
        mData.X = 0.0;
        mData.Y = 0.0;
    }

    // Geometry is carried by the item; the stored X/Y are only meaningful on the way in
    // and out of a document.
    NodeGraphComment ToDocument() const
    {
        NodeGraphComment result = mData;
        result.X = pos().x();
        result.Y = pos().y();
        return result;
    }

    const QString& Text() const { return mText; }

    void SetText(const QString& text)
    {
        mText = text;
        mData.Text = text.toStdString();
        update();
    }

    void SetColor(QColor color)
    {
        mData.Color = (static_cast<unsigned>(color.red()) << 16) |
                      (static_cast<unsigned>(color.green()) << 8) |
                      static_cast<unsigned>(color.blue());
        update();
    }

    QColor Color() const
    {
        return QColor(static_cast<int>((mData.Color >> 16) & 0xff),
                      static_cast<int>((mData.Color >> 8) & 0xff),
                      static_cast<int>(mData.Color & 0xff));
    }

    QRectF boundingRect() const override
    {
        return QRectF(0.0, -kTitleHeight, mData.Width, mData.Height + kTitleHeight);
    }

    QPainterPath shape() const override
    {
        QPainterPath path;
        path.addRect(QRectF(0.0, -kTitleHeight, mData.Width, kTitleHeight));
        path.addRect(ResizeGrip());
        return path;
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        const QColor base = Color();

        painter->setRenderHint(QPainter::Antialiasing, true);

        QColor body = base;
        body.setAlpha(48);
        painter->setBrush(body);

        const bool highlighted = isSelected() || mHovered;
        painter->setPen(QPen(isSelected() ? base.lighter(160) : base, highlighted ? 2.0 : 1.0));
        painter->drawRoundedRect(QRectF(0.0, 0.0, mData.Width, mData.Height), 4.0, 4.0);

        QColor title = base;
        title.setAlpha(220);
        painter->setBrush(title);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(QRectF(0.0, -kTitleHeight, mData.Width, kTitleHeight + 6.0), 4.0, 4.0);
        painter->drawRect(QRectF(0.0, -6.0, mData.Width, 6.0));

        painter->setPen(QPen(base.lightness() > 140 ? Qt::black : Qt::white));
        QFont font = painter->font();
        font.setBold(true);
        font.setPointSizeF(11.0);
        painter->setFont(font);
        painter->drawText(QRectF(8.0, -kTitleHeight, mData.Width - 16.0, kTitleHeight),
                          Qt::AlignVCenter | Qt::AlignLeft, mText);

        // Resize grip: three short diagonals in the bottom-right corner.
        painter->setPen(QPen(base, 1.5));
        const QRectF grip = ResizeGrip();
        for (int i = 1; i <= 3; ++i)
        {
            const qreal offset = i * (kGripSize / 4.0);
            painter->drawLine(QPointF(grip.right() - offset, grip.bottom()),
                              QPointF(grip.right(), grip.bottom() - offset));
        }
    }

    // Nodes whose centre falls inside the box travel with it.
    void SetCapturedItems(std::vector<QGraphicsItem*> items) { mCaptured = std::move(items); }

    // Invoked when the edit dialog's Delete button is used. Always called back through the
    // event loop, never straight from the dialog, so the item is not destroyed while one
    // of its own handlers is still on the stack.
    void SetDeleteRequestHandler(std::function<void()> handler) { mOnDeleteRequested = std::move(handler); }

protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override
    {
        mHovered = true;
        setCursor(ResizeGrip().contains(event->pos()) ? Qt::SizeFDiagCursor : Qt::SizeAllCursor);
        update();
        QGraphicsItem::hoverMoveEvent(event);
    }

    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override
    {
        mHovered = false;
        unsetCursor();
        update();
        QGraphicsItem::hoverLeaveEvent(event);
    }

    void mousePressEvent(QGraphicsSceneMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            event->ignore();
            return;
        }

        mResizing = ResizeGrip().contains(event->pos());
        mDragAnchor = event->scenePos();

        // Selection is done by hand because this handler never chains to the base one -
        // without it the box could never be selected, and Delete would skip it.
        if ((event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) == 0 && !isSelected())
        {
            if (scene() != nullptr)
            {
                scene()->clearSelection();
            }
        }
        setSelected(true);

        if (!mResizing)
        {
            // Capture on press rather than continuously: a node dragged out of the box
            // mid-move should not keep following it.
            mCaptured.clear();
            const QRectF area = mapToScene(QRectF(0.0, 0.0, mData.Width, mData.Height)).boundingRect();
            if (scene() != nullptr)
            {
                for (QGraphicsItem* item : scene()->items())
                {
                    if (item == this || item->parentItem() != nullptr || item->zValue() < 0.0)
                        continue;

                    if (area.contains(item->sceneBoundingRect().center()))
                        mCaptured.push_back(item);
                }
            }
        }

        event->accept();
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override
    {
        const QPointF delta = event->scenePos() - mDragAnchor;
        mDragAnchor = event->scenePos();

        if (mResizing)
        {
            mData.Width = (std::max)(kMinWidth, mData.Width + delta.x());
            mData.Height = (std::max)(kMinHeight, mData.Height + delta.y());
            prepareGeometryChange();
            update();
        }
        else
        {
            moveBy(delta.x(), delta.y());
            for (QGraphicsItem* item : mCaptured)
            {
                item->moveBy(delta.x(), delta.y());
            }
        }

        event->accept();
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override
    {
        mResizing = false;
        mCaptured.clear();
        if (mOnChanged)
        {
            mOnChanged();
        }

        event->accept();
    }

    // Title and colour are edited together here rather than from a context menu, because
    // the right button is the canvas pan and never raises one.
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override
    {
        QWidget* parent = event->widget() != nullptr ? event->widget()->window() : nullptr;

        QDialog dialog(parent);
        dialog.setWindowTitle(QStringLiteral("Comment"));

        auto* form = new QFormLayout(&dialog);
        auto* textEdit = new QLineEdit(mText, &dialog);
        textEdit->setMinimumWidth(280);
        form->addRow(QStringLiteral("Text"), textEdit);

        QColor chosen = Color();
        auto* colorButton = new QPushButton(&dialog);
        auto paintButton = [colorButton](const QColor& color) {
            colorButton->setText(color.name().toUpper());
            colorButton->setStyleSheet(
                QStringLiteral("background-color: %1; color: %2;")
                    .arg(color.name(), color.lightness() > 140 ? QStringLiteral("black")
                                                               : QStringLiteral("white")));
        };
        paintButton(chosen);
        form->addRow(QStringLiteral("Colour"), colorButton);

        QObject::connect(colorButton, &QPushButton::clicked, colorButton, [&] {
            const QColor picked = QColorDialog::getColor(chosen, &dialog, QStringLiteral("Comment colour"));
            if (picked.isValid())
            {
                chosen = picked;
                paintButton(chosen);
            }
        });

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        // Selecting the title bar and pressing Delete also works, but the right button is
        // the canvas pan and raises no context menu, so this is the discoverable route.
        QPushButton* deleteButton = buttons->addButton(QStringLiteral("Delete"), QDialogButtonBox::DestructiveRole);
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        bool deleteRequested = false;
        QObject::connect(deleteButton, &QPushButton::clicked, &dialog, [&] {
            deleteRequested = true;
            dialog.reject();
        });

        const int outcome = dialog.exec();

        if (deleteRequested)
        {
            if (mOnDeleteRequested)
            {
                // Queued: this is still inside the item's own double-click handler, and
                // the handler cannot return through an object that has been freed.
                QTimer::singleShot(0, mOnDeleteRequested);
            }

            event->accept();
            return;
        }

        if (outcome == QDialog::Accepted)
        {
            SetText(textEdit->text());
            SetColor(chosen);
            if (mOnChanged)
            {
                mOnChanged();
            }
        }

        event->accept();
    }

private:
    static constexpr qreal kTitleHeight = 26.0;
    static constexpr qreal kGripSize = 16.0;
    static constexpr qreal kMinWidth = 120.0;
    static constexpr qreal kMinHeight = 80.0;

    QRectF ResizeGrip() const
    {
        return QRectF(mData.Width - kGripSize, mData.Height - kGripSize, kGripSize, kGripSize);
    }

    NodeGraphComment mData;
    QString mText = QStringLiteral("Comment");
    std::function<void()> mOnChanged;
    std::function<void()> mOnDeleteRequested;
    std::vector<QGraphicsItem*> mCaptured;
    QPointF mDragAnchor;
    bool mResizing = false;
    bool mHovered = false;
};

// ---------------------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------------------

// Navigation follows the convention the rest of the editor uses rather than the QtNodes
// default: the right button pans, the left button only ever selects or drags a rubber
// band, and the node list is on Q instead of the right-click menu the base class installs.
class PteroGraphicsView : public QtNodes::GraphicsView
{
public:
    PteroGraphicsView(QtNodes::BasicGraphicsScene* scene, QWidget* parent)
        : QtNodes::GraphicsView(scene, parent)
    {
        setDragMode(QGraphicsView::RubberBandDrag);
        setAcceptDrops(true);
        setFocusPolicy(Qt::StrongFocus);
    }

    // Set by the window: creates a node of the given type at a scene position.
    std::function<void(const QString&, const QPointF&)> OnCreateNode;
    // Set by the window: drops a variable, asking the user for Get or Set.
    std::function<void(const QString&, const QPointF&)> OnDropVariable;
    // Set by the window: creates a comment box, wrapping the selection when there is one.
    std::function<void(const QPointF&)> OnCreateComment;

    // Opens the node list where the pointer is, which is what Q does and what the Insert
    // menu calls into.
    void ShowNodeMenuAtCursor()
    {
        if (nodeScene() == nullptr)
        {
            return;
        }

        if (QMenu* menu = nodeScene()->createSceneMenu(CursorScenePos()))
        {
            menu->exec(QCursor::pos());
            delete menu;
        }
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::RightButton)
        {
            mPanning = true;
            mPanAnchor = event->pos();
            viewport()->setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }

        // Deliberately not GraphicsView::mousePressEvent: the base records an anchor for
        // its own left-button panning, which is exactly the behaviour being replaced.
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (mPanning)
        {
            // Panning moves the view transform, not the scene rect.
            //
            // The scene rect is what the QtNodes base class translates, but QGraphicsView
            // *centres* a scene rect smaller than the viewport, so that approach moves
            // nothing at all until the graph has outgrown the window - which is exactly
            // the point at which a user gives up on it. Scrollbars are no better here:
            // the base switches them off, so their range is zero.
            //
            // The transformation anchor has to be neutralised around the change. The base
            // sets AnchorUnderMouse so the wheel zooms toward the pointer, and that same
            // anchor would drag the content straight back under the cursor here.
            const QPointF delta = mapToScene(event->pos()) - mapToScene(mPanAnchor);

            const QGraphicsView::ViewportAnchor anchor = transformationAnchor();
            setTransformationAnchor(QGraphicsView::NoAnchor);
            QTransform moved = transform();
            moved.translate(delta.x(), delta.y());
            setTransform(moved);
            setTransformationAnchor(anchor);

            mPanAnchor = event->pos();
            event->accept();
            return;
        }

        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::RightButton && mPanning)
        {
            mPanning = false;
            viewport()->unsetCursor();
            event->accept();
            return;
        }

        QGraphicsView::mouseReleaseEvent(event);
    }

    // The right button pans, so it must not also raise a menu.
    void contextMenuEvent(QContextMenuEvent* event) override { event->accept(); }

    void keyPressEvent(QKeyEvent* event) override
    {
        // Let a node's embedded editor have the key first, otherwise typing a "q" into a
        // Print String node would open the palette instead. The test is specifically for a
        // proxy widget: QtNodes marks every node focusable, so merely having clicked one
        // must not count as typing.
        QGraphicsItem* focus = scene() != nullptr ? scene()->focusItem() : nullptr;
        const bool editing = focus != nullptr &&
                             qgraphicsitem_cast<QGraphicsProxyWidget*>(focus) != nullptr;

        if (!editing && event->modifiers() == Qt::NoModifier)
        {
            if (event->key() == Qt::Key_Q)
            {
                ShowNodeMenuAtCursor();
                event->accept();
                return;
            }

            if (event->key() == Qt::Key_C && OnCreateComment)
            {
                OnCreateComment(CursorScenePos());
                event->accept();
                return;
            }

        }

        // Delete is deliberately absent here. GraphicsView binds it as a QAction shortcut
        // on the view, and Qt dispatches shortcuts before a widget's key events, so a
        // branch in this function would never run. Comment removal hangs off that same
        // action instead - see where the window connects deleteSelectionAction().

        // Deliberately not GraphicsView::keyPressEvent: the base swaps the drag mode while
        // Shift is held, which would break shift-extend rubber-band selection here.
        QGraphicsView::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override { QGraphicsView::keyReleaseEvent(event); }

    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasFormat(kNodeTypeMime) || event->mimeData()->hasFormat(kVariableMime))
        {
            event->acceptProposedAction();
            return;
        }

        event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (event->mimeData()->hasFormat(kNodeTypeMime) || event->mimeData()->hasFormat(kVariableMime))
        {
            event->acceptProposedAction();
            return;
        }

        event->ignore();
    }

    void dropEvent(QDropEvent* event) override
    {
        const QPointF scenePos = mapToScene(event->position().toPoint());

        if (event->mimeData()->hasFormat(kNodeTypeMime))
        {
            const QString typeId = QString::fromUtf8(event->mimeData()->data(kNodeTypeMime));
            if (typeId == QLatin1String(kCommentTypeId))
            {
                if (OnCreateComment)
                {
                    OnCreateComment(scenePos);
                }
            }
            else if (OnCreateNode)
            {
                OnCreateNode(typeId, scenePos);
            }

            event->acceptProposedAction();
            return;
        }

        if (event->mimeData()->hasFormat(kVariableMime) && OnDropVariable)
        {
            OnDropVariable(QString::fromUtf8(event->mimeData()->data(kVariableMime)), scenePos);
            event->acceptProposedAction();
            return;
        }

        event->ignore();
    }

private:
    QPointF CursorScenePos() const
    {
        const QPoint local = mapFromGlobal(QCursor::pos());
        return viewport()->rect().contains(local) ? mapToScene(local)
                                                  : mapToScene(viewport()->rect().center());
    }

    // Viewport pixels, re-anchored every move so the grabbed point stays under the cursor.
    QPoint mPanAnchor;
    bool mPanning = false;
};

// ---------------------------------------------------------------------------------------
// Draggable source views
// ---------------------------------------------------------------------------------------

// The palette hands the canvas a node type id; the canvas turns it into a node on drop.
class PaletteTree : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return { QString::fromUtf8(kNodeTypeMime) }; }

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        if (items.isEmpty())
        {
            return nullptr;
        }

        const QString typeId = items.first()->data(0, Qt::UserRole).toString();
        if (typeId.isEmpty())
        {
            return nullptr; // A category row, not a node.
        }

        auto* data = new QMimeData();
        data->setData(QString::fromUtf8(kNodeTypeMime), typeId.toUtf8());
        return data;
    }
};

// The variable table hands over a variable name; the canvas asks whether to get or set it.
class VariableTable : public QTableWidget
{
public:
    using QTableWidget::QTableWidget;

protected:
    QStringList mimeTypes() const override { return { QString::fromUtf8(kVariableMime) }; }

    QMimeData* mimeData(const QList<QTableWidgetItem*>& items) const override
    {
        for (const QTableWidgetItem* item : items)
        {
            // Only the name column identifies the variable; the type and default columns
            // would drag a value the canvas cannot do anything with.
            if (item->column() == 0 && !item->text().isEmpty())
            {
                auto* data = new QMimeData();
                data->setData(QString::fromUtf8(kVariableMime), item->text().toUtf8());
                return data;
            }
        }

        return nullptr;
    }
};

// ---------------------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------------------

class NodeGraphWindow : public QMainWindow
{
public:
    explicit NodeGraphWindow(QWidget* parent)
        : QMainWindow(parent, Qt::Window)
    {
        setWindowTitle(QStringLiteral("Node Graph"));
        setObjectName(QStringLiteral("nodeGraphWindow"));

        // The canvas is unbounded, so there is no content size to open at - this is simply
        // room enough to work in. Clamped to the desktop: on a laptop screen the untrimmed
        // size puts the status bar and part of the palette off the bottom, and a window
        // that opens larger than the screen cannot be dragged back into view.
        QSize opening(1280, 820);
        if (const QScreen* display = screen())
        {
            opening = opening.boundedTo(display->availableGeometry().size() * 0.9);
        }
        resize(opening);

        mContext.OnEdited = [this] { MarkEdited(); };

        mRegistry = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
        std::size_t typeCount = 0;
        const NodeGraphNodeType* types = NodeGraphCatalog::Types(typeCount);
        for (std::size_t i = 0; i < typeCount; ++i)
        {
            const NodeGraphNodeType* type = &types[i];
            GraphContext* context = &mContext;
            mRegistry->registerModel<PteroNodeDelegate>(
                [type, context]() { return std::make_unique<PteroNodeDelegate>(type, context); },
                QString::fromUtf8(type->Category));
        }

        mModel = std::make_unique<PteroGraphModel>(mRegistry);
        mScene = new PteroNodeScene(*mModel, this);
        mScene->OnCreateComment = [this](const QPointF& scenePos) { CreateCommentAt(scenePos); };

        mView = new PteroGraphicsView(mScene, this);
        mView->OnCreateNode = [this](const QString& typeId, const QPointF& scenePos) {
            AddNodeAt(typeId, scenePos);
        };
        mView->OnDropVariable = [this](const QString& name, const QPointF& scenePos) {
            DropVariableAt(name, scenePos);
        };
        mView->OnCreateComment = [this](const QPointF& scenePos) { CreateCommentAt(scenePos); };
        setCentralWidget(mView);

        // The view owns Delete as a QAction shortcut, and its own slot only removes nodes
        // and connections - the graph model knows nothing about comment boxes. Hanging off
        // the same action keeps one gesture for both, and covers Edit > Delete Selection
        // as well as the key, so a mixed selection goes in a single press.
        QObject::connect(mView->deleteSelectionAction(), &QAction::triggered, this,
                         [this] { DeleteSelectedComments(); });

        QObject::connect(mModel.get(), &QtNodes::AbstractGraphModel::nodeCreated, this,
                         [this](NodeId) { MarkEdited(); });
        QObject::connect(mModel.get(), &QtNodes::AbstractGraphModel::nodeDeleted, this,
                         [this](NodeId) { MarkEdited(); });
        QObject::connect(mModel.get(), &QtNodes::AbstractGraphModel::nodePositionUpdated, this,
                         [this](NodeId) { MarkEdited(); });
        QObject::connect(mModel.get(), &QtNodes::AbstractGraphModel::connectionCreated, this,
                         [this](ConnectionId) { MarkEdited(); });
        QObject::connect(mModel.get(), &QtNodes::AbstractGraphModel::connectionDeleted, this,
                         [this](ConnectionId) { MarkEdited(); });

        BuildMenus();
        BuildPalette();
        BuildVariablesDock();
        BuildFunctionsDock();

        statusBar()->showMessage(QStringLiteral("Stored in the level."));
    }

    // ---- document ----------------------------------------------------------------

    void RefreshEntityPickers()
    {
        for (const NodeId nodeId : mModel->allNodeIds())
        {
            if (auto* delegate = mModel->delegateModel<PteroNodeDelegate>(nodeId))
            {
                delegate->RefreshEntityPickers();
            }
        }
    }

    void SetDocument(const NodeGraphDocument& document)
    {
        mSuspendEdits = true;

        // clearScene only deletes what the graph model owns, so the comment items - which
        // are plain scene items - have to be taken out by hand.
        ClearComments();
        mScene->clearScene();
        mScene->undoStack().clear();

        mGraphName = QString::fromStdString(document.Name);
        mContext.Variables = document.Variables;
        RefreshVariableTable();

        std::unordered_map<int, NodeId> idMap;
        for (const NodeGraphNode& node : document.Nodes)
        {
            const NodeId nodeId = mModel->addNode(QString::fromStdString(node.TypeId));
            if (nodeId == QtNodes::InvalidNodeId)
            {
                continue;
            }

            idMap[node.Id] = nodeId;
            mModel->setNodeData(nodeId, NodeRole::Position, QPointF(node.X, node.Y));

            if (auto* delegate = mModel->delegateModel<PteroNodeDelegate>(nodeId))
            {
                delegate->SetParams(node.Params);
            }
        }

        for (const NodeGraphConnection& connection : document.Connections)
        {
            const auto from = idMap.find(connection.FromNode);
            const auto to = idMap.find(connection.ToNode);
            if (from == idMap.end() || to == idMap.end())
            {
                continue;
            }

            mModel->addConnection(ConnectionId{ from->second,
                                                static_cast<PortIndex>(connection.FromPort),
                                                to->second,
                                                static_cast<PortIndex>(connection.ToPort) });
        }

        for (const NodeGraphComment& comment : document.Comments)
        {
            AddCommentItem(comment);
            mNextCommentId = (std::max)(mNextCommentId, comment.Id + 1);
        }

        mSuspendEdits = false;
        // Edits are suppressed above, so the scan that normally rides on MarkEdited has to
        // be run by hand once the whole document is in place.
        RefreshFunctions();
        RefreshFunctionList();
        UpdateStatus();
    }

    NodeGraphDocument BuildDocument() const
    {
        NodeGraphDocument document;
        document.Name = mGraphName.toStdString();
        document.Variables = mContext.Variables;

        std::unordered_set<ConnectionId> connections;
        for (const NodeId nodeId : mModel->allNodeIds())
        {
            auto* delegate = mModel->delegateModel<PteroNodeDelegate>(nodeId);
            if (delegate == nullptr)
            {
                continue;
            }

            const QPointF position = mModel->nodeData(nodeId, NodeRole::Position).value<QPointF>();

            NodeGraphNode node;
            node.Id = static_cast<int>(nodeId);
            node.TypeId = delegate->Type().Id;
            node.X = position.x();
            node.Y = position.y();
            node.Params = delegate->ParamsAsStdMap();
            document.Nodes.push_back(std::move(node));

            for (const ConnectionId& connectionId : mModel->allConnectionIds(nodeId))
            {
                connections.insert(connectionId);
            }
        }

        for (const ConnectionId& connectionId : connections)
        {
            NodeGraphConnection connection;
            connection.FromNode = static_cast<int>(connectionId.outNodeId);
            connection.FromPort = static_cast<int>(connectionId.outPortIndex);
            connection.ToNode = static_cast<int>(connectionId.inNodeId);
            connection.ToPort = static_cast<int>(connectionId.inPortIndex);
            document.Connections.push_back(connection);
        }

        for (const CommentBoxItem* comment : mComments)
        {
            document.Comments.push_back(comment->ToDocument());
        }

        return document;
    }

    void SetEditedCallback(std::function<void()> callback) { mOnEdited = std::move(callback); }

    const QString& ExportPath() const { return mExportPath; }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        // Closing the window only hides it: the graph belongs to the level, not to the
        // window, and reopening must show exactly what was there before.
        hide();
        event->ignore();
    }

private:
    // ---- creation ----------------------------------------------------------------

    void AddNodeAt(const QString& typeId, const QPointF& scenePos)
    {
        if (typeId == QLatin1String(kCommentTypeId))
        {
            CreateCommentAt(scenePos);
            return;
        }

        mScene->undoStack().push(new QtNodes::CreateCommand(mScene, typeId, scenePos));
    }

    // Dropping a variable is ambiguous, so ask - the same choice Blueprint offers.
    void DropVariableAt(const QString& name, const QPointF& scenePos)
    {
        QMenu menu(this);
        QAction* getAction = menu.addAction(QStringLiteral("Get %1").arg(name));
        QAction* setAction = menu.addAction(QStringLiteral("Set %1").arg(name));

        const QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == nullptr)
        {
            return;
        }

        const QString typeId = (chosen == setAction) ? QStringLiteral("Variable.Set")
                                                     : QStringLiteral("Variable.Get");
        Q_UNUSED(getAction);

        // Created straight through the model rather than the undo stack, because the
        // variable name has to be stamped on before the node is worth undoing to.
        const NodeId nodeId = mModel->addNode(typeId);
        if (nodeId == QtNodes::InvalidNodeId)
        {
            return;
        }

        mModel->setNodeData(nodeId, NodeRole::Position, scenePos);
        if (auto* delegate = mModel->delegateModel<PteroNodeDelegate>(nodeId))
        {
            delegate->SetParams({ { "Variable", name.toStdString() } });
        }

        MarkEdited();
    }

    // ---- comments ----------------------------------------------------------------

    CommentBoxItem* AddCommentItem(const NodeGraphComment& data)
    {
        auto* item = new CommentBoxItem(data, [this] { MarkEdited(); });
        item->SetDeleteRequestHandler([this, item] { RemoveComment(item); });
        mScene->addItem(item);
        mComments.push_back(item);
        return item;
    }

    void RemoveComment(CommentBoxItem* comment)
    {
        const auto it = std::find(mComments.begin(), mComments.end(), comment);
        if (it == mComments.end())
        {
            return;
        }

        mComments.erase(it);
        mScene->removeItem(comment);
        delete comment;
        MarkEdited();
    }

    void ClearComments()
    {
        for (CommentBoxItem* comment : mComments)
        {
            mScene->removeItem(comment);
            delete comment;
        }

        mComments.clear();
        mNextCommentId = 1;
    }

    void DeleteSelectedComments()
    {
        bool removed = false;
        for (auto it = mComments.begin(); it != mComments.end();)
        {
            if ((*it)->isSelected())
            {
                mScene->removeItem(*it);
                delete *it;
                it = mComments.erase(it);
                removed = true;
            }
            else
            {
                ++it;
            }
        }

        if (removed)
        {
            MarkEdited();
        }
    }

    void CreateCommentAt(const QPointF& scenePos)
    {
        NodeGraphComment data;
        data.Id = mNextCommentId++;

        // With a selection, wrap it: that is how a comment usually gets made, and doing it
        // by hand afterwards means dragging the box to the right size twice.
        QRectF selection;
        for (const NodeId nodeId : mScene->selectedNodes())
        {
            if (QGraphicsObject* object = mScene->nodeGraphicsObject(nodeId))
            {
                selection = selection.united(object->sceneBoundingRect());
            }
        }

        if (selection.isValid())
        {
            constexpr qreal margin = 28.0;
            data.X = selection.left() - margin;
            data.Y = selection.top() - margin;
            data.Width = selection.width() + margin * 2.0;
            data.Height = selection.height() + margin * 2.0;
        }
        else
        {
            data.X = scenePos.x();
            data.Y = scenePos.y();
        }

        AddCommentItem(data);
        MarkEdited();
    }

    void MarkEdited()
    {
        if (mSuspendEdits)
        {
            return;
        }

        // A rename on a Function Entry body arrives here like any other edit, so this is
        // the one place that has to notice the function list may have moved.
        RefreshFunctions();

        if (mOnEdited)
        {
            mOnEdited();
        }

        UpdateStatus();
    }

    void UpdateStatus()
    {
        const int nodeCount = static_cast<int>(mModel->allNodeIds().size());
        QString message = QStringLiteral("%1 node(s), %2 variable(s) - stored in the level")
                              .arg(nodeCount)
                              .arg(static_cast<int>(mContext.Variables.size()));

        if (!mExportPath.isEmpty())
        {
            message += QStringLiteral("; last file: ") + QDir::toNativeSeparators(mExportPath);
        }

        statusBar()->showMessage(message);
    }

    void BuildMenus()
    {
        QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

        QAction* newAction = fileMenu->addAction(QStringLiteral("&New Graph"));
        newAction->setShortcut(QKeySequence::New);
        QObject::connect(newAction, &QAction::triggered, this, [this] {
            if (QMessageBox::question(this, QStringLiteral("New Graph"),
                                      QStringLiteral("Discard the current graph and start an empty one?"))
                != QMessageBox::Yes)
            {
                return;
            }

            NodeGraphDocument empty;
            SetDocument(empty);
            mExportPath.clear();
            MarkEdited();
        });

        QAction* loadAction = fileMenu->addAction(QStringLiteral("&Load Graph..."));
        loadAction->setShortcut(QKeySequence::Open);
        loadAction->setToolTip(QStringLiteral("Replaces the graph that is open."));
        QObject::connect(loadAction, &QAction::triggered, this, [this] { LoadFromFile(); });

        QAction* exportAction = fileMenu->addAction(QStringLiteral("&Export Graph..."));
        exportAction->setShortcut(QKeySequence::Save);
        QObject::connect(exportAction, &QAction::triggered, this, [this] { ExportToFile(); });

        QAction* importAction = fileMenu->addAction(QStringLiteral("&Import Graph as Library..."));
        importAction->setToolTip(
            QStringLiteral("Copies another graph in alongside this one so its functions can be called."));
        QObject::connect(importAction, &QAction::triggered, this, [this] { ImportLibrary(); });

        fileMenu->addSeparator();
        QAction* closeAction = fileMenu->addAction(QStringLiteral("&Close Window"));
        closeAction->setShortcut(QKeySequence::Close);
        QObject::connect(closeAction, &QAction::triggered, this, [this] { hide(); });

        QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
        QAction* undoAction = mScene->undoStack().createUndoAction(this, QStringLiteral("&Undo"));
        undoAction->setShortcut(QKeySequence::Undo);
        editMenu->addAction(undoAction);

        QAction* redoAction = mScene->undoStack().createRedoAction(this, QStringLiteral("&Redo"));
        redoAction->setShortcut(QKeySequence::Redo);
        editMenu->addAction(redoAction);

        editMenu->addSeparator();
        editMenu->addAction(mView->deleteSelectionAction());
        editMenu->addAction(mView->clearSelectionAction());

        // The keys themselves are handled by the canvas rather than declared as QAction
        // shortcuts: a single-key window shortcut would fire even while a node's text
        // field has focus, and "Print String" has every right to contain a q. The canvas
        // sees the key only once the focused editor has passed on it, so the labels here
        // merely advertise what the canvas already does.
        QMenu* insertMenu = menuBar()->addMenu(QStringLiteral("&Insert"));

        QAction* nodeListAction = insertMenu->addAction(QStringLiteral("&Node List   (Q)"));
        QObject::connect(nodeListAction, &QAction::triggered, this,
                         [this] { mView->ShowNodeMenuAtCursor(); });

        QAction* commentAction = insertMenu->addAction(QStringLiteral("&Comment Box   (C)"));
        commentAction->setToolTip(
            QStringLiteral("Wraps the selected nodes, or drops an empty box under the cursor."));
        QObject::connect(commentAction, &QAction::triggered, this, [this] {
            CreateCommentAt(mView->mapToScene(mView->viewport()->rect().center()));
        });

        QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
        QObject::connect(viewMenu->addAction(QStringLiteral("&Fit to Contents")), &QAction::triggered, this,
                         [this] { mView->centerScene(); });
        QObject::connect(viewMenu->addAction(QStringLiteral("Zoom &In")), &QAction::triggered, this,
                         [this] { mView->scaleUp(); });
        QObject::connect(viewMenu->addAction(QStringLiteral("Zoom &Out")), &QAction::triggered, this,
                         [this] { mView->scaleDown(); });
        viewMenu->addSeparator();
        mViewMenu = viewMenu;
    }

    void BuildPalette()
    {
        auto* dock = new QDockWidget(QStringLiteral("Palette"), this);
        dock->setObjectName(QStringLiteral("nodeGraphPalette"));

        auto* tree = new PaletteTree(dock);
        tree->header()->close();
        tree->setSelectionMode(QAbstractItemView::SingleSelection);
        tree->setDragEnabled(true);
        tree->setDragDropMode(QAbstractItemView::DragOnly);
        PopulateNodeTree(tree);

        QObject::connect(tree, &QTreeWidget::itemDoubleClicked, this,
                         [this](QTreeWidgetItem* item, int) {
                             const QString typeId = item->data(0, Qt::UserRole).toString();
                             if (typeId.isEmpty())
                             {
                                 return;
                             }

                             AddNodeAt(typeId, mView->mapToScene(mView->viewport()->rect().center()));
                         });

        auto* body = new QWidget(dock);
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(4, 4, 4, 4);
        auto* hint = new QLabel(
            QStringLiteral("Drag onto the canvas, or double-click to add at the centre.\n"
                           "Press Q on the canvas for the same list."),
            body);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        layout->addWidget(tree);
        dock->setWidget(body);

        addDockWidget(Qt::LeftDockWidgetArea, dock);
        mViewMenu->addAction(dock->toggleViewAction());
    }

    void BuildVariablesDock()
    {
        auto* dock = new QDockWidget(QStringLiteral("Variables"), this);
        dock->setObjectName(QStringLiteral("nodeGraphVariables"));

        auto* body = new QWidget(dock);
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(4, 4, 4, 4);

        auto* hint = new QLabel(QStringLiteral("Drag a name onto the canvas to get or set it."), body);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        mVariableTable = new VariableTable(0, 3, body);
        mVariableTable->setHorizontalHeaderLabels({ QStringLiteral("Name"), QStringLiteral("Type"),
                                                    QStringLiteral("Default") });
        mVariableTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        mVariableTable->verticalHeader()->setVisible(false);
        mVariableTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        mVariableTable->setDragEnabled(true);
        mVariableTable->setDragDropMode(QAbstractItemView::DragOnly);
        layout->addWidget(mVariableTable);

        auto* buttons = new QWidget(body);
        auto* buttonLayout = new QHBoxLayout(buttons);
        buttonLayout->setContentsMargins(0, 0, 0, 0);

        auto* addButton = new QPushButton(QStringLiteral("Add"), buttons);
        auto* removeButton = new QPushButton(QStringLiteral("Remove"), buttons);
        buttonLayout->addWidget(addButton);
        buttonLayout->addWidget(removeButton);
        buttonLayout->addStretch(1);
        layout->addWidget(buttons);

        QObject::connect(addButton, &QPushButton::clicked, this, [this] {
            NodeGraphVariable variable;
            variable.Name = UniqueVariableName();
            variable.Type = NodeGraphVariableType::Float;
            variable.DefaultValue = NodeGraphVariableTypeDefault(variable.Type);
            mContext.Variables.push_back(variable);
            RefreshVariableTable();
            RefreshVariablePickers();
            MarkEdited();
        });

        QObject::connect(removeButton, &QPushButton::clicked, this, [this] {
            const int row = mVariableTable->currentRow();
            if (row < 0 || static_cast<std::size_t>(row) >= mContext.Variables.size())
            {
                return;
            }

            mContext.Variables.erase(mContext.Variables.begin() + row);
            RefreshVariableTable();
            RefreshVariablePickers();
            MarkEdited();
        });

        QObject::connect(mVariableTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
            if (mSuspendVariableSignals)
            {
                return;
            }

            const int row = item->row();
            if (row < 0 || static_cast<std::size_t>(row) >= mContext.Variables.size())
            {
                return;
            }

            NodeGraphVariable& variable = mContext.Variables[row];
            if (item->column() == 0)
            {
                const std::string requested = item->text().toStdString();
                if (requested.empty() || NameTakenByAnother(requested, row))
                {
                    // Two variables with one name would make every picker ambiguous, so
                    // put the old name back rather than accepting a broken table.
                    RefreshVariableTable();
                    return;
                }

                variable.Name = requested;
            }
            else if (item->column() == 2)
            {
                variable.DefaultValue = NormalisedDefault(item->text().toStdString(), variable.Type);
                if (variable.DefaultValue != item->text().toStdString())
                {
                    const QSignalBlocker blocker(mVariableTable);
                    item->setText(QString::fromStdString(variable.DefaultValue));
                }
            }

            RefreshVariablePickers();
            MarkEdited();
        });

        dock->setWidget(body);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        mViewMenu->addAction(dock->toggleViewAction());
    }

    void BuildFunctionsDock()
    {
        auto* dock = new QDockWidget(QStringLiteral("Functions"), this);
        dock->setObjectName(QStringLiteral("nodeGraphFunctions"));

        auto* body = new QWidget(dock);
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(4, 4, 4, 4);

        auto* hint = new QLabel(
            QStringLiteral("Every Function Entry in the graph, including imported ones.\n"
                           "Double-click to add a Call node at the centre."),
            body);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        mFunctionList = new QListWidget(body);
        layout->addWidget(mFunctionList);

        QObject::connect(mFunctionList, &QListWidget::itemDoubleClicked, this,
                         [this](QListWidgetItem* item) {
                             const QPointF scenePos =
                                 mView->mapToScene(mView->viewport()->rect().center());
                             const NodeId nodeId = mModel->addNode(QStringLiteral("Function.Call"));
                             if (nodeId == QtNodes::InvalidNodeId)
                             {
                                 return;
                             }

                             mModel->setNodeData(nodeId, NodeRole::Position, scenePos);
                             if (auto* delegate = mModel->delegateModel<PteroNodeDelegate>(nodeId))
                             {
                                 delegate->SetParams({ { "Function", item->text().toStdString() } });
                             }

                             MarkEdited();
                         });

        dock->setWidget(body);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        mViewMenu->addAction(dock->toggleViewAction());
    }

    std::string UniqueVariableName() const
    {
        for (int suffix = 1;; ++suffix)
        {
            const std::string candidate = "Variable" + std::to_string(suffix);
            bool taken = false;
            for (const NodeGraphVariable& variable : mContext.Variables)
            {
                taken = taken || variable.Name == candidate;
            }

            if (!taken)
            {
                return candidate;
            }
        }
    }

    bool NameTakenByAnother(const std::string& name, int row) const
    {
        for (std::size_t i = 0; i < mContext.Variables.size(); ++i)
        {
            if (static_cast<int>(i) != row && mContext.Variables[i].Name == name)
            {
                return true;
            }
        }

        return false;
    }

    void RefreshVariableTable()
    {
        mSuspendVariableSignals = true;

        mVariableTable->setRowCount(static_cast<int>(mContext.Variables.size()));
        for (std::size_t i = 0; i < mContext.Variables.size(); ++i)
        {
            const NodeGraphVariable& variable = mContext.Variables[i];
            const int row = static_cast<int>(i);

            mVariableTable->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(variable.Name)));

            auto* typeCombo = new QComboBox(mVariableTable);
            std::size_t typeCount = 0;
            const NodeGraphVariableType* types = NodeGraphVariableTypes(typeCount);
            for (std::size_t t = 0; t < typeCount; ++t)
            {
                // The id is carried in the data role so the visible name can stay
                // capitalised without the lookup depending on how it is spelled.
                typeCombo->addItem(QString::fromUtf8(NodeGraphVariableTypeName(types[t])),
                                   QString::fromUtf8(NodeGraphVariableTypeId(types[t])));
            }
            typeCombo->setCurrentIndex(
                typeCombo->findData(QString::fromUtf8(NodeGraphVariableTypeId(variable.Type))));
            QObject::connect(typeCombo, &QComboBox::currentIndexChanged, this,
                             [this, row, typeCombo](int) {
                                 if (row < 0 || static_cast<std::size_t>(row) >= mContext.Variables.size())
                                 {
                                     return;
                                 }

                                 NodeGraphVariable& changed = mContext.Variables[row];
                                 changed.Type = NodeGraphVariableTypeFromId(
                                     typeCombo->currentData().toString().toStdString());

                                 // Re-run the stored text through the new type so a
                                 // default left over from the old one cannot survive as
                                 // something the type could never produce.
                                 changed.DefaultValue =
                                     NormalisedDefault(changed.DefaultValue, changed.Type);

                                 // Update the one cell rather than rebuilding the table:
                                 // this runs inside the combo's own signal, and
                                 // setCellWidget would delete the combo out from under it.
                                 mSuspendVariableSignals = true;
                                 if (QTableWidgetItem* cell = mVariableTable->item(row, 2))
                                 {
                                     cell->setText(QString::fromStdString(changed.DefaultValue));
                                 }
                                 mSuspendVariableSignals = false;

                                 RefreshVariablePickers();
                                 MarkEdited();
                             });
            mVariableTable->setCellWidget(row, 1, typeCombo);

            mVariableTable->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(variable.DefaultValue)));
        }

        mSuspendVariableSignals = false;
    }

    void RefreshVariablePickers()
    {
        for (const NodeId nodeId : mModel->allNodeIds())
        {
            if (auto* delegate = mModel->delegateModel<PteroNodeDelegate>(nodeId))
            {
                delegate->RefreshVariableChoices();
            }
        }
    }

    // The set of functions is not declared anywhere: a Function Entry node *is* the
    // declaration. Rescanning is what keeps the Call pickers and the Functions dock honest
    // after a rename, a delete or an import, with nothing to fall out of sync.
    void RefreshFunctions()
    {
        QStringList found;
        for (const NodeId nodeId : mModel->allNodeIds())
        {
            auto* delegate = mModel->delegateModel<PteroNodeDelegate>(nodeId);
            if (delegate == nullptr || std::strcmp(delegate->Type().Id, "Function.Entry") != 0)
            {
                continue;
            }

            const auto params = delegate->ParamsAsStdMap();
            const auto it = params.find("Function");
            if (it == params.end() || it->second.empty())
            {
                continue;
            }

            const QString name = QString::fromStdString(it->second);
            if (!found.contains(name))
            {
                found.append(name);
            }
        }

        found.sort(Qt::CaseInsensitive);
        if (found == mContext.Functions)
        {
            return;
        }

        mContext.Functions = found;
        RefreshVariablePickers();
        RefreshFunctionList();
    }

    void RefreshFunctionList()
    {
        if (mFunctionList == nullptr)
        {
            return;
        }

        mFunctionList->clear();
        mFunctionList->addItems(mContext.Functions);
    }

    void LoadFromFile()
    {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Node Graph"), mExportPath,
            QString::fromUtf8(NodeGraphDocument::FileDialogFilter()) + QStringLiteral(";;All files (*.*)"));

        if (path.isEmpty())
        {
            return;
        }

        NodeGraphDocument document;
        std::string error;
        if (!document.LoadFromFile(path.toStdString(), &error))
        {
            QMessageBox::warning(this, QStringLiteral("Load Node Graph"), QString::fromStdString(error));
            return;
        }

        SetDocument(document);
        mExportPath = path;
        MarkEdited();
    }

    // Copies an exported graph in as a library rather than replacing what is open.
    //
    // The library is merged rather than referenced by path, so a level stays a single
    // self-contained file and there is no path to go stale between the level and its
    // scripts. The cost is that re-importing is how an updated library is picked up.
    void ImportLibrary()
    {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import Graph as Library"), mExportPath,
            QString::fromUtf8(NodeGraphDocument::FileDialogFilter()));

        if (path.isEmpty())
        {
            return;
        }

        NodeGraphDocument library;
        std::string error;
        if (!library.LoadFromFile(path.toStdString(), &error))
        {
            QMessageBox::warning(this, QStringLiteral("Import Graph"), QString::fromStdString(error));
            return;
        }

        const QString suggested = QFileInfo(path).completeBaseName();
        bool accepted = false;
        const QString prefix = QInputDialog::getText(
            this, QStringLiteral("Import Graph as Library"),
            QStringLiteral("Name to qualify the library's functions and variables with:"),
            QLineEdit::Normal, suggested, &accepted);

        if (!accepted)
        {
            return;
        }

        // Drop the copy clear of whatever is already on the canvas, so an import never
        // lands on top of existing nodes.
        QRectF used;
        for (const NodeId nodeId : mModel->allNodeIds())
        {
            if (QGraphicsObject* object = mScene->nodeGraphicsObject(nodeId))
            {
                used = used.united(object->sceneBoundingRect());
            }
        }

        constexpr double gap = 120.0;
        const double offsetY = used.isValid() ? used.bottom() + gap : 0.0;

        NodeGraphDocument merged = BuildDocument();
        const std::vector<int> added = merged.Merge(library, prefix.toStdString(), 0.0, offsetY);

        if (added.empty())
        {
            QMessageBox::information(this, QStringLiteral("Import Graph"),
                                     QStringLiteral("That graph has no nodes to import."));
            return;
        }

        // Label the block so it is obvious on the canvas where a library begins and ends.
        QRectF imported;
        for (const int id : added)
        {
            if (const NodeGraphNode* node = merged.FindNode(id))
            {
                imported = imported.united(QRectF(node->X, node->Y, 220.0, 120.0));
            }
        }

        NodeGraphComment banner;
        banner.Id = 1;
        for (const NodeGraphComment& comment : merged.Comments)
        {
            banner.Id = (std::max)(banner.Id, comment.Id + 1);
        }
        banner.Text = prefix.isEmpty() ? suggested.toStdString() : prefix.toStdString();
        banner.X = imported.left() - 40.0;
        banner.Y = imported.top() - 40.0;
        banner.Width = imported.width() + 80.0;
        banner.Height = imported.height() + 80.0;
        banner.Color = 0x3f5a2f;
        merged.Comments.push_back(banner);

        SetDocument(merged);
        MarkEdited();

        statusBar()->showMessage(
            QStringLiteral("Imported %1 (%2 node(s), %3 function(s)).")
                .arg(QFileInfo(path).fileName())
                .arg(static_cast<int>(added.size()))
                .arg(static_cast<int>(library.FunctionNames().size())),
            8000);
    }

    void ExportToFile()
    {
        QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export Node Graph"), mExportPath,
            QString::fromUtf8(NodeGraphDocument::FileDialogFilter()));

        if (path.isEmpty())
        {
            return;
        }

        const QString suffix = QStringLiteral(".") + QString::fromUtf8(NodeGraphDocument::FileExtension());
        if (!path.endsWith(suffix, Qt::CaseInsensitive))
        {
            path += suffix;
        }

        std::string error;
        if (!BuildDocument().SaveToFile(path.toStdString(), &error))
        {
            QMessageBox::warning(this, QStringLiteral("Export Node Graph"), QString::fromStdString(error));
            return;
        }

        mExportPath = path;
        UpdateStatus();
    }

    GraphContext mContext;
    std::shared_ptr<QtNodes::NodeDelegateModelRegistry> mRegistry;
    std::unique_ptr<PteroGraphModel> mModel;
    PteroNodeScene* mScene = nullptr;
    PteroGraphicsView* mView = nullptr;
    QMenu* mViewMenu = nullptr;
    VariableTable* mVariableTable = nullptr;
    QListWidget* mFunctionList = nullptr;

    // Comment boxes are scene items the graph model knows nothing about, so the window
    // owns them outright and is responsible for their lifetime.
    std::vector<CommentBoxItem*> mComments;
    int mNextCommentId = 1;

    QString mGraphName = QStringLiteral("Level Graph");
    QString mExportPath;
    std::function<void()> mOnEdited;
    bool mSuspendEdits = false;
    bool mSuspendVariableSignals = false;
};

// ---------------------------------------------------------------------------------------
// Facade state
// ---------------------------------------------------------------------------------------

NodeGraphWindow* gWindow = nullptr;
// Authoritative copy of the graph. The window writes into it on every edit, so the level
// serializer and the runtime can read it without the window having to exist.
NodeGraphDocument gDocument;
std::string gExportPath;
unsigned gRevision = 0;

NodeGraphWindow* EnsureWindow()
{
    if (gWindow != nullptr)
    {
        return gWindow;
    }

    // Qt has to be up before a widget can be built. QtUi owns the QApplication, so a call
    // that arrives before the editor is initialised simply keeps working on the document.
    if (QApplication::instance() == nullptr)
    {
        return nullptr;
    }

    gWindow = new NodeGraphWindow(static_cast<QWidget*>(QtUi::ShellWidget()));
    gWindow->SetEditedCallback([] {
        if (gWindow == nullptr)
        {
            return;
        }

        gDocument = gWindow->BuildDocument();
        gExportPath = gWindow->ExportPath().toStdString();
        ++gRevision;
    });

    gWindow->SetDocument(gDocument);
    return gWindow;
}
} // namespace

namespace NodeGraphEditor
{
void Show()
{
    NodeGraphWindow* window = EnsureWindow();
    if (window == nullptr)
    {
        return;
    }

    // Entities may have been renamed while the window was closed.
    window->RefreshEntityPickers();
    window->show();
    window->raise();
    window->activateWindow();
}

void Hide()
{
    if (gWindow != nullptr)
    {
        gWindow->hide();
    }
}

bool IsVisible()
{
    return gWindow != nullptr && gWindow->isVisible();
}

void Shutdown()
{
    delete gWindow;
    gWindow = nullptr;
}

const NodeGraphDocument& Document()
{
    return gDocument;
}

void SetDocument(const NodeGraphDocument& document)
{
    gDocument = document;
    gExportPath.clear();

    if (gWindow != nullptr)
    {
        gWindow->SetDocument(gDocument);
    }

    // A load is not an edit: reset the baseline so opening a level does not immediately
    // report the graph as changed.
    gRevision = 0;
}

unsigned Revision()
{
    return gRevision;
}

const std::string& ExportPath()
{
    return gExportPath;
}

void SetEntitySource(
    std::function<std::vector<NodeGraphEntityInfo>()> listEntities,
    std::function<std::uint64_t()> selectedEntity)
{
    gListEntities = std::move(listEntities);
    gSelectedEntity = std::move(selectedEntity);

    if (gWindow != nullptr)
    {
        gWindow->RefreshEntityPickers();
    }
}
} // namespace NodeGraphEditor
