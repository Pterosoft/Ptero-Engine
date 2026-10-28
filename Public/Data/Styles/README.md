# Editor styles

Each `.style` file in this folder is one editor look. You pick one in
**Edit > Editor Settings...**, and the editor remembers it for the next session.

A style is a JSON document. Any key you leave out keeps its value from the
built-in default ("Ptero Dark"), so a style can be as short as one accent color.
`Ptero Dark.style` lists every key and is the best file to start from.

## Making a new style

1. Open **Edit > Editor Settings...**, type a name and press **Duplicate**. This
   copies the current style to `<name>.style` in this folder and switches to it.
2. Edit that file in any text editor. The editor watches it and reapplies it
   every time you save, so you see changes straight away.
3. If the file has a syntax error, Editor Settings shows the error and the
   previous look stays in place.

## Keys

| Key | Meaning |
| --- | --- |
| `name`, `description` | Shown in Editor Settings. |
| `dark` | `true` for a dark style. This also sets the window title bar and Qt's color scheme. |
| `font.family`, `font.pointSize` | UI font. An empty family or a size of `0` uses the Windows UI font. |
| `icons.folder` | Icon folder, relative to `Data/` (default `Icons/Editor`). |
| `icons.color`, `active`, `checked`, `selected`, `disabled` | Icon tints for normal, hovered or focused, checked, on a selection, and disabled. |
| `colors.*` | See the list below. |
| `metrics.*` | Sizes in pixels: `radius`, `radiusSmall`, `iconSize`, `toolButtonSize` (whole toolbar button), `toolIconSize`, `listButtonHeight` (text row of a Components entry, without its padding), `indicatorSize`, `scrollbarWidth`, `splitterWidth`. |
| `stylesheet` | Extra Qt stylesheet (QSS), as one string or as an array of lines. It is applied after the built-in sheet. |

Colors can be written as `#rrggbb`, `#rrggbbaa` (alpha last, as in CSS),
`rgb(r, g, b)`, `rgba(r, g, b, a)` with `a` from 0 to 1, or a color name. A value
that is not a color, such as a `qlineargradient(...)`, is passed to the
stylesheet unchanged.

Color keys: `window`, `panel`, `header`, `base`, `alternateBase`, `button`,
`buttonHover`, `buttonPressed`, `border`, `borderStrong`, `separator`, `text`,
`textDim`, `textDisabled`, `accent`, `accentHover`, `accentPressed`,
`accentSoft`, `accentText`, `checkedText`, `menu`, `menuBorder`, `tooltip`,
`scrollbar`, `scrollbarHover`.

## Tokens in `stylesheet`

Inside `stylesheet`, `@name` is replaced by the value of any color or metric key.
Metrics come out as pixel lengths. You can also add color keys of your own and
use them the same way. For example:

```json
"colors": { "dockTitle": "#2a1a10" },
"stylesheet": [
    "QDockWidget::title { background: @dockTitle; border-bottom: 2px solid @accent; }"
]
```

## Icons

Icons are SVG files, drawn in the style's icon colors. The ones in
`Data/Icons/Editor` come from [Lucide](https://lucide.dev) (ISC license, see
`LICENSE-lucide.txt` there). To change one, drop an SVG or PNG with the same
name into the icon folder. A PNG is drawn as it is, without the style's tint.
For a whole new icon set, point `icons.folder` at another folder.
The SVG reader handles `path`, `line`, `rect`, `circle`, `ellipse`, `polyline`
and `polygon`, which covers stroke icon sets such as Lucide, Tabler and Feather.
