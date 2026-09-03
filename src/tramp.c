#include "trace.h"

#include <string.h>

#if defined(__i386__)

static void *g_orig_dev[160];
static void *g_orig_d3d[32];
static void trace_dev_slot(int slot);
static void trace_d3d_slot(int slot);
static void (*g_call_dev)(int) = trace_dev_slot;
static void (*g_call_d3d)(int) = trace_d3d_slot;

static const char *k_dev_names[] = {
	"QueryInterface",
	"AddRef",
	"Release",
	"TestCooperativeLevel",
	"GetAvailableTextureMem",
	"EvictManagedResources",
	"GetDirect3D",
	"GetDeviceCaps",
	"GetDisplayMode",
	"GetCreationParameters",
	"SetCursorProperties",
	"SetCursorPosition",
	"ShowCursor",
	"CreateAdditionalSwapChain",
	"GetSwapChain",
	"GetNumberOfSwapChains",
	"Reset",
	"Present",
	"GetBackBuffer",
	"GetRasterStatus",
	"SetDialogBoxMode",
	"SetGammaRamp",
	"GetGammaRamp",
	"CreateTexture",
	"CreateVolumeTexture",
	"CreateCubeTexture",
	"CreateVertexBuffer",
	"CreateIndexBuffer",
	"CreateRenderTarget",
	"CreateDepthStencilSurface",
	"UpdateSurface",
	"UpdateTexture",
	"GetRenderTargetData",
	"GetFrontBufferData",
	"StretchRect",
	"ColorFill",
	"CreateOffscreenPlainSurface",
	"SetRenderTarget",
	"GetRenderTarget",
	"SetDepthStencilSurface",
	"GetDepthStencilSurface",
	"BeginScene",
	"EndScene",
	"Clear",
	"SetTransform",
	"GetTransform",
	"MultiplyTransform",
	"SetViewport",
	"GetViewport",
	"SetMaterial",
	"GetMaterial",
	"SetLight",
	"GetLight",
	"LightEnable",
	"GetLightEnable",
	"SetClipPlane",
	"GetClipPlane",
	"SetRenderState",
	"GetRenderState",
	"CreateStateBlock",
	"BeginStateBlock",
	"EndStateBlock",
	"SetClipStatus",
	"GetClipStatus",
	"GetTexture",
	"SetTexture",
	"GetTextureStageState",
	"SetTextureStageState",
	"GetSamplerState",
	"SetSamplerState",
	"ValidateDevice",
	"SetPaletteEntries",
	"GetPaletteEntries",
	"SetCurrentTexturePalette",
	"GetCurrentTexturePalette",
	"SetScissorRect",
	"GetScissorRect",
	"SetSoftwareVertexProcessing",
	"GetSoftwareVertexProcessing",
	"SetNPatchMode",
	"GetNPatchMode",
	"DrawPrimitive",
	"DrawIndexedPrimitive",
	"DrawPrimitiveUP",
	"DrawIndexedPrimitiveUP",
	"ProcessVertices",
	"CreateVertexDeclaration",
	"SetVertexDeclaration",
	"GetVertexDeclaration",
	"SetFVF",
	"GetFVF",
	"CreateVertexShader",
	"SetVertexShader",
	"GetVertexShader",
	"SetVertexShaderConstantF",
	"GetVertexShaderConstantF",
	"SetVertexShaderConstantI",
	"GetVertexShaderConstantI",
	"SetVertexShaderConstantB",
	"GetVertexShaderConstantB",
	"SetStreamSource",
	"GetStreamSource",
	"SetStreamSourceFreq",
	"GetStreamSourceFreq",
	"SetIndices",
	"GetIndices",
	"CreatePixelShader",
	"SetPixelShader",
	"GetPixelShader",
	"SetPixelShaderConstantF",
	"GetPixelShaderConstantF",
	"SetPixelShaderConstantI",
	"GetPixelShaderConstantI",
	"SetPixelShaderConstantB",
	"GetPixelShaderConstantB",
	"DrawRectPatch",
	"DrawTriPatch",
	"DeletePatch",
	"CreateQuery",
};

static const char *k_d3d_names[] = {
	"QueryInterface",
	"AddRef",
	"Release",
	"RegisterSoftwareDevice",
	"GetAdapterCount",
	"GetAdapterIdentifier",
	"GetAdapterModeCount",
	"EnumAdapterModes",
	"GetAdapterDisplayMode",
	"CheckDeviceType",
	"CheckDeviceFormat",
	"CheckDeviceMultiSampleType",
	"CheckDepthStencilMatch",
	"CheckDeviceFormatConversion",
	"GetDeviceCaps",
	"GetAdapterMonitor",
	"CreateDevice",
};

static void trace_named(const char *iface, int slot, const char **names, int nnames)
{
	const char *n = "?";
	if (slot >= 0 && slot < nnames && names[slot])
		n = names[slot];
	if (strcmp(n, "AddRef") == 0 || strcmp(n, "Release") == 0)
		return;
	if (slot >= nnames)
		sw_trace("%s.slot%d", iface, slot);
	else
		sw_trace("%s.%s", iface, n);
}

static void trace_dev_slot(int slot)
{
	trace_named("Dev", slot, k_dev_names, (int)(sizeof(k_dev_names) / sizeof(k_dev_names[0])));
}

static void trace_d3d_slot(int slot)
{
	trace_named("D3D9", slot, k_d3d_names, (int)(sizeof(k_d3d_names) / sizeof(k_d3d_names[0])));
}

#define TRAMP(tag, n, orig, callp)                                                                    \
	__attribute__((naked)) static void tramp_##tag##_##n(void)                                    \
	{                                                                                             \
		__asm__ __volatile__("pushal\n\t"                                                     \
				     "pushl $" #n "\n\t"                                              \
				     "call *%0\n\t"                                                   \
				     "addl $4, %%esp\n\t"                                             \
				     "popal\n\t"                                                      \
				     "jmpl *%1\n\t"                                                   \
				     :                                                                \
				     : "m"(callp), "m"(orig[n])                                       \
				     : "memory");                                                     \
	}

#define T8(tag, a, b, c, d, e, f, g, h, orig, callp)                                                  \
	TRAMP(tag, a, orig, callp)                                                                    \
	TRAMP(tag, b, orig, callp)                                                                    \
	TRAMP(tag, c, orig, callp)                                                                    \
	TRAMP(tag, d, orig, callp)                                                                    \
	TRAMP(tag, e, orig, callp)                                                                    \
	TRAMP(tag, f, orig, callp)                                                                    \
	TRAMP(tag, g, orig, callp)                                                                    \
	TRAMP(tag, h, orig, callp)

T8(dev, 0, 1, 2, 3, 4, 5, 6, 7, g_orig_dev, g_call_dev)
T8(dev, 8, 9, 10, 11, 12, 13, 14, 15, g_orig_dev, g_call_dev)
T8(dev, 16, 17, 18, 19, 20, 21, 22, 23, g_orig_dev, g_call_dev)
T8(dev, 24, 25, 26, 27, 28, 29, 30, 31, g_orig_dev, g_call_dev)
T8(dev, 32, 33, 34, 35, 36, 37, 38, 39, g_orig_dev, g_call_dev)
T8(dev, 40, 41, 42, 43, 44, 45, 46, 47, g_orig_dev, g_call_dev)
T8(dev, 48, 49, 50, 51, 52, 53, 54, 55, g_orig_dev, g_call_dev)
T8(dev, 56, 57, 58, 59, 60, 61, 62, 63, g_orig_dev, g_call_dev)
T8(dev, 64, 65, 66, 67, 68, 69, 70, 71, g_orig_dev, g_call_dev)
T8(dev, 72, 73, 74, 75, 76, 77, 78, 79, g_orig_dev, g_call_dev)
T8(dev, 80, 81, 82, 83, 84, 85, 86, 87, g_orig_dev, g_call_dev)
T8(dev, 88, 89, 90, 91, 92, 93, 94, 95, g_orig_dev, g_call_dev)
T8(dev, 96, 97, 98, 99, 100, 101, 102, 103, g_orig_dev, g_call_dev)
T8(dev, 104, 105, 106, 107, 108, 109, 110, 111, g_orig_dev, g_call_dev)
T8(dev, 112, 113, 114, 115, 116, 117, 118, 119, g_orig_dev, g_call_dev)
T8(dev, 120, 121, 122, 123, 124, 125, 126, 127, g_orig_dev, g_call_dev)
T8(d3d, 0, 1, 2, 3, 4, 5, 6, 7, g_orig_d3d, g_call_d3d)
T8(d3d, 8, 9, 10, 11, 12, 13, 14, 15, g_orig_d3d, g_call_d3d)
T8(d3d, 16, 17, 18, 19, 20, 21, 22, 23, g_orig_d3d, g_call_d3d)

/* clang-format off */
static void (*k_tramp_dev[])(void) = {
	tramp_dev_0, tramp_dev_1, tramp_dev_2, tramp_dev_3, tramp_dev_4, tramp_dev_5, tramp_dev_6, tramp_dev_7,
	tramp_dev_8, tramp_dev_9, tramp_dev_10, tramp_dev_11, tramp_dev_12, tramp_dev_13, tramp_dev_14, tramp_dev_15,
	tramp_dev_16, tramp_dev_17, tramp_dev_18, tramp_dev_19, tramp_dev_20, tramp_dev_21, tramp_dev_22, tramp_dev_23,
	tramp_dev_24, tramp_dev_25, tramp_dev_26, tramp_dev_27, tramp_dev_28, tramp_dev_29, tramp_dev_30, tramp_dev_31,
	tramp_dev_32, tramp_dev_33, tramp_dev_34, tramp_dev_35, tramp_dev_36, tramp_dev_37, tramp_dev_38, tramp_dev_39,
	tramp_dev_40, tramp_dev_41, tramp_dev_42, tramp_dev_43, tramp_dev_44, tramp_dev_45, tramp_dev_46, tramp_dev_47,
	tramp_dev_48, tramp_dev_49, tramp_dev_50, tramp_dev_51, tramp_dev_52, tramp_dev_53, tramp_dev_54, tramp_dev_55,
	tramp_dev_56, tramp_dev_57, tramp_dev_58, tramp_dev_59, tramp_dev_60, tramp_dev_61, tramp_dev_62, tramp_dev_63,
	tramp_dev_64, tramp_dev_65, tramp_dev_66, tramp_dev_67, tramp_dev_68, tramp_dev_69, tramp_dev_70, tramp_dev_71,
	tramp_dev_72, tramp_dev_73, tramp_dev_74, tramp_dev_75, tramp_dev_76, tramp_dev_77, tramp_dev_78, tramp_dev_79,
	tramp_dev_80, tramp_dev_81, tramp_dev_82, tramp_dev_83, tramp_dev_84, tramp_dev_85, tramp_dev_86, tramp_dev_87,
	tramp_dev_88, tramp_dev_89, tramp_dev_90, tramp_dev_91, tramp_dev_92, tramp_dev_93, tramp_dev_94, tramp_dev_95,
	tramp_dev_96, tramp_dev_97, tramp_dev_98, tramp_dev_99, tramp_dev_100, tramp_dev_101, tramp_dev_102, tramp_dev_103,
	tramp_dev_104, tramp_dev_105, tramp_dev_106, tramp_dev_107, tramp_dev_108, tramp_dev_109, tramp_dev_110, tramp_dev_111,
	tramp_dev_112, tramp_dev_113, tramp_dev_114, tramp_dev_115, tramp_dev_116, tramp_dev_117, tramp_dev_118, tramp_dev_119,
	tramp_dev_120, tramp_dev_121, tramp_dev_122, tramp_dev_123, tramp_dev_124, tramp_dev_125, tramp_dev_126, tramp_dev_127,
};

static void (*k_tramp_d3d[])(void) = {
	tramp_d3d_0, tramp_d3d_1, tramp_d3d_2, tramp_d3d_3, tramp_d3d_4, tramp_d3d_5, tramp_d3d_6, tramp_d3d_7,
	tramp_d3d_8, tramp_d3d_9, tramp_d3d_10, tramp_d3d_11, tramp_d3d_12, tramp_d3d_13, tramp_d3d_14, tramp_d3d_15,
	tramp_d3d_16, tramp_d3d_17, tramp_d3d_18, tramp_d3d_19, tramp_d3d_20, tramp_d3d_21, tramp_d3d_22, tramp_d3d_23,
};
/* clang-format on */

static void wrap_slots(void **slots, size_t n, void **orig, void (**tramps)(void), size_t ntramps,
		       int skip_iunknown)
{
	size_t i;
	if (!slots)
		return;
	for (i = 0; i < n && i < ntramps; i++) {
		orig[i] = slots[i];
		if (!orig[i])
			continue;
		if (skip_iunknown && (i == 1 || i == 2))
			continue;
		slots[i] = (void *)tramps[i];
	}
}

void d3d9_trace_wrap_dev(void **slots, size_t bytes)
{
	g_call_dev = trace_dev_slot;
	wrap_slots(slots, bytes / sizeof(void *), g_orig_dev, k_tramp_dev,
		   sizeof(k_tramp_dev) / sizeof(k_tramp_dev[0]), 1);
}

void d3d9_trace_wrap_d3d(void **slots, size_t bytes)
{
	g_call_d3d = trace_d3d_slot;
	wrap_slots(slots, bytes / sizeof(void *), g_orig_d3d, k_tramp_d3d,
		   sizeof(k_tramp_d3d) / sizeof(k_tramp_d3d[0]), 1);
}

#else

void d3d9_trace_wrap_dev(void **slots, size_t bytes)
{
	(void)slots;
	(void)bytes;
}

void d3d9_trace_wrap_d3d(void **slots, size_t bytes)
{
	(void)slots;
	(void)bytes;
}

#endif
