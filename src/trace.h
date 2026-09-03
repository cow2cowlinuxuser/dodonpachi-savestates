#ifndef D3D9_SW_TRACE_H
#define D3D9_SW_TRACE_H

#include <stddef.h>

void sw_trace(const char *fmt, ...);
void d3d9_trace_wrap_dev(void **slots, size_t bytes);
void d3d9_trace_wrap_d3d(void **slots, size_t bytes);

#endif
