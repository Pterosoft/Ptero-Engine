# UI Editor

`Windows → UI Editor` opens a viewer for the engine's RmlUi documents: load one from `Data/UI`, see it drawn at a real size, click it, and watch what the click actually did.

That last part is why the panel exists. A document that renders correctly and responds to nothing is the normal failure, and the screen looks identical whether the button has no handler or the click never reached it. The event trace tells those apart by looking.

## The preview

The preview is the **same** RmlUi target the game composites over the viewport, not a second one built for the editor. It is shown through `QtUi::Image`, which creates a native child surface that `QtViewportRenderer` blits the GPU texture into — the Qt draw list can only paint file-backed pixmaps, never a live GPU texture.

Opening the panel makes the RmlUi pass run outside a play session. It does **not** make the document appear over the viewport: compositing stays tied to `IsGameUiActive()`, which needs a running game. The two are separate flags (`SetVisible` for the game, `SetPreviewActive` for the panel) precisely so opening a viewer cannot change what the scene looks like.

Preview sizes are a fixed list rather than "fill the panel". Sizing a QtUi widget from `GetContentRegionAvail()` feeds the window's own minimum size back into its content, and the panel grows a little wider every frame — the bug the G-Buffer debug window had.

Because the target is sized to the render resolution and shown at the preview size, pointer coordinates are scaled into UI pixels before RmlUi sees them. Hit testing is therefore the real thing at every preview size, not an approximation that happens to line up at one of them.

**Interactive** hands the pointer to the document. The panel only writes mouse state while the pointer is actually over the preview, plus the one frame it leaves — the viewport's own overlay writes the same state, so an unconditional call would clear the game UI's focus every frame while a play session was running with this panel open.

## Authoring loop

Documents and their `.rcss` are read from disk on load, so:

1. Edit `Data/UI/hud.rml` or `hud.rcss` in any editor.
2. **Reload** in the panel.
3. Click.

The document list recursively scans `Data/UI` and all its subfolders every frame the panel is open, so a newly written document appears without restarting. Entries show their full path relative to `Data/UI`, such as `Farkle/farkle.rml` or `Menus/Settings/audio.rml`; identical filenames in different folders remain separate. `.rml` extensions are matched without regard to case, and inaccessible folders are skipped.

Select an entry and press **Load**. Your selection stays in place until you load it, even while another document is open or the list refreshes. **Reload** uses the same relative path. Game code and the node graph's UI load action accept these same paths. Linked stylesheets and textures resolve relative to the document or stylesheet that references them.

## Inspector

**Inspector** toggles RmlUi's own debugger, hosted in the same context as the document: the DOM tree, computed styles, and the box model of whatever you hover. This is the tool for "why is this element the wrong size", and it is the upstream one, not a reimplementation.

## Event trace

**Trace events** attaches a listener to the document in the **capture phase**, so it sees an event on its way *down* to the target whether or not anything further along handles it. It records `click`, `dblclick`, `mousedown`, `mouseup`, `mouseover`, `mouseout`, `focus`, `blur`, `change`, `submit` and `keydown` — deliberately not `mousemove`, which would bury every click under a thousand lines and answer no question anyone asks.

Read it this way:

| What you see | What it means |
|---|---|
| `click  button#start` | The click reached the element. If nothing happened, the handler is missing or broken. |
| `mouseover` but no `click` | The press and release landed on different elements, or something is swallowing the press. |
| nothing at all | The click never reached the document: pointer mapping, hit testing, or the element is covered. |

The trace survives a reload, because reloading and clicking again is the whole workflow. It keeps the most recent 400 events and shows the newest 20 first — the event you just caused is the one you want to read.

Tracing is off by default and costs a listener call per event; a shipping game has no use for it.

## Element

The **Element** section drives the same four calls game code and the node graph's UI nodes make — `SetElementText`, `SetElementProperty`, `SetElementClass`, `SetElementVisible` — addressed by RML `id`. Poking an element here therefore exercises the path those use, not a parallel one, and each call reports success or "found no such element" to the Console under the `UI` category.

## Where it lives

- `Source/Renderer_DX12/EditorUiViewer.cpp` — the panel.
- `Source/Renderer_DX12/RmlUiRenderer.{h,cpp}` — library lifetime, the context, documents, the off-screen target, input, and the event trace.
- `Data/UI` — documents and stylesheets.

See [Console.md](Console.md) for the log the panel reports into.
