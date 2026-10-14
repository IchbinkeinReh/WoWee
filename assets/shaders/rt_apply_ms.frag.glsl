#version 460
// The ray traced lighting's apply pass for a multisampled scene, once per
// sample, from that sample's depth and G-buffer (rt_apply.glsli).

#define RT_MSAA
#include "rt_apply.glsli"
