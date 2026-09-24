// The NodeGraphEditor facade for the packaged game's renderer, which has no Qt and so no
// Node Graph window. It keeps what the runtime needs - the level's graph document -
// and does nothing for the window calls. Compiled in place of NodeGraphEditorWindow.cpp
// when Renderer_DX12 is built with PteroGameRuntime=true.

#include "System/NodeGraphEditor.h"

namespace
{
NodeGraphDocument gDocument;
std::string gExportPath;
}

namespace NodeGraphEditor
{
void Show() {}
void Hide() {}
bool IsVisible() { return false; }
void Shutdown() {}

const NodeGraphDocument& Document()
{
    return gDocument;
}

void SetDocument(const NodeGraphDocument& document)
{
    gDocument = document;
    gExportPath.clear();
}

unsigned Revision()
{
    return 0;
}

const std::string& ExportPath()
{
    return gExportPath;
}

void SetEntitySource(std::function<std::vector<NodeGraphEntityInfo>()>, std::function<std::uint64_t()>) {}
} // namespace NodeGraphEditor
