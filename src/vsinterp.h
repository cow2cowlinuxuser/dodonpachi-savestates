#ifndef D3D9_SW_VSINTERP_H
#define D3D9_SW_VSINTERP_H

#include <d3d9.h>
#include "swrast.h"

typedef struct VsInVert {
	const unsigned char *stream0;
	const unsigned char *stream1;
} VsInVert;

int vs_exec(const DWORD *code, UINT code_bytes, const float constants[256][4], unsigned const_ver,
	    const D3DVERTEXELEMENT9 *decl, const unsigned char *s0, const unsigned char *s1,
	    const unsigned char *s2, SwVert *out, char *err, int err_n);

#endif
