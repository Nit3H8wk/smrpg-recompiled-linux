API headers of AMD's FSR SDK 2.4 (FidelityFX SDK, AMD), with their MIT license in each file, in the SDK's own folder
structure (the headers include each other with relative paths).

runtime/nvn/vulkan/vk_fsr.cpp (FSR) and vk_fg.cpp (frame generation) use them. At run time the SDK's signed DLLs are
needed next to smrpg-recomp.exe (they have AMD's own redistribution license, which is why they are not in the repository):
  amd_fidelityfx_loader_vk.dll
  amd_fidelityfx_upscaler_vk.dll
  amd_fidelityfx_framegeneration_vk.dll   (frame generation; without it, only FSR)
Without them the menu's "AMD FSR" option says it is not available and the game draws as usual.
