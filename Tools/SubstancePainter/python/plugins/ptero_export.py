"""
Ptero Engine export for Substance 3D Painter.

Adds a "Ptero Engine" menu with two actions:

  Add Ptero channels to active Texture Set
      Adds the channels the ptero-principled shader reads for the Disney lobes' masks:
      User0 "Clearcoat", User1 "Sheen", Anisotropy level, User2 "Flatten" and
      User3 "Aniso direction". Only add the ones you paint; a missing channel exports
      as white, which leaves the material slider whole.

  Export to Ptero Engine...
      Exports every Texture Set into a folder (pick one under the engine's Data/ folder)
      packed the way Data/Shaders/GBuffer.hlsl reads it, and writes one
      <TextureSet>.material next to the textures with the shader's slider values. The
      editor's Data folder watcher then imports the PNGs beside themselves as .dds, which
      is what the .material refers to.

Install: copy this file to
  Documents/Adobe/Adobe Substance 3D Painter/python/plugins/
and enable it from Python > ptero_export.
"""

import json
import os

from PySide6 import QtGui, QtWidgets

import substance_painter.export
import substance_painter.js
import substance_painter.logging
import substance_painter.project
import substance_painter.textureset
import substance_painter.ui

_SHADER_NAME = "ptero-principled"
_MENU = None

ChannelType = substance_painter.textureset.ChannelType
ChannelFormat = substance_painter.textureset.ChannelFormat

# Mask channels, in principledMask's RGBA order (GBuffer.hlsl, t8).
_MASK_CHANNELS = [
    ("R", ChannelType.User0, "user0"),             # clearcoat
    ("G", ChannelType.User1, "user1"),             # sheen
    ("B", ChannelType.Anisotropylevel, "anisotropylevel"),
    ("A", ChannelType.User2, "user2"),             # subsurface flatten
]

# Shader uniform -> .material key.
_SHADER_KEYS = {
    "ptero_specular": "specularFactor",
    "ptero_specularTint": "specularTint",
    "ptero_subsurfaceFlatten": "subsurfaceFlatten",
    "ptero_anisotropic": "anisotropic",
    "ptero_sheen": "sheen",
    "ptero_sheenTint": "sheenTint",
    "ptero_clearcoat": "clearcoat",
    "ptero_clearcoatGloss": "clearcoatGloss",
    "ptero_aoStrength": "ambientOcclusionStrength",
}


def _log(message):
    substance_painter.logging.info("[Ptero] " + message)


def _warn(message):
    substance_painter.logging.warning("[Ptero] " + message)


def _channel(dest, map_name, src="R", map_type="documentMap"):
    return {"destChannel": dest, "srcChannel": src, "srcMapType": map_type, "srcMapName": map_name}


def _rgb(map_name, map_type="documentMap"):
    return [_channel(c, map_name, c, map_type) for c in "RGB"]


def _map(file_name, channels, file_format="png", bit_depth="8"):
    return {
        "fileName": file_name,
        "channels": channels,
        "parameters": {"fileFormat": file_format, "bitDepth": bit_depth, "dithering": False},
    }


def _build_preset(stack):
    """The maps one Texture Set exports, and the .material texture key of each."""
    has = stack.has_channel
    maps = []  # (material key, preset map)

    maps.append(("baseColor", _map("$textureSet_BaseColor", _rgb("basecolor"))))
    # Painter folds the height channel into this map, as the shader's preview does.
    maps.append(("normal", _map("$textureSet_Normal", _rgb("Normal_DirectX", "virtualMap"))))
    # ORM: R = AO, G = roughness, B = metallic (packed layout 2, Unreal's).
    maps.append(("orm", _map("$textureSet_ORM", [
        _channel("R", "AO_Mixed", "L", "virtualMap"),
        _channel("G", "roughness", "L") if has(ChannelType.Roughness) else _channel("G", "white", "L", "defaultMap"),
        _channel("B", "metallic", "L") if has(ChannelType.Metallic) else _channel("B", "black", "L", "defaultMap"),
    ])))
    if has(ChannelType.Emissive):
        maps.append(("emissive", _map("$textureSet_Emissive", _rgb("emissive"))))
    if has(ChannelType.Opacity):
        maps.append(("opacity", _map("$textureSet_Opacity", [_channel("L", "opacity", "L")])))
    if has(ChannelType.Height):
        maps.append(("height", _map("$textureSet_Height", [_channel("L", "height", "L")], bit_depth="16")))

    if any(has(channel_type) for _, channel_type, _ in _MASK_CHANNELS):
        mask = []
        for dest, channel_type, map_name in _MASK_CHANNELS:
            mask.append(_channel(dest, map_name, "L") if has(channel_type)
                        else _channel(dest, "white", "L", "defaultMap"))
        # RGB without A is not a valid map, so write A even when it is white.
        maps.append(("principledMask", _map("$textureSet_PrincipledMask", mask)))

    if has(ChannelType.User3):
        maps.append(("anisotropyDirection", _map("$textureSet_AnisoDirection", _rgb("user3"))))

    return maps


_JS_SHADER_STATE = """
(function() {
  var result = { instances: [], mapping: null };
  alg.shaders.instances().forEach(function(instance) {
    var values = {};
    var parameters = alg.shaders.parameters(instance.id);
    for (var key in parameters) {
      try { values[key] = parameters[key].value; } catch (e) {}
    }
    result.instances.push({ id: instance.id, label: instance.label, shader: instance.shader, values: values });
  });
  try { result.mapping = alg.shaders.shaderInstancesToObject(); } catch (e) {}
  return result;
})()
"""


def _find_instance_label(mapping, texture_set_name):
    """Best effort: the shader instance label shaderInstancesToObject assigns to a Texture Set."""
    if not isinstance(mapping, dict):
        return None
    for key in ("texturesets", "textureSets", "texture_sets"):
        sets = mapping.get(key)
        if isinstance(sets, dict) and texture_set_name in sets:
            entry = sets[texture_set_name]
            if isinstance(entry, dict):
                return entry.get("shader") or entry.get("shaderInstance") or entry.get("label")
            return entry
        if isinstance(sets, list):
            for entry in sets:
                if isinstance(entry, dict) and entry.get("name") == texture_set_name:
                    return entry.get("shader") or entry.get("shaderInstance") or entry.get("label")
    return None


def _shader_values(state, texture_set_name):
    instances = [i for i in state.get("instances", []) if i.get("shader") == _SHADER_NAME]
    if not instances:
        return None
    wanted = _find_instance_label(state.get("mapping"), texture_set_name)
    for instance in instances:
        if wanted is not None and wanted in (instance.get("label"), instance.get("id"), str(instance.get("id"))):
            return instance["values"]
    if len(instances) > 1:
        _warn("Could not tell which %s instance '%s' uses; taking '%s'."
              % (_SHADER_NAME, texture_set_name, instances[0].get("label")))
    return instances[0]["values"]


def _data_relative(path):
    """Path relative to the nearest enclosing Data folder, or None."""
    path = os.path.abspath(path)
    parts = path.replace("\\", "/").split("/")
    for i in range(len(parts) - 1, -1, -1):
        if parts[i].lower() == "data":
            return "/".join(parts[i + 1:])
    return None


def _write_material(export_dir, texture_set_name, textures, values, has_emissive, has_height):
    relative_dir = _data_relative(export_dir)
    if relative_dir is None:
        _warn("%s is not inside a Data folder; texture paths in the .material are bare file names."
              % export_dir)

    def engine_path(file_path):
        name = os.path.splitext(os.path.basename(file_path))[0] + ".dds"
        return (relative_dir + "/" + name) if relative_dir else name

    material = {
        "name": texture_set_name,
        "baseColorTint": [1.0, 1.0, 1.0, 1.0],
        "metallicFactor": 1.0,
        "roughnessFactor": 1.0,
        "specularFactor": 0.5,
        "normalScale": 1.0,
        "normalFlipGreen": False,
        "ambientOcclusionStrength": 1.0,
        "emissiveColor": [0.0, 0.0, 0.0],
        "textures": {key: engine_path(path) for key, path in textures.items()},
    }
    if has_height:
        # Painter's height channel sits at 0.5 on the surface once exported.
        material["heightReference"] = 0.5
        material["displacementMidLevel"] = 0.5

    if values is not None:
        for uniform, key in _SHADER_KEYS.items():
            if uniform in values and values[uniform] is not None:
                material[key] = float(values[uniform])
        if has_emissive:
            intensity = float(values.get("emissive_intensity", 1.0) or 0.0)
            material["emissiveColor"] = [intensity, intensity, intensity]
        if values.get("alpha_test_enabled"):
            material["useAlphaCutout"] = True
            material["alphaCutoff"] = float(values.get("alpha_test_threshold", 0.5))
    else:
        _warn("'%s' does not use the %s shader; the .material gets default slider values."
              % (texture_set_name, _SHADER_NAME))

    material_path = os.path.join(export_dir, texture_set_name + ".material")
    with open(material_path, "w", encoding="utf-8") as file:
        json.dump(material, file, indent=4, sort_keys=True)
    _log("Wrote " + material_path)


def export_to_ptero():
    if not substance_painter.project.is_open():
        _warn("No project is open.")
        return

    export_dir = QtWidgets.QFileDialog.getExistingDirectory(
        substance_painter.ui.get_main_window(), "Export to Ptero Engine (pick a folder under Data/)")
    if not export_dir:
        return

    state = substance_painter.js.evaluate(_JS_SHADER_STATE)

    presets, export_list, plans = [], [], []
    for texture_set in substance_painter.textureset.all_texture_sets():
        if texture_set.is_layered_material():
            _warn("Skipping layered Texture Set '%s'." % texture_set.name())
            continue
        stack = texture_set.get_stack()
        maps = _build_preset(stack)
        preset_name = "Ptero_" + texture_set.name()
        presets.append({"name": preset_name, "maps": [m for _, m in maps]})
        export_list.append({"rootPath": texture_set.name(), "exportPreset": preset_name})
        plans.append((texture_set.name(), maps, stack))

    config = {
        "exportPath": export_dir,
        "exportShaderParams": False,
        "exportPresets": presets,
        "exportList": export_list,
        "exportParameters": [{"parameters": {"paddingAlgorithm": "infinite"}}],
    }
    result = substance_painter.export.export_project_textures(config)
    if result.status != substance_painter.export.ExportStatus.Success:
        _warn("Export failed: " + str(result.message))
        return

    for texture_set_name, maps, stack in plans:
        exported = []
        for stack_key, files in result.textures.items():
            if texture_set_name in (stack_key if isinstance(stack_key, tuple) else (stack_key,)):
                exported.extend(files)
        textures = {}
        for key, preset_map in maps:
            suffix = preset_map["fileName"].replace("$textureSet", texture_set_name)
            match = next((f for f in exported
                          if os.path.splitext(os.path.basename(f))[0] == suffix), None)
            textures[key] = match or os.path.join(export_dir, suffix + ".png")
        _write_material(export_dir, texture_set_name, textures, _shader_values(state, texture_set_name),
                        stack.has_channel(ChannelType.Emissive), stack.has_channel(ChannelType.Height))

    _log("Export finished: " + export_dir)


def add_ptero_channels():
    if not substance_painter.project.is_open():
        _warn("No project is open.")
        return
    stack = substance_painter.textureset.get_active_stack()
    wanted = [
        (ChannelType.User0, ChannelFormat.L8, "Clearcoat"),
        (ChannelType.User1, ChannelFormat.L8, "Sheen"),
        (ChannelType.Anisotropylevel, ChannelFormat.L8, None),
        (ChannelType.User2, ChannelFormat.L8, "Flatten"),
        (ChannelType.User3, ChannelFormat.RGB8, "Aniso direction"),
    ]
    for channel_type, channel_format, label in wanted:
        if stack.has_channel(channel_type):
            continue
        try:
            stack.add_channel(channel_type, channel_format, label)
            _log("Added channel %s" % (label or channel_type))
        except ValueError as error:
            _warn("Could not add %s: %s" % (label or channel_type, error))


def start_plugin():
    global _MENU  # pylint: disable=global-statement
    _MENU = QtWidgets.QMenu("Ptero Engine")
    add_channels = QtGui.QAction("Add Ptero channels to active Texture Set", _MENU)
    add_channels.triggered.connect(add_ptero_channels)
    export = QtGui.QAction("Export to Ptero Engine...", _MENU)
    export.triggered.connect(export_to_ptero)
    _MENU.addAction(add_channels)
    _MENU.addAction(export)
    substance_painter.ui.add_menu(_MENU)


def close_plugin():
    global _MENU  # pylint: disable=global-statement
    if _MENU is not None:
        substance_painter.ui.delete_ui_element(_MENU)
    _MENU = None
