#pragma once

#include <cstddef>

// The catalogue of every node type the Node Graph understands.
//
// One table drives three things that would otherwise drift apart: the palette and the
// port layout in the Qt editor, the shape of a saved .nodegraph document, and the
// interpreter that runs the graph during play. Adding a node means adding a row here and
// a case in NodeGraphRuntime; nothing else has to be touched.
//
// The table is deliberately free of Qt and of engine types so the runtime can execute a
// graph in a build that never creates the editor.

enum class NodePinKind
{
    // Execution flow, drawn white. Carries no value: an exec output fires exactly one
    // target, an exec input may be fired by many sources.
    Exec,
    Bool,
    Number,
    String,
    // Accepts a connection from any value pin. Used by the variable and multiplexer
    // nodes, whose type is only known once the graph declares its variables.
    Any,
    // A level entity, identified by the persistent id saved with it in the level - not by
    // its name or its position in the entity list, both of which change under editing.
    // Travels as a number at runtime; ids are kept below 2^53 so a double holds them
    // exactly.
    Entity
};

struct NodeGraphPin
{
    const char* Name;
    NodePinKind Kind;
};

enum class NodeParamKind
{
    Number,
    Bool,
    String,
    // Fixed choice list, `Items` holds the options separated by '|'.
    Enum,
    // Picks one of the graph's declared variables.
    Variable,
    // Picks one of the graph's functions - every Function Entry in the document, including
    // those merged in from an imported library.
    Function,
    // Picks an entity of the open level. Stored as the entity's persistent id in decimal,
    // so renaming or reordering entities never re-points a node at something else.
    Entity
};

// A value edited on the node body itself. `Key` matches the name of an input pin whenever
// the node has one, and the runtime then treats the parameter as that pin's default: a
// connected pin always wins, an unconnected pin falls back to what was typed on the node.
struct NodeGraphParam
{
    const char* Key;
    const char* Label;
    NodeParamKind Kind;
    const char* DefaultValue;
    const char* Items;
};

struct NodeGraphNodeType
{
    const char* Id;
    const char* Caption;
    const char* Category;
    const char* Tooltip;
    const NodeGraphPin* Inputs;
    unsigned InputCount;
    const NodeGraphPin* Outputs;
    unsigned OutputCount;
    const NodeGraphParam* Params;
    unsigned ParamCount;
};

namespace NodeGraphCatalog
{
    const NodeGraphNodeType* Types(std::size_t& count);

    // Null when the id is unknown, which is how both the editor and the runtime detect a
    // document written by a newer build.
    const NodeGraphNodeType* Find(const char* id);

    const NodeGraphPin* FindInput(const NodeGraphNodeType& type, const char* pinName);
    const NodeGraphParam* FindParam(const NodeGraphNodeType& type, const char* key);
    int IndexOfInput(const NodeGraphNodeType& type, const char* pinName);
    int IndexOfOutput(const NodeGraphNodeType& type, const char* pinName);

    // Stable identifier written into saved documents and used by the editor to decide
    // whether two ports may be connected.
    const char* PinKindId(NodePinKind kind);
    const char* PinKindName(NodePinKind kind);
    NodePinKind PinKindFromId(const char* id);

    // 0xRRGGBB, used for the port and connection colours in the editor.
    unsigned PinKindColor(NodePinKind kind);

    // Events are the graph's entry points: they take no execution input and the runtime
    // fires them itself.
    bool IsEventType(const NodeGraphNodeType& type);
}
