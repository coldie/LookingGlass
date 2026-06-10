/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef _H_WGC_UTIL_
#define _H_WGC_UTIL_

#include <stdbool.h>
#include <stdint.h>

#include <dxgi.h>
#include <winstring.h>

// Small helpers shared between the WGC backend and the top-level interface.

bool wgcUtil_colorSpaceIsHDR(DXGI_COLOR_SPACE_TYPE colorSpace);

// Queries the current color space of `output`; a NULL output returns the SDR
// default (DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709).
DXGI_COLOR_SPACE_TYPE wgcUtil_getOutputColorSpace(IDXGIOutput * output);

float wgcUtil_halfToFloat(uint16_t h);

bool wgcUtil_createHString(const WCHAR * str, HSTRING * result);

#endif
