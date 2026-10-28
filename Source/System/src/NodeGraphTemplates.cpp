#include "System/NodeGraphTemplates.h"

#include "System/NodeGraphCatalog.h"

#include <initializer_list>
#include <string>
#include <utility>

namespace
{
    // Lays a graph out node by node. Ports are named rather than numbered, so a template
    // keeps working when a node grows a pin; a name that does not exist is a bug in the
    // template, and the connection is simply left out.
    class GraphBuilder
    {
    public:
        using Params = std::initializer_list<std::pair<const char*, const char*>>;

        explicit GraphBuilder(const char* name)
        {
            mDocument.Name = name;
        }

        int Node(const char* typeId, double x, double y, Params params = {})
        {
            NodeGraphNode node;
            node.Id = mNextId++;
            node.TypeId = typeId;
            node.X = x;
            node.Y = y;
            for (const auto& [key, value] : params)
            {
                node.Params[key] = value;
            }

            mDocument.Nodes.push_back(std::move(node));
            return mDocument.Nodes.back().Id;
        }

        void Link(int fromNode, const char* outputPin, int toNode, const char* inputPin)
        {
            const NodeGraphNodeType* fromType = TypeOf(fromNode);
            const NodeGraphNodeType* toType = TypeOf(toNode);
            if (fromType == nullptr || toType == nullptr)
            {
                return;
            }

            const int fromPort = NodeGraphCatalog::IndexOfOutput(*fromType, outputPin);
            const int toPort = NodeGraphCatalog::IndexOfInput(*toType, inputPin);
            if (fromPort < 0 || toPort < 0)
            {
                return;
            }

            mDocument.Connections.push_back({ fromNode, fromPort, toNode, toPort });
        }

        void Variable(const char* name, NodeGraphVariableType type, const char* defaultValue)
        {
            mDocument.Variables.push_back({ name, type, defaultValue });
        }

        void Comment(const char* text, double x, double y, double width, double height, unsigned color)
        {
            NodeGraphComment comment;
            comment.Id = mNextId++;
            comment.Text = text;
            comment.X = x;
            comment.Y = y;
            comment.Width = width;
            comment.Height = height;
            comment.Color = color;
            mDocument.Comments.push_back(std::move(comment));
        }

        NodeGraphDocument Take() { return std::move(mDocument); }

    private:
        const NodeGraphNodeType* TypeOf(int id) const
        {
            const NodeGraphNode* node = mDocument.FindNode(id);
            return node != nullptr ? NodeGraphCatalog::Find(node->TypeId.c_str()) : nullptr;
        }

        NodeGraphDocument mDocument;
        int mNextId = 1;
    };

    // Column spacing of the layout, wide enough for the widest node body.
    constexpr double C = 300.0;
}

namespace NodeGraphTemplates
{
    NodeGraphDocument FirstPersonController()
    {
        GraphBuilder g("First Person Controller");

        g.Variable("DefaultFieldOfView", NodeGraphVariableType::Float, "70");
        g.Variable("SprintToggled", NodeGraphVariableType::Bool, "false");

        // ---- Start: grab the mouse, remember the field of view -----------------------
        g.Comment("Start: capture the mouse for looking (Set Control Settings > Capture mouse) and remember the "
                  "field of view the zoom returns to",
                  -40, -80, C * 4 + 40, 420, 0x6b1f1fu);
        const int start = g.Node("Event.GameStart", 0, 0);
        const int startSequence = g.Node("Flow.Sequence", C, 0);

        // The character's whole configuration lives on these three nodes: edit the values
        // on their bodies to tune the controller. They run first, so everything after them
        // - the capture check, the remembered field of view - sees the configured values.
        g.Comment("Character settings: edit the values on these nodes to tune movement, camera and controls",
                  C * 2 - 40, -1000, C * 3 + 40, 940, 0x1c4a2cu);
        const int movementSettings = g.Node("Character.SetMovementSettings", C * 2, -920);
        const int cameraSettings = g.Node("Character.SetCameraSettings", C * 3, -920);
        const int controlSettings = g.Node("Character.SetControlSettings", C * 4, -920);
        g.Link(start, "Then", startSequence, "In");
        g.Link(startSequence, "Then 0", movementSettings, "In");
        g.Link(movementSettings, "Then", cameraSettings, "In");
        g.Link(cameraSettings, "Then", controlSettings, "In");

        const int captureSetting = g.Node("Character.GetCameraProperty", C, 180, { { "Property", "Capture Mouse" } });
        const int captureBranch = g.Node("Flow.Branch", C * 2, 0);
        const int capture = g.Node("Input.SetMouseCaptured", C * 3, 0, { { "Captured", "true" } });
        const int fovSetting = g.Node("Character.GetCameraProperty", C * 2, 260, { { "Property", "Field of View" } });
        const int storeFov = g.Node("Variable.Set", C * 3, 180, { { "Variable", "DefaultFieldOfView" } });
        g.Link(startSequence, "Then 1", captureBranch, "In");
        g.Link(captureSetting, "Value", captureBranch, "Condition");
        g.Link(captureBranch, "True", capture, "In");
        g.Link(startSequence, "Then 2", storeFov, "In");
        g.Link(fovSetting, "Value", storeFov, "Value");

        // ---- Every frame: move, look, crouch, sprint ---------------------------------
        constexpr double tickY = 500;
        g.Comment("Every frame: WASD moves and the mouse looks. Crouch (Ctrl / C) and sprint (Shift) are held "
                  "here unless Set Control Settings makes them toggles - the key presses below handle those",
                  -40, tickY - 80, C * 6 + 40, 1500, 0x2f6690u);
        const int tick = g.Node("Event.Tick", 0, tickY);
        const int tickSequence = g.Node("Flow.Sequence", C, tickY);

        const int move = g.Node("Character.AddMovementInput", C * 3, tickY);
        const int forwardAxis = g.Node("Input.GetAxis", C * 2, tickY + 120, { { "Positive", "W" }, { "Negative", "S" } });
        const int rightAxis = g.Node("Input.GetAxis", C * 2, tickY + 260, { { "Positive", "D" }, { "Negative", "A" } });
        g.Link(tick, "Then", tickSequence, "In");
        g.Link(tickSequence, "Then 0", move, "In");
        g.Link(forwardAxis, "Value", move, "Forward");
        g.Link(rightAxis, "Value", move, "Right");

        const double lookY = tickY + 420;
        const int look = g.Node("Character.AddLookInput", C * 3, lookY);
        const int lookInput = g.Node("Input.GetLookInput", C * 2, lookY + 80);
        g.Link(tickSequence, "Then 1", look, "In");
        g.Link(lookInput, "Turn", look, "Turn");
        g.Link(lookInput, "Look Up", look, "Look Up");

        const double crouchY = tickY + 640;
        const int crouchMode = g.Node("Flow.Branch", C * 2, crouchY);
        const int crouchToggleSetting = g.Node("Character.GetCameraProperty", C, crouchY + 140,
                                               { { "Property", "Crouch Toggle" } });
        const int crouchHeld = g.Node("Flow.Branch", C * 3, crouchY + 40);
        const int crouchKeys = g.Node("Logic.Or", C * 2, crouchY + 200);
        const int ctrlDown = g.Node("Input.IsKeyDown", C, crouchY + 260, { { "Key", "Ctrl" } });
        const int cDown = g.Node("Input.IsKeyDown", C, crouchY + 360, { { "Key", "C" } });
        const int crouch = g.Node("Character.Crouch", C * 4, crouchY);
        const int uncrouch = g.Node("Character.UnCrouch", C * 4, crouchY + 120);
        g.Link(tickSequence, "Then 2", crouchMode, "In");
        g.Link(crouchToggleSetting, "Value", crouchMode, "Condition");
        g.Link(crouchMode, "False", crouchHeld, "In");
        g.Link(ctrlDown, "Down", crouchKeys, "A");
        g.Link(cDown, "Down", crouchKeys, "B");
        g.Link(crouchKeys, "Result", crouchHeld, "Condition");
        g.Link(crouchHeld, "True", crouch, "In");
        g.Link(crouchHeld, "False", uncrouch, "In");

        const double sprintY = tickY + 1100;
        const int sprintMode = g.Node("Flow.Branch", C * 2, sprintY);
        const int sprintToggleSetting = g.Node("Character.GetCameraProperty", C, sprintY + 140,
                                               { { "Property", "Sprint Toggle" } });
        const int sprintHeld = g.Node("Character.SetSprinting", C * 3, sprintY + 200);
        const int shiftDown = g.Node("Input.IsKeyDown", C * 2, sprintY + 260, { { "Key", "Shift" } });
        // A toggled sprint ends as soon as the player stops going forward.
        const int stillForward = g.Node("Logic.Compare", C * 3, sprintY - 40, { { "Operator", ">" }, { "B", "0" } });
        const int keepSprint = g.Node("Flow.Branch", C * 4, sprintY - 120);
        const int sprintToggled = g.Node("Character.SetSprinting", C * 5, sprintY - 160);
        const int readToggled = g.Node("Variable.Get", C * 4, sprintY + 40, { { "Variable", "SprintToggled" } });
        const int clearToggled = g.Node("Variable.Set", C * 5, sprintY - 20,
                                        { { "Variable", "SprintToggled" }, { "Value", "false" } });
        const int stopSprint = g.Node("Character.SetSprinting", C * 5, sprintY + 120, { { "Sprinting", "false" } });
        g.Link(tickSequence, "Then 3", sprintMode, "In");
        g.Link(sprintToggleSetting, "Value", sprintMode, "Condition");
        g.Link(sprintMode, "False", sprintHeld, "In");
        g.Link(shiftDown, "Down", sprintHeld, "Sprinting");
        g.Link(sprintMode, "True", keepSprint, "In");
        g.Link(forwardAxis, "Value", stillForward, "A");
        g.Link(stillForward, "Result", keepSprint, "Condition");
        g.Link(keepSprint, "True", sprintToggled, "In");
        g.Link(readToggled, "Value", sprintToggled, "Sprinting");
        g.Link(keepSprint, "False", clearToggled, "In");
        g.Link(clearToggled, "Then", stopSprint, "In");

        // ---- Jump ---------------------------------------------------------------------
        constexpr double jumpY = 2100;
        g.Comment("Jump: press to jump, release early for a shorter hop", -40, jumpY - 80, C * 2 + 40, 300, 0x1c4a2cu);
        const int spacePressed = g.Node("Event.KeyPressed", 0, jumpY, { { "Key", "Space" } });
        const int jump = g.Node("Character.Jump", C, jumpY);
        const int spaceReleased = g.Node("Event.KeyReleased", 0, jumpY + 120, { { "Key", "Space" } });
        const int stopJumping = g.Node("Character.StopJumping", C, jumpY + 120);
        g.Link(spacePressed, "Then", jump, "In");
        g.Link(spaceReleased, "Then", stopJumping, "In");

        // ---- Toggles --------------------------------------------------------------------
        constexpr double toggleY = 2500;
        g.Comment("Crouch and sprint toggles: only when Set Control Settings makes them toggles", -40, toggleY - 80,
                  C * 4 + 40, 660, 0x402a5au);
        const int ctrlPressed = g.Node("Event.KeyPressed", 0, toggleY, { { "Key", "Ctrl" } });
        const int cPressed = g.Node("Event.KeyPressed", 0, toggleY + 120, { { "Key", "C" } });
        const int crouchToggleOn = g.Node("Flow.Branch", C, toggleY);
        const int crouchToggleSetting2 = g.Node("Character.GetCameraProperty", 0, toggleY + 240,
                                                { { "Property", "Crouch Toggle" } });
        const int crouchFlipFlop = g.Node("Flow.FlipFlop", C * 2, toggleY);
        const int toggleCrouch = g.Node("Character.Crouch", C * 3, toggleY - 20);
        const int toggleStand = g.Node("Character.UnCrouch", C * 3, toggleY + 100);
        g.Link(ctrlPressed, "Then", crouchToggleOn, "In");
        g.Link(cPressed, "Then", crouchToggleOn, "In");
        g.Link(crouchToggleSetting2, "Value", crouchToggleOn, "Condition");
        g.Link(crouchToggleOn, "True", crouchFlipFlop, "In");
        g.Link(crouchFlipFlop, "A", toggleCrouch, "In");
        g.Link(crouchFlipFlop, "B", toggleStand, "In");

        const int shiftPressed = g.Node("Event.KeyPressed", 0, toggleY + 380, { { "Key", "Shift" } });
        const int sprintToggleOn = g.Node("Flow.Branch", C, toggleY + 380);
        const int sprintToggleSetting2 = g.Node("Character.GetCameraProperty", 0, toggleY + 500,
                                                { { "Property", "Sprint Toggle" } });
        const int flipSprint = g.Node("Variable.Set", C * 3, toggleY + 380, { { "Variable", "SprintToggled" } });
        const int notToggled = g.Node("Logic.Not", C * 2, toggleY + 480);
        const int readToggled2 = g.Node("Variable.Get", C, toggleY + 520, { { "Variable", "SprintToggled" } });
        g.Link(shiftPressed, "Then", sprintToggleOn, "In");
        g.Link(sprintToggleSetting2, "Value", sprintToggleOn, "Condition");
        g.Link(sprintToggleOn, "True", flipSprint, "In");
        g.Link(readToggled2, "Value", notToggled, "In");
        g.Link(notToggled, "Result", flipSprint, "Value");

        // ---- Camera properties: hold right mouse to zoom ----------------------------------
        constexpr double zoomY = 3300;
        g.Comment("Camera properties at runtime: hold the right mouse button to zoom to half the field of view",
                  -40, zoomY - 80, C * 3 + 40, 420, 0x2e2a52u);
        const int zoomPressed = g.Node("Event.KeyPressed", 0, zoomY, { { "Key", "Right Mouse" } });
        const int zoomIn = g.Node("Character.SetCameraProperty", C * 2, zoomY, { { "Property", "Field of View" } });
        const int halfFov = g.Node("Math.Multiply", C, zoomY + 100, { { "B", "0.5" } });
        const int readFov = g.Node("Variable.Get", 0, zoomY + 120, { { "Variable", "DefaultFieldOfView" } });
        const int zoomReleased = g.Node("Event.KeyReleased", 0, zoomY + 220, { { "Key", "Right Mouse" } });
        const int zoomOut = g.Node("Character.SetCameraProperty", C * 2, zoomY + 220, { { "Property", "Field of View" } });
        g.Link(zoomPressed, "Then", zoomIn, "In");
        g.Link(readFov, "Value", halfFov, "A");
        g.Link(halfFov, "Result", zoomIn, "Value");
        g.Link(zoomReleased, "Then", zoomOut, "In");
        g.Link(readFov, "Value", zoomOut, "Value");

        return g.Take();
    }
}
