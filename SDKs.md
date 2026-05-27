## SDKs
In order to build the source code successfully you'll need the following SDKs

 - DirectXTex
 - DXC
 - Fmod (2.03.12)
 - Hosek-Wilkie
 - ImGui
 - ImGuizmo
 - nlohmann json
 - NRD
 - XeGTAO

Once you have all of them place them in Ptero-Engine\Source\SDKs folder. The folder does not exists and has to be created.

## SDKs Files in the Projects
Some SDKs require direct code file injection into the projects. Simply import the following files into designated projects.

**Editor**

 - imgui.cpp
 - imgui_draw.cpp
 - imgui_tables.cpp
 - imgui_widgets.cpp

*SDKs: ImGui*

**Renderer_DX12**

 - ImGuizmo.cpp
 - ArHosekSkyModel.c
 - imgui.cpp
 - imgui_demo.cpp
 - imgui_draw.cpp
 - imgui_impl_dx12.cpp
 - imgui_impl_win32.cpp
 - imgui_tables.cpp
 - imgui_widgets.cpp
 - BC.cpp
 - BC4BC5.cpp
 - BC6HBC7.cpp
 - DDSTextureLoader12.cpp
 - DirectXTexCompress.cpp
 - DirectXTexConvert.cpp
 - DirectXTexDDS.cpp
 - DirectXTexFlipRotate.cpp
 - DirectXTexHDR.cpp
 - DirectXTexImage.cpp
 - DirectXTexMipmaps.cpp
 - DirectXTexMisc.cpp
 - DirectXTexPMAlpha.cpp
 - DirectXTexResize.cpp
 - DirectXTexTGA.cpp
 - DirectXTexUtil.cpp
 - DirectXTexWIC.cpp

*SDKs: ImGui, DirectXTex, Hosek-Wilkie*

**System:**
 - BC.cpp
 - BC4BC5.cpp
 - BC6HBC7.cpp
 - DDSTextureLoader12.cpp
 - DirectXTexCompress.cpp
 - DirectXTexConvert.cpp
 - DirectXTexDDS.cpp
 - DirectXTexFlipRotate.cpp
 - DirectXTexHDR.cpp
 - DirectXTexImage.cpp
 - DirectXTexMipmaps.cpp
 - DirectXTexMisc.cpp
 - DirectXTexPMAlpha.cpp
 - DirectXTexResize.cpp
 - DirectXTexTGA.cpp
 - DirectXTexUtil.cpp
 - DirectXTexWIC.cpp

*SDKs: DirectXTex*