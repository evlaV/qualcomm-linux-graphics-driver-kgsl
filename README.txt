.so files are from r00007.1 release (except libcutils.so.0, didn't bother finding sources for it)
libvulkan_adreno.so is patched: all instances of "gbm" replaced with "gbM" to avoid conflicts with upstream libgbm

run install.sh to install to default locations

add "ln -s /dev/dri/renderD128 /dev/kgsl-3d0" to run on boot to create /dev/kgsl-3d0

env vars:

to use adreno blob:
VK_ICD_FILENAMES=/usr/share/vulkan/qcom_adreno_icd.json

to use xcb/x11 WSI:
VK_INSTANCE_LAYERS=VK_LAYER_WSI_xcb

to use wayland WSI:
VK_INSTANCE_LAYERS=VK_LAYER_WSI_wayland

note WSI layer has UBWC disabled (linear images only), and that signficantly reduces performance in simple demos
