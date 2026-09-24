#include "System/DataFiles.h"
#include "System/NodeGraphDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>

using json = nlohmann::json;

namespace
{
    // Bumped only for a change no older reader can cope with. Unknown node types are
    // already dropped with a message rather than failing the load, so adding nodes does
    // not need a new version.
    constexpr int kDocumentVersion = 1;
    constexpr const char* kVersionKey = "PteroNodeGraph";

    constexpr NodeGraphVariableType kVariableTypes[] = {
        NodeGraphVariableType::Bool,   NodeGraphVariableType::Int,    NodeGraphVariableType::Float,
        NodeGraphVariableType::Double, NodeGraphVariableType::String, NodeGraphVariableType::Text
    };
}

const char* NodeGraphVariableTypeId(NodeGraphVariableType type)
{
    switch (type)
    {
    case NodeGraphVariableType::Bool: return "bool";
    case NodeGraphVariableType::Int: return "int";
    case NodeGraphVariableType::Float: return "float";
    case NodeGraphVariableType::Double: return "double";
    case NodeGraphVariableType::String: return "string";
    case NodeGraphVariableType::Text: return "text";
    }

    return "float";
}

const char* NodeGraphVariableTypeName(NodeGraphVariableType type)
{
    switch (type)
    {
    case NodeGraphVariableType::Bool: return "Bool";
    case NodeGraphVariableType::Int: return "Int";
    case NodeGraphVariableType::Float: return "Float";
    case NodeGraphVariableType::Double: return "Double";
    case NodeGraphVariableType::String: return "String";
    case NodeGraphVariableType::Text: return "Text";
    }

    return "Float";
}

NodeGraphVariableType NodeGraphVariableTypeFromId(const std::string& id)
{
    for (const NodeGraphVariableType type : kVariableTypes)
    {
        if (id == NodeGraphVariableTypeId(type))
        {
            return type;
        }
    }

    // Documents written before the type list was split into C++ types recorded every
    // numeric variable as "number". Float is the closest match and the one users expect;
    // a graph that genuinely needs the extra precision can say so explicitly.
    return NodeGraphVariableType::Float;
}

NodePinKind NodeGraphVariablePinKind(NodeGraphVariableType type)
{
    switch (type)
    {
    case NodeGraphVariableType::Bool: return NodePinKind::Bool;
    case NodeGraphVariableType::String:
    case NodeGraphVariableType::Text: return NodePinKind::String;
    default: return NodePinKind::Number;
    }
}

const NodeGraphVariableType* NodeGraphVariableTypes(std::size_t& count)
{
    count = sizeof(kVariableTypes) / sizeof(kVariableTypes[0]);
    return kVariableTypes;
}

const char* NodeGraphVariableTypeDefault(NodeGraphVariableType type)
{
    switch (type)
    {
    case NodeGraphVariableType::Bool: return "false";
    case NodeGraphVariableType::String:
    case NodeGraphVariableType::Text: return "";
    default: return "0";
    }
}

const std::string* NodeGraphNode::FindParam(const std::string& key) const
{
    const auto it = Params.find(key);
    return it == Params.end() ? nullptr : &it->second;
}

void NodeGraphDocument::Clear()
{
    Name = "Level Graph";
    Variables.clear();
    Nodes.clear();
    Connections.clear();
    Comments.clear();
}

bool NodeGraphDocument::IsEmpty() const
{
    return Nodes.empty() && Connections.empty() && Variables.empty() && Comments.empty();
}

const NodeGraphNode* NodeGraphDocument::FindNode(int id) const
{
    for (const NodeGraphNode& node : Nodes)
    {
        if (node.Id == id)
        {
            return &node;
        }
    }

    return nullptr;
}

const NodeGraphVariable* NodeGraphDocument::FindVariable(const std::string& name) const
{
    for (const NodeGraphVariable& variable : Variables)
    {
        if (variable.Name == name)
        {
            return &variable;
        }
    }

    return nullptr;
}

std::string NodeGraphDocument::ToJsonString(bool indented) const
{
    json documentJson;
    documentJson[kVersionKey] = kDocumentVersion;
    documentJson["Name"] = Name;

    json variablesJson = json::array();
    for (const NodeGraphVariable& variable : Variables)
    {
        variablesJson.push_back(json{
            { "Name", variable.Name },
            { "Type", NodeGraphVariableTypeId(variable.Type) },
            { "Default", variable.DefaultValue }
        });
    }
    documentJson["Variables"] = variablesJson;

    json nodesJson = json::array();
    for (const NodeGraphNode& node : Nodes)
    {
        json nodeJson{
            { "Id", node.Id },
            { "Type", node.TypeId },
            { "X", node.X },
            { "Y", node.Y }
        };

        if (!node.Params.empty())
        {
            json paramsJson = json::object();
            for (const auto& [key, value] : node.Params)
            {
                paramsJson[key] = value;
            }

            nodeJson["Params"] = paramsJson;
        }

        nodesJson.push_back(nodeJson);
    }
    documentJson["Nodes"] = nodesJson;

    json connectionsJson = json::array();
    for (const NodeGraphConnection& connection : Connections)
    {
        connectionsJson.push_back(json{
            { "FromNode", connection.FromNode },
            { "FromPort", connection.FromPort },
            { "ToNode", connection.ToNode },
            { "ToPort", connection.ToPort }
        });
    }
    documentJson["Connections"] = connectionsJson;

    if (!Comments.empty())
    {
        json commentsJson = json::array();
        for (const NodeGraphComment& comment : Comments)
        {
            commentsJson.push_back(json{
                { "Id", comment.Id },
                { "Text", comment.Text },
                { "X", comment.X },
                { "Y", comment.Y },
                { "Width", comment.Width },
                { "Height", comment.Height },
                { "Color", comment.Color }
            });
        }

        documentJson["Comments"] = commentsJson;
    }

    return indented ? documentJson.dump(4) : documentJson.dump();
}

bool NodeGraphDocument::FromJsonString(const std::string& text, std::string* errorMessage)
{
    if (text.empty())
    {
        Clear();
        return true;
    }

    json documentJson;
    try
    {
        documentJson = json::parse(text);
    }
    catch (const json::exception& parseError)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::string("Not a valid node graph: ") + parseError.what();
        }

        return false;
    }

    if (!documentJson.is_object())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Not a valid node graph: the document is not a JSON object.";
        }

        return false;
    }

    const int version = documentJson.value(kVersionKey, kDocumentVersion);
    if (version > kDocumentVersion)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "This node graph was written by a newer build of the editor.";
        }

        return false;
    }

    Clear();
    Name = documentJson.value("Name", std::string("Level Graph"));

    if (documentJson.contains("Variables") && documentJson["Variables"].is_array())
    {
        for (const json& variableJson : documentJson["Variables"])
        {
            NodeGraphVariable variable;
            variable.Name = variableJson.value("Name", std::string());
            variable.Type = NodeGraphVariableTypeFromId(variableJson.value("Type", std::string("float")));
            variable.DefaultValue = variableJson.value("Default", std::string());

            if (!variable.Name.empty() && FindVariable(variable.Name) == nullptr)
            {
                Variables.push_back(variable);
            }
        }
    }

    if (documentJson.contains("Nodes") && documentJson["Nodes"].is_array())
    {
        for (const json& nodeJson : documentJson["Nodes"])
        {
            NodeGraphNode node;
            node.Id = nodeJson.value("Id", 0);
            node.TypeId = nodeJson.value("Type", std::string());
            node.X = nodeJson.value("X", 0.0);
            node.Y = nodeJson.value("Y", 0.0);

            if (node.TypeId.empty() || FindNode(node.Id) != nullptr)
            {
                continue;
            }

            if (nodeJson.contains("Params") && nodeJson["Params"].is_object())
            {
                for (const auto& [key, value] : nodeJson["Params"].items())
                {
                    // Older documents may have written numbers and booleans directly;
                    // normalise everything to the text form the editor now uses.
                    if (value.is_string())
                    {
                        node.Params[key] = value.get<std::string>();
                    }
                    else if (value.is_boolean())
                    {
                        node.Params[key] = value.get<bool>() ? "true" : "false";
                    }
                    else if (value.is_number())
                    {
                        node.Params[key] = std::to_string(value.get<double>());
                    }
                }
            }

            Nodes.push_back(std::move(node));
        }
    }

    if (documentJson.contains("Connections") && documentJson["Connections"].is_array())
    {
        for (const json& connectionJson : documentJson["Connections"])
        {
            NodeGraphConnection connection;
            connection.FromNode = connectionJson.value("FromNode", -1);
            connection.FromPort = connectionJson.value("FromPort", -1);
            connection.ToNode = connectionJson.value("ToNode", -1);
            connection.ToPort = connectionJson.value("ToPort", -1);

            // A connection to a node that failed to load would leave a dangling edge the
            // editor cannot draw, so drop it here rather than downstream.
            if (connection.FromPort < 0 || connection.ToPort < 0 ||
                FindNode(connection.FromNode) == nullptr || FindNode(connection.ToNode) == nullptr)
            {
                continue;
            }

            Connections.push_back(connection);
        }
    }

    if (documentJson.contains("Comments") && documentJson["Comments"].is_array())
    {
        for (const json& commentJson : documentJson["Comments"])
        {
            NodeGraphComment comment;
            comment.Id = commentJson.value("Id", 0);
            comment.Text = commentJson.value("Text", std::string("Comment"));
            comment.X = commentJson.value("X", 0.0);
            comment.Y = commentJson.value("Y", 0.0);
            comment.Width = commentJson.value("Width", 360.0);
            comment.Height = commentJson.value("Height", 220.0);
            comment.Color = commentJson.value("Color", 0x2f6690u);
            Comments.push_back(std::move(comment));
        }
    }

    return true;
}

bool NodeGraphDocument::SaveToFile(const std::string& filePath, std::string* errorMessage) const
{
    std::ofstream outputStream(filePath, std::ios::binary);
    if (!outputStream)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Could not open " + filePath + " for writing.";
        }

        return false;
    }

    outputStream << ToJsonString(true);
    if (!outputStream)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed while writing " + filePath + ".";
        }

        return false;
    }

    return true;
}

bool NodeGraphDocument::LoadFromFile(const std::string& filePath, std::string* errorMessage)
{
    DataFiles::InputFile inputStream(filePath, std::ios::binary);
    if (!inputStream)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Could not open " + filePath + ".";
        }

        return false;
    }

    const std::string text((std::istreambuf_iterator<char>(inputStream)), std::istreambuf_iterator<char>());
    return FromJsonString(text, errorMessage);
}

std::vector<std::string> NodeGraphDocument::FunctionNames() const
{
    std::vector<std::string> names;
    for (const NodeGraphNode& node : Nodes)
    {
        if (node.TypeId != "Function.Entry")
        {
            continue;
        }

        const std::string* name = node.FindParam("Function");
        if (name == nullptr || name->empty())
        {
            continue;
        }

        if (std::find(names.begin(), names.end(), *name) == names.end())
        {
            names.push_back(*name);
        }
    }

    return names;
}

std::vector<int> NodeGraphDocument::Merge(const NodeGraphDocument& library,
                                          const std::string& prefix,
                                          double offsetX,
                                          double offsetY)
{
    // Renumber past everything already here, so a library carrying node id 1 cannot land
    // on top of this document's node 1.
    int nextId = 1;
    for (const NodeGraphNode& node : Nodes)
    {
        nextId = (std::max)(nextId, node.Id + 1);
    }

    const auto qualify = [&prefix](const std::string& name) {
        return prefix.empty() || name.empty() ? name : prefix + "." + name;
    };

    for (const NodeGraphVariable& variable : library.Variables)
    {
        NodeGraphVariable copy = variable;
        copy.Name = qualify(variable.Name);
        if (FindVariable(copy.Name) == nullptr)
        {
            Variables.push_back(std::move(copy));
        }
    }

    std::map<int, int> idMap;
    std::vector<int> addedIds;
    addedIds.reserve(library.Nodes.size());

    for (const NodeGraphNode& node : library.Nodes)
    {
        NodeGraphNode copy = node;
        copy.Id = nextId++;
        copy.X += offsetX;
        copy.Y += offsetY;

        // Rewrite the names the library refers to so they point at the copies just made.
        // The catalogue says which parameters name something rather than hard-coding a
        // list here, so a future node type that picks a variable is covered automatically.
        if (const NodeGraphNodeType* type = NodeGraphCatalog::Find(copy.TypeId.c_str()))
        {
            for (unsigned i = 0; i < type->ParamCount; ++i)
            {
                const NodeGraphParam& param = type->Params[i];
                if (param.Kind != NodeParamKind::Variable && param.Kind != NodeParamKind::Function)
                {
                    continue;
                }

                const auto it = copy.Params.find(param.Key);
                if (it != copy.Params.end())
                {
                    it->second = qualify(it->second);
                }
            }

            // Function.Entry names its function through a plain string parameter, since
            // there is nothing to pick from when the function is being defined.
            if (copy.TypeId == "Function.Entry")
            {
                const auto it = copy.Params.find("Function");
                if (it != copy.Params.end())
                {
                    it->second = qualify(it->second);
                }
            }
        }

        idMap[node.Id] = copy.Id;
        addedIds.push_back(copy.Id);
        Nodes.push_back(std::move(copy));
    }

    for (const NodeGraphConnection& connection : library.Connections)
    {
        const auto from = idMap.find(connection.FromNode);
        const auto to = idMap.find(connection.ToNode);
        if (from == idMap.end() || to == idMap.end())
        {
            continue;
        }

        NodeGraphConnection copy = connection;
        copy.FromNode = from->second;
        copy.ToNode = to->second;
        Connections.push_back(copy);
    }

    int nextCommentId = 1;
    for (const NodeGraphComment& comment : Comments)
    {
        nextCommentId = (std::max)(nextCommentId, comment.Id + 1);
    }

    for (const NodeGraphComment& comment : library.Comments)
    {
        NodeGraphComment copy = comment;
        copy.Id = nextCommentId++;
        copy.X += offsetX;
        copy.Y += offsetY;
        Comments.push_back(std::move(copy));
    }

    return addedIds;
}

const char* NodeGraphDocument::FileExtension()
{
    return "nodegraph";
}

const char* NodeGraphDocument::FileDialogFilter()
{
    // A graph is plain JSON, so a .json written by hand or by a tool opens just as well as
    // one the editor exported; the extension only decides what the dialog offers first.
    return "Ptero Node Graph (*.nodegraph *.json);;JSON (*.json);;All files (*.*)";
}
