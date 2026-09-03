#include "vsinterp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct VsReg {
	float v[4];
} VsReg;

static int reg_type(DWORD t)
{
	return (int)(((t & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) |
		     ((t & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2));
}

static int decl_type_size(BYTE type)
{
	switch (type) {
	case D3DDECLTYPE_FLOAT1:
	case D3DDECLTYPE_D3DCOLOR:
	case D3DDECLTYPE_UBYTE4:
	case D3DDECLTYPE_UBYTE4N:
		return 4;
	case D3DDECLTYPE_FLOAT2:
	case D3DDECLTYPE_SHORT2:
	case D3DDECLTYPE_USHORT2N:
	case D3DDECLTYPE_FLOAT16_2:
		return 8;
	case D3DDECLTYPE_FLOAT3:
		return 12;
	case D3DDECLTYPE_FLOAT4:
	case D3DDECLTYPE_SHORT4:
	case D3DDECLTYPE_SHORT4N:
	case D3DDECLTYPE_USHORT4N:
	case D3DDECLTYPE_FLOAT16_4:
		return 16;
	default:
		return 4;
	}
}

static void load_elem(float out[4], const unsigned char *base, const D3DVERTEXELEMENT9 *e)
{
	const unsigned char *p;
	out[0] = 0.0f;
	out[1] = 0.0f;
	out[2] = 0.0f;
	out[3] = 1.0f;
	if (!base)
		return;
	p = base + e->Offset;
	switch (e->Type) {
	case D3DDECLTYPE_FLOAT1:
		out[0] = *(const float *)p;
		break;
	case D3DDECLTYPE_FLOAT2:
		memcpy(out, p, 8);
		break;
	case D3DDECLTYPE_FLOAT3:
		memcpy(out, p, 12);
		break;
	case D3DDECLTYPE_FLOAT4:
		memcpy(out, p, 16);
		break;
	case D3DDECLTYPE_D3DCOLOR: {
		DWORD c;
		memcpy(&c, p, 4);
		out[0] = ((c >> 16) & 255) / 255.0f;
		out[1] = ((c >> 8) & 255) / 255.0f;
		out[2] = (c & 255) / 255.0f;
		out[3] = ((c >> 24) & 255) / 255.0f;
		break;
	}
	case D3DDECLTYPE_UBYTE4:
		out[0] = (float)p[0];
		out[1] = (float)p[1];
		out[2] = (float)p[2];
		out[3] = (float)p[3];
		break;
	case D3DDECLTYPE_UBYTE4N: {
		out[0] = p[0] / 255.0f;
		out[1] = p[1] / 255.0f;
		out[2] = p[2] / 255.0f;
		out[3] = p[3] / 255.0f;
		break;
	}
	case D3DDECLTYPE_SHORT2: {
		const short *s = (const short *)p;
		out[0] = (float)s[0];
		out[1] = (float)s[1];
		break;
	}
	case D3DDECLTYPE_SHORT2N: {
		const short *s = (const short *)p;
		out[0] = s[0] / 32767.0f;
		out[1] = s[1] / 32767.0f;
		break;
	}
	case D3DDECLTYPE_USHORT2N: {
		const unsigned short *s = (const unsigned short *)p;
		out[0] = s[0] / 65535.0f;
		out[1] = s[1] / 65535.0f;
		break;
	}
	case D3DDECLTYPE_FLOAT16_2:
	case D3DDECLTYPE_FLOAT16_4: {
		int n = (e->Type == D3DDECLTYPE_FLOAT16_4) ? 4 : 2;
		int i;
		for (i = 0; i < n; i++) {
			unsigned h = ((const unsigned short *)p)[i];
			unsigned sig = (h >> 15) & 1u;
			unsigned exp = (h >> 10) & 31u;
			unsigned man = h & 1023u;
			float v;
			if (exp == 0)
				v = (man == 0) ? 0.0f : ldexpf((float)man / 1024.0f, -14);
			else if (exp == 31)
				v = 1.0e30f;
			else
				v = ldexpf(1.0f + (float)man / 1024.0f, (int)exp - 15);
			out[i] = sig ? -v : v;
		}
		break;
	}
	default:
		if (decl_type_size(e->Type) >= 12)
			memcpy(out, p, 12);
		else if (decl_type_size(e->Type) >= 8)
			memcpy(out, p, 8);
		else
			memcpy(out, p, 4);
		break;
	}
}

static const unsigned char *stream_base(const D3DVERTEXELEMENT9 *e, const unsigned char *s0,
					const unsigned char *s1, const unsigned char *s2)
{
	if (e->Stream == 0)
		return s0;
	if (e->Stream == 1)
		return s1;
	if (e->Stream == 2)
		return s2;
	return NULL;
}

typedef struct OutDcl {
	BYTE usage;
	BYTE index;
	BYTE used;
} OutDcl;

static void fill_inputs(VsReg vin[16], OutDcl omap[16], const D3DVERTEXELEMENT9 *decl,
			const unsigned char *s0, const unsigned char *s1, const unsigned char *s2,
			const DWORD *code, UINT ndwords)
{
	int i;
	int mapped = 0;
	UINT pc;

	for (i = 0; i < 16; i++) {
		vin[i].v[0] = vin[i].v[1] = vin[i].v[2] = 0.0f;
		vin[i].v[3] = 1.0f;
		if (omap) {
			omap[i].usage = 0;
			omap[i].index = 0;
			omap[i].used = 0;
		}
	}
	if (!code || ndwords < 2)
		return;

	pc = 1;
	while (pc < ndwords) {
		DWORD op = code[pc];
		int opcode = (int)(op & D3DSI_OPCODE_MASK);
		int extra = (int)((op & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT);
		if (opcode == D3DSIO_END)
			break;
		if (opcode == D3DSIO_COMMENT) {
			pc += 1u + (UINT)((op & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT);
			continue;
		}
		if (opcode == D3DSIO_DCL && extra >= 2 && pc + 2 < ndwords) {
			DWORD usage_tok = code[pc + 1];
			DWORD dest = code[pc + 2];
			int usage = (int)(usage_tok & D3DSP_DCL_USAGE_MASK);
			int uidx = (int)((usage_tok & D3DSP_DCL_USAGEINDEX_MASK) >>
					 D3DSP_DCL_USAGEINDEX_SHIFT);
			int rtype = reg_type(dest);
			int rnum = (int)(dest & D3DSP_REGNUM_MASK);
			if (rtype == D3DSPR_INPUT && rnum >= 0 && rnum < 16 && decl) {
				const D3DVERTEXELEMENT9 *e;
				for (e = decl; e->Stream != 0xff; e++) {
					if (e->Usage == (BYTE)usage && e->UsageIndex == (BYTE)uidx) {
						load_elem(vin[rnum].v, stream_base(e, s0, s1, s2), e);
						mapped = 1;
						break;
					}
				}
			} else if (omap && rnum >= 0 && rnum < 16 &&
				   (rtype == D3DSPR_OUTPUT || rtype == D3DSPR_RASTOUT ||
				    rtype == D3DSPR_ATTROUT || rtype == D3DSPR_COLOROUT)) {
				omap[rnum].usage = (BYTE)usage;
				omap[rnum].index = (BYTE)uidx;
				omap[rnum].used = 1;
			}
		}
		if (extra <= 0)
			extra = 1;
		pc += 1u + (UINT)extra;
	}

	if (mapped || !decl)
		return;
	for (i = 0; decl[i].Stream != 0xff; i++) {
		const D3DVERTEXELEMENT9 *e = &decl[i];
		float tmp[4];
		int slot = -1;
		load_elem(tmp, stream_base(e, s0, s1, s2), e);
		if (e->Usage == D3DDECLUSAGE_POSITION && e->UsageIndex == 0)
			slot = 0;
		else if (e->Usage == D3DDECLUSAGE_TEXCOORD && e->UsageIndex == 0)
			slot = 1;
		else if (e->Usage == D3DDECLUSAGE_COLOR && e->UsageIndex == 0)
			slot = 2;
		else if (e->Usage == D3DDECLUSAGE_TEXCOORD && e->UsageIndex == 1)
			slot = 3;
		if (slot >= 0)
			memcpy(vin[slot].v, tmp, 16);
	}
}

static void apply_srcmod(float v[4], DWORD tok)
{
	int mod = (int)((tok & D3DSP_SRCMOD_MASK) >> D3DSP_SRCMOD_SHIFT);
	int c;
	switch (mod) {
	case 1: /* NEG */
		for (c = 0; c < 4; c++)
			v[c] = -v[c];
		break;
	case 11: /* ABS */
		for (c = 0; c < 4; c++)
			v[c] = fabsf(v[c]);
		break;
	case 12: /* ABSNEG */
		for (c = 0; c < 4; c++)
			v[c] = -fabsf(v[c]);
		break;
	case 6: /* COMP 1-x */
		for (c = 0; c < 4; c++)
			v[c] = 1.0f - v[c];
		break;
	default:
		break;
	}
}

static void swizzle_src(float v[4], DWORD tok)
{
	float in[4];
	int c;
	memcpy(in, v, 16);
	for (c = 0; c < 4; c++) {
		int src = (int)((tok >> (D3DSP_SWIZZLE_SHIFT + c * 2)) & 3);
		v[c] = in[src];
	}
}

static int read_src(float out[4], DWORD tok, VsReg *tmp, VsReg *vin, VsReg *c, VsReg *a0)
{
	int type = reg_type(tok);
	int num = (int)(tok & D3DSP_REGNUM_MASK);
	const float *p = NULL;

	if (tok & D3DVS_ADDRMODE_RELATIVE)
		num += (int)floorf(a0->v[0] + (a0->v[0] >= 0.0f ? 0.5f : -0.5f));
	switch (type) {
	case D3DSPR_TEMP:
		if (num < 0 || num >= 32)
			return 0;
		p = tmp[num].v;
		break;
	case D3DSPR_INPUT:
		if (num < 0 || num >= 16)
			return 0;
		p = vin[num].v;
		break;
	case D3DSPR_CONST:
		if (num < 0 || num >= 256)
			return 0;
		p = c[num].v;
		break;
	case D3DSPR_CONST2:
		/* c2048+ has no backing store here; only 256 float constants exist. */
		return 0;
	case D3DSPR_ADDR:
		p = a0->v;
		break;
	default:
		return 0;
	}
	memcpy(out, p, 16);
	swizzle_src(out, tok);
	apply_srcmod(out, tok);
	return 1;
}

static int write_dst(DWORD tok, const float src[4], VsReg *tmp, VsReg *rast, VsReg *attr,
		     VsReg *tex, VsReg *a0, const OutDcl *omap)
{
	int type = reg_type(tok);
	int num = (int)(tok & D3DSP_REGNUM_MASK);
	int mask = (int)((tok & D3DSP_WRITEMASK_ALL) >> 16);
	int sat = ((tok & D3DSP_DSTMOD_MASK) == D3DSPDM_SATURATE);
	float *p = NULL;
	int c;
	float v[4];

	memcpy(v, src, 16);
	if (sat) {
		for (c = 0; c < 4; c++) {
			if (v[c] < 0.0f)
				v[c] = 0.0f;
			if (v[c] > 1.0f)
				v[c] = 1.0f;
		}
	}
	switch (type) {
	case D3DSPR_TEMP:
		if (num < 0 || num >= 32)
			return 0;
		p = tmp[num].v;
		break;
	case D3DSPR_RASTOUT:
		if (num < 0 || num >= 3)
			return 0;
		p = rast[num].v;
		break;
	case D3DSPR_ATTROUT:
		if (num < 0 || num >= 2)
			return 0;
		p = attr[num].v;
		break;
	case D3DSPR_OUTPUT:
		if (num < 0 || num >= 16)
			return 0;
		if (omap && omap[num].used) {
			BYTE u = omap[num].usage;
			BYTE idx = omap[num].index;
			if (u == D3DDECLUSAGE_POSITION || u == D3DDECLUSAGE_POSITIONT)
				p = rast[0].v;
			else if (u == D3DDECLUSAGE_COLOR)
				p = attr[idx < 2 ? idx : 0].v;
			else if (u == D3DDECLUSAGE_TEXCOORD)
				p = tex[idx < 8 ? idx : 0].v;
			else
				p = tex[num < 8 ? num : 0].v;
		} else {
			if (num >= 8)
				return 0;
			p = tex[num].v;
		}
		break;
	case D3DSPR_ADDR:
		p = a0->v;
		break;
	default:
		return 0;
	}
	if (!mask)
		mask = 0xf;
	for (c = 0; c < 4; c++) {
		if (mask & (1 << c))
			p[c] = v[c];
	}
	return 1;
}

static float dot4(const float a[4], const float b[4])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static DWORD pack_color(const float v[4])
{
	int ch[4];
	int i;
	for (i = 0; i < 4; i++) {
		float t = v[i] * 255.0f + 0.5f;
		if (t < 0.0f)
			t = 0.0f;
		if (t > 255.0f)
			t = 255.0f;
		ch[i] = (int)t;
	}
	return ((DWORD)ch[3] << 24) | ((DWORD)ch[0] << 16) | ((DWORD)ch[1] << 8) | (DWORD)ch[2];
}

static int vs_cmp(int mode, float a, float b)
{
	switch (mode) {
	case D3DSPC_GT:
		return a > b;
	case D3DSPC_EQ:
		return a == b;
	case D3DSPC_GE:
		return a >= b;
	case D3DSPC_LT:
		return a < b;
	case D3DSPC_NE:
		return a != b;
	case D3DSPC_LE:
		return a <= b;
	default:
		return 0;
	}
}

int vs_exec(const DWORD *code, UINT code_bytes, const float constants[256][4], unsigned const_ver,
	    const D3DVERTEXELEMENT9 *decl, const unsigned char *s0, const unsigned char *s1,
	    const unsigned char *s2, SwVert *out, char *err, int err_n)
{
	VsReg tmp[32], vin[16], rast[3], attr[2], tex[8], a0;
	VsReg *c;
	OutDcl omap[16];
	UINT ndwords, pc;
	int i;
	/* Snapshotting all 256 constants per vertex dominated the interpreter, so
	 * keep one mirror and refresh it only when the shader or the app's
	 * constants change. DEF writes land in the mirror and stay valid for the
	 * same (shader, version) pair, which is exactly their scope. */
	static VsReg c_mirror[256];
	static const DWORD *c_shader;
	static unsigned c_ver = 0xffffffffu;

	if (!code || !out || code_bytes < 8)
		return 0;
	ndwords = code_bytes / 4u;
	memset(tmp, 0, sizeof(tmp));
	memset(rast, 0, sizeof(rast));
	memset(attr, 0, sizeof(attr));
	memset(tex, 0, sizeof(tex));
	memset(&a0, 0, sizeof(a0));
	rast[0].v[3] = 1.0f;
	attr[0].v[0] = attr[0].v[1] = attr[0].v[2] = attr[0].v[3] = 1.0f;
	if (c_shader != code || c_ver != const_ver) {
		memcpy(c_mirror, constants, sizeof(c_mirror));
		c_shader = code;
		c_ver = const_ver;
	}
	c = c_mirror;
	fill_inputs(vin, omap, decl, s0, s1, s2, code, ndwords);

	pc = 1;
	{
	int skip = 0;
	int relext = (int)((code[0] >> 8) & 0xffu) >= 2;
	while (pc < ndwords) {
		DWORD op = code[pc];
		int opcode = (int)(op & D3DSI_OPCODE_MASK);
		int extra = (int)((op & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT);
		const DWORD *args;
		int po[5];
		float s0v[4], s1v[4], s2v[4], r[4];

		if (opcode == D3DSIO_END)
			break;
		if (opcode == D3DSIO_COMMENT) {
			pc += 1u + (UINT)((op & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT);
			continue;
		}
		if (opcode == D3DSIO_ELSE || opcode == D3DSIO_ENDIF || opcode == D3DSIO_NOP ||
		    opcode == D3DSIO_RET || opcode == D3DSIO_BREAK)
			;
		else if (extra <= 0) {
			if (err && err_n)
				_snprintf(err, err_n, "vs len0 op=%d", opcode);
			return 0;
		}
		if (pc + 1u + (UINT)extra > ndwords)
			return 0;
		args = code + pc + 1;
		/* From vs_2_0 on, a parameter token that uses relative addressing is
		 * followed by an extra address token. Operands are therefore not at
		 * fixed offsets and have to be walked. */
		{
			int k = 0, q;
			for (q = 0; q < 5; q++) {
				po[q] = k;
				k += (k < extra && relext && (args[k] & D3DVS_ADDRMODE_RELATIVE))
					     ? 2
					     : 1;
			}
		}

		if (opcode == D3DSIO_IFC || opcode == D3DSIO_IF) {
			if (skip) {
				skip++;
			} else if (opcode == D3DSIO_IFC) {
				int mode = (int)((op >> 16) & 7);
				if (po[1] >= extra || !read_src(s0v, args[po[0]], tmp, vin, c, &a0) ||
				    !read_src(s1v, args[po[1]], tmp, vin, c, &a0))
					goto bad;
				if (!vs_cmp(mode, s0v[0], s1v[0]))
					skip = 1;
			} else {
				if (po[0] >= extra || !read_src(s0v, args[po[0]], tmp, vin, c, &a0))
					goto bad;
				if (s0v[0] == 0.0f && s0v[1] == 0.0f && s0v[2] == 0.0f && s0v[3] == 0.0f)
					skip = 1;
			}
			pc += 1u + (UINT)extra;
			continue;
		}
		if (opcode == D3DSIO_ELSE) {
			if (skip == 1)
				skip = 0;
			else if (skip == 0)
				skip = 1;
			pc += 1u + (UINT)extra;
			continue;
		}
		if (opcode == D3DSIO_ENDIF) {
			if (skip)
				skip--;
			pc += 1u + (UINT)extra;
			continue;
		}
		if (skip) {
			pc += 1u + (UINT)extra;
			continue;
		}

		switch (opcode) {
		case D3DSIO_NOP:
		case D3DSIO_DCL:
			break;
		case D3DSIO_DEF:
			if (extra >= 5) {
				int n = (int)(args[po[0]] & D3DSP_REGNUM_MASK);
				if (n >= 0 && n < 256)
					memcpy(c[n].v, &args[po[1]], 16);
			}
			break;
		case D3DSIO_MOV:
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			if (!write_dst(args[po[0]], s0v, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_MOVA:
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++)
				r[i] = floorf(s0v[i] + (s0v[i] >= 0.0f ? 0.5f : -0.5f));
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_ADD:
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++)
				r[i] = s0v[i] + s1v[i];
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_SUB:
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++)
				r[i] = s0v[i] - s1v[i];
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_MUL:
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++)
				r[i] = s0v[i] * s1v[i];
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_MAD:
			if (po[3] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0) ||
			    !read_src(s2v, args[po[3]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++)
				r[i] = s0v[i] * s1v[i] + s2v[i];
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_DP3: {
			float d;
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			d = s0v[0] * s1v[0] + s0v[1] * s1v[1] + s0v[2] * s1v[2];
			r[0] = r[1] = r[2] = r[3] = d;
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		}
		case D3DSIO_DP4: {
			float d;
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			d = dot4(s0v, s1v);
			r[0] = r[1] = r[2] = r[3] = d;
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		}
		case D3DSIO_MIN:
		case D3DSIO_MAX:
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++)
				r[i] = (opcode == D3DSIO_MIN)
					   ? (s0v[i] < s1v[i] ? s0v[i] : s1v[i])
					   : (s0v[i] > s1v[i] ? s0v[i] : s1v[i]);
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_SLT:
		case D3DSIO_SGE:
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++) {
				int cmp = (opcode == D3DSIO_SLT) ? (s0v[i] < s1v[i]) : (s0v[i] >= s1v[i]);
				r[i] = cmp ? 1.0f : 0.0f;
			}
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_LRP:
			if (po[3] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0) ||
			    !read_src(s2v, args[po[3]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++)
				r[i] = s0v[i] * s1v[i] + (1.0f - s0v[i]) * s2v[i];
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_FRC:
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++)
				r[i] = s0v[i] - floorf(s0v[i]);
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_RCP:
		case D3DSIO_RSQ:
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			{
				float x = s0v[0];
				float y = (opcode == D3DSIO_RCP) ? (x != 0.0f ? 1.0f / x : 0.0f)
								 : (x > 0.0f ? 1.0f / sqrtf(x) : 0.0f);
				r[0] = r[1] = r[2] = r[3] = y;
			}
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_ABS:
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			for (i = 0; i < 4; i++)
				r[i] = fabsf(s0v[i]);
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_NRM:
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			{
				float l = sqrtf(s0v[0] * s0v[0] + s0v[1] * s0v[1] + s0v[2] * s0v[2]);
				if (l <= 0.0f)
					l = 1.0f;
				r[0] = s0v[0] / l;
				r[1] = s0v[1] / l;
				r[2] = s0v[2] / l;
				r[3] = s0v[3];
			}
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_SINCOS:
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			r[0] = cosf(s0v[0]);
			r[1] = sinf(s0v[0]);
			r[2] = 0.0f;
			r[3] = 1.0f;
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_EXP:
		case D3DSIO_EXPP: {
			float e;
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			e = powf(2.0f, s0v[0]);
			r[0] = r[1] = r[2] = r[3] = e;
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		}
		case D3DSIO_LOG:
		case D3DSIO_LOGP: {
			float e;
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			e = (s0v[0] > 0.0f) ? logf(s0v[0]) / logf(2.0f) : -127.0f;
			r[0] = r[1] = r[2] = r[3] = e;
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		}
		case D3DSIO_POW: {
			float e;
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			e = powf(fabsf(s0v[0]), s1v[0]);
			r[0] = r[1] = r[2] = r[3] = e;
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		}
		case D3DSIO_CRS:
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			r[0] = s0v[1] * s1v[2] - s0v[2] * s1v[1];
			r[1] = s0v[2] * s1v[0] - s0v[0] * s1v[2];
			r[2] = s0v[0] * s1v[1] - s0v[1] * s1v[0];
			r[3] = s0v[3];
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_DST:
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0) ||
			    !read_src(s1v, args[po[2]], tmp, vin, c, &a0))
				goto bad;
			r[0] = 1.0f;
			r[1] = s0v[1] * s1v[1];
			r[2] = s0v[2];
			r[3] = s1v[3];
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_LIT:
			if (po[1] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			r[0] = 1.0f;
			r[1] = (s0v[0] < 0.0f) ? 0.0f : s0v[0];
			r[2] = 0.0f;
			if (s0v[0] > 0.0f && s0v[1] > 0.0f)
				r[2] = powf(s0v[1], s0v[3]);
			r[3] = 1.0f;
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		case D3DSIO_M4x4:
		case D3DSIO_M4x3: {
			int rows = (opcode == D3DSIO_M4x4) ? 4 : 3;
			int n, k;
			if (po[2] >= extra || !read_src(s0v, args[po[1]], tmp, vin, c, &a0))
				goto bad;
			n = (int)(args[po[2]] & D3DSP_REGNUM_MASK);
			if (reg_type(args[po[2]]) != D3DSPR_CONST || n < 0 || n + rows > 256)
				goto bad;
			memset(r, 0, sizeof(r));
			r[3] = 1.0f;
			for (k = 0; k < rows; k++)
				r[k] = dot4(s0v, c[n + k].v);
			if (!write_dst(args[po[0]], r, tmp, rast, attr, tex, &a0, omap))
				goto bad;
			break;
		}
		default:
			if (err && err_n)
				_snprintf(err, err_n, "vs op=%d", opcode);
			return 0;
		}
		pc += 1u + (UINT)extra;
		continue;
	bad:
		if (err && err_n)
			_snprintf(err, err_n, "vs bad op=%d", opcode);
		return 0;
	}
	}

	if (rast[0].v[0] == 0.0f && rast[0].v[1] == 0.0f && rast[0].v[2] == 0.0f &&
	    rast[0].v[3] == 1.0f) {
		int has_pos = 0;
		for (i = 0; i < 16; i++) {
			if (omap[i].used && (omap[i].usage == D3DDECLUSAGE_POSITION ||
					     omap[i].usage == D3DDECLUSAGE_POSITIONT))
				has_pos = 1;
		}
		if (!has_pos)
			rast[0] = tex[0];
	}

	memset(out, 0, sizeof(*out));
	out->x = rast[0].v[0];
	out->y = rast[0].v[1];
	out->z = rast[0].v[2];
	out->rhw = rast[0].v[3] != 0.0f ? rast[0].v[3] : 1.0f;
	out->color = pack_color(attr[0].v);
	out->u = tex[0].v[0];
	out->v = tex[0].v[1];
	{
		static int once;
		if (++once == 1) {
			FILE *f = fopen("d3d9_sw.log", "a");
			if (f) {
				fprintf(f,
					"[d3d9_sw] vs_out rast=(%.3f,%.3f,%.3f,%.3f) tex0=(%.4f,%.4f) tex1=(%.4f,%.4f) attr=(%.3f,%.3f,%.3f,%.3f) vin0=(%.3f,%.3f,%.3f) vin1=(%.3f,%.3f)\n",
					rast[0].v[0], rast[0].v[1], rast[0].v[2], rast[0].v[3],
					tex[0].v[0], tex[0].v[1], tex[1].v[0], tex[1].v[1],
					attr[0].v[0], attr[0].v[1], attr[0].v[2], attr[0].v[3],
					vin[0].v[0], vin[0].v[1], vin[0].v[2], vin[1].v[0],
					vin[1].v[1]);
				fclose(f);
			}
		}
	}
	return 1;
}
