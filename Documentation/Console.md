# Console, logging and cvars

The Console panel docks on the left, tabbed beside Components (`Windows → Console`). It shows everything the engine logs and takes commands against every renderer setting.

Three things share one design: the log is the single stream, the Console is a view onto it, and a cvar is a name bound to the live variable. Nothing is mirrored, so nothing can drift.

## The log

`PteroLog` (`Source/System/include/System/PteroLog.h`) is the only sink. It lives in System rather than the renderer because it is not a renderer facility - asset import, the node graph and the editor shell all have things to say. Every line goes three places at once: an in-memory ring the Console renders, a session `.txt` under `Logs/`, and `OutputDebugStringA` for an attached debugger.

The file is the point. A renderer fault that takes ten minutes of editing to provoke is worth nothing if the evidence dies with the process, so the sink is written through rather than buffered to the end of the session:

- anything at Warning or above flushes immediately,
- ordinary traffic flushes every 200 ms, and once per frame from the render loop,
- an unhandled exception writes what it can through `SetUnhandledExceptionFilter` and closes the file before the process goes down,
- `std::terminate`, `atexit` and the console control handler (logoff, shutdown, Ctrl+C) all close it cleanly.

That covers everything except Task Manager's **End process**, which is `TerminateProcess` — no code runs, ever, in any application. Write-through is the only defence against it, and it bounds the loss to the current frame.

Files are named `Ptero_<date>_<time>.txt`. The thirty most recent are kept and older ones are deleted at startup, so the directory does not become an archive. `logpath` prints the current one.

### Watchdog

A frame that never returns writes nothing, because the thread that would write the line is the thread that is stuck. So a second thread watches: the render loop stamps a heartbeat each frame, and if the stamp stops moving for five seconds the watchdog writes

```
[Watchdog] Render thread has not completed frame 4821 for 5.0 s.
```

and flushes. A hang therefore leaves a record of which frame it died on, and everything above that line is what the renderer did last. It reports again if the stall continues, and once more if the frame eventually completes.

### Levels and categories

`Trace`, `Debug`, `Info`, `Warning`, `Error`, `Fatal`. The minimum level defaults to `Debug`; `log trace` lowers it, `log warning` quietens everything below. Categories (`Renderer`, `Meshes`, `Editor`, `Console`, `CVar`, `Watchdog`, `Crash`, `Log`) are a short tag per line, never formatted into the message, so the Console's category filter stays exact.

## CVars

The registry is `Source/System/include/System/CVar.h`; what it is bound *to* is renderer-specific and stays there, in `EngineCVars.cpp`.

A cvar holds a pointer into the settings struct the renderer already reads every frame. Setting one from the Console is the same write as moving the slider in a panel, so there is no synchronisation step to get out of date and the two can never disagree. Registration happens once, against the long-lived scene renderer; nothing is ever unregistered.

Names are dotted and lower-case, matched case-insensitively.

```
rtgi.specular.enabled            show the value, its range and its startup default
rtgi.specular.enabled false      set it
list rtgi.specular               every cvar whose name or description contains that
help rtgi.radianceclamp          one cvar, described
reset rtgi.specular.enabled      back to the startup default
reset all                        all of them
```

Booleans accept `true`/`false`, `1`/`0`, `on`/`off`, `yes`/`no`. Enums accept either the label or the number. A value outside a cvar's range is **rejected, not clamped**, because silently clamping hides a typo.

### Type-ahead

As you type, matching cvars drop down beneath the field with their descriptions beside them, the way a game console does. Matching is by substring rather than prefix and ignores case, because a half-remembered name is the usual reason to want the list: typing `specular` finds `rtgi.specular.enabled` without recalling which group it belongs to. Arrow keys move through the list, Enter or Tab inserts the name, and only the name is ever inserted — the description column is a hint.

### Built-in commands

| Command | Meaning |
|---|---|
| `help` | list the commands |
| `help <cvar>` | describe one cvar |
| `list [text]`, `find <text>` | cvars whose name or description contains `text` |
| `reset <cvar>` / `reset all` | restore the startup default |
| `dumpcvars`, `cvarlist` | every cvar and its current value |
| `log <level>` | `trace`, `debug`, `info`, `warning`, `error`, `fatal` |
| `logpath` | where this session's `.txt` is being written |
| `clear` | empty the Console; the log file keeps everything |
| `echo <text>` | print text |

`clear` deliberately empties only the on-screen ring. The file is the record of what happened, and clearing a view must not edit history.

## CVar reference

Every cvar, with what it does, its type, its allowed values and its default, is listed in
[Manual/CVar-Reference.md](Manual/CVar-Reference.md). That page is generated from
`Source/Renderer_DX12/EngineCVars.cpp` and the settings headers by
`node Documentation/Tools/GenerateCVarReference.mjs`; run it after adding or renaming a cvar,
or after changing a description or a default.
