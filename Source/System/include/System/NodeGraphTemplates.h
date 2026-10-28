#pragma once

#include "System/NodeGraphDocument.h"

// Ready-made graphs the editor can hand out, built in code so they always use the node
// set of the build that writes them.
namespace NodeGraphTemplates
{
    // The Node Graph twin of Game.dll's FirstPersonCharacter: WASD to move, mouse to look,
    // Space to jump (variable height), Ctrl/C to crouch, Shift to sprint, plus a right-mouse
    // zoom as an example of changing camera properties at runtime. Every movement, camera
    // and control value is set on its Set Movement/Camera/Control Settings nodes.
    //
    // Game Settings exports this as a .nodegraph to start a custom controller from, and
    // falls back to it when the selected controller file cannot be loaded.
    NodeGraphDocument FirstPersonController();
}
