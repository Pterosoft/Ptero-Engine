#pragma once

#include "System/NodeGraphCatalog.h"

#include <map>
#include <string>
#include <vector>

// The saved form of a node graph.
//
// A document is a flat list of nodes, the connections between their ports, and the
// variables the graph declares. It is deliberately just data: the Qt editor builds a
// QtNodes scene from it, the runtime interprets it, and neither owns it.
//
// Documents travel as JSON, either inside a level file under "NodeGraph" or as a
// standalone ".nodegraph" file. Both carry the identical object, so a graph can be
// exported out of one level and imported into another.

// The C++ types a graph variable can hold.
//
// Pins stay deliberately coarser than this - every numeric kind travels on a Number pin -
// so widening this list never invalidates an existing connection. The declared type only
// decides how a value is coerced when it is written, how its default is read back, and
// which editor the Variables dock offers for it.
enum class NodeGraphVariableType
{
    Bool,
    Int,
    Float,
    Double,
    String,
    // A string the editor edits in a multi-line box. Identical to String at runtime.
    Text
};

const char* NodeGraphVariableTypeId(NodeGraphVariableType type);
const char* NodeGraphVariableTypeName(NodeGraphVariableType type);
NodeGraphVariableType NodeGraphVariableTypeFromId(const std::string& id);

// Which pin a Get/Set node for this variable talks over.
NodePinKind NodeGraphVariablePinKind(NodeGraphVariableType type);

// Every declared type, in the order the editor should list them.
const NodeGraphVariableType* NodeGraphVariableTypes(std::size_t& count);

// The default a freshly declared variable of this type should start with, so the stored
// text is always something the type can actually parse.
const char* NodeGraphVariableTypeDefault(NodeGraphVariableType type);

struct NodeGraphVariable
{
    std::string Name;
    NodeGraphVariableType Type = NodeGraphVariableType::Float;
    std::string DefaultValue;
};

struct NodeGraphNode
{
    // Unique inside the document. Connections address nodes by this id, so it survives
    // save and load unchanged.
    int Id = 0;
    std::string TypeId;
    double X = 0.0;
    double Y = 0.0;
    // Values typed on the node body, keyed by NodeGraphParam::Key. Stored as text so the
    // document format does not have to grow a type tag for every parameter.
    std::map<std::string, std::string> Params;

    const std::string* FindParam(const std::string& key) const;
};

struct NodeGraphConnection
{
    int FromNode = 0;
    int FromPort = 0;
    int ToNode = 0;
    int ToPort = 0;
};

// A titled frame drawn behind the nodes. Purely documentation - the runtime never looks
// at one - but dragging a box carries the nodes sitting inside it, which is what makes a
// large graph navigable.
struct NodeGraphComment
{
    int Id = 0;
    std::string Text = "Comment";
    double X = 0.0;
    double Y = 0.0;
    double Width = 360.0;
    double Height = 220.0;
    // 0xRRGGBB, picked by the user from the box's own context menu.
    unsigned Color = 0x2f6690u;
};

class NodeGraphDocument
{
public:
    std::string Name = "Level Graph";
    std::vector<NodeGraphVariable> Variables;
    std::vector<NodeGraphNode> Nodes;
    std::vector<NodeGraphConnection> Connections;
    std::vector<NodeGraphComment> Comments;

    void Clear();
    bool IsEmpty() const;

    const NodeGraphNode* FindNode(int id) const;
    const NodeGraphVariable* FindVariable(const std::string& name) const;

    // Compact by default; the standalone file writer indents so a .nodegraph stays
    // reviewable in a diff.
    std::string ToJsonString(bool indented = false) const;
    bool FromJsonString(const std::string& text, std::string* errorMessage = nullptr);

    bool SaveToFile(const std::string& filePath, std::string* errorMessage = nullptr) const;
    bool LoadFromFile(const std::string& filePath, std::string* errorMessage = nullptr);

    // Names of every function this document defines, in declaration order.
    std::vector<std::string> FunctionNames() const;

    // Copies another document in as a library.
    //
    // Everything the library names - its functions and its variables - is prefixed with
    // `prefix`, so importing a player controller twice, or alongside a graph that happens
    // to use the same names, cannot collide. References inside the library are rewritten
    // to match, which is what lets its own Call and Get/Set nodes keep working. Node ids
    // are renumbered past whatever this document already uses, and positions are shifted
    // by the given offset so the imported nodes land clear of the existing ones.
    //
    // Returns the ids of the nodes that were added.
    std::vector<int> Merge(const NodeGraphDocument& library,
                           const std::string& prefix,
                           double offsetX,
                           double offsetY);

    // Extension and filter used by every node graph file dialog in the editor.
    static const char* FileExtension();
    static const char* FileDialogFilter();
};
