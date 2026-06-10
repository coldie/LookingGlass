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

#include "wgc_util.h"

#include "common/windebug.h"

#include <dxgi1_6.h>
#include <math.h>
#include <wchar.h>

bool wgcUtil_colorSpaceIsHDR(DXGI_COLOR_SPACE_TYPE colorSpace)
{
  return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
         colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 ||
         colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

DXGI_COLOR_SPACE_TYPE wgcUtil_getOutputColorSpace(IDXGIOutput * output)
{
  DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  if (!output)
    return colorSpace;

  IDXGIOutput6 * output6 = NULL;
  HRESULT hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6,
    (void **)&output6);
  if (SUCCEEDED(hr))
  {
    DXGI_OUTPUT_DESC1 desc1;
    hr = IDXGIOutput6_GetDesc1(output6, &desc1);
    if (SUCCEEDED(hr))
      colorSpace = desc1.ColorSpace;
    IDXGIOutput6_Release(output6);
  }
  return colorSpace;
}

float wgcUtil_halfToFloat(uint16_t h)
{
  const uint16_t sign = h >> 15;
  const uint16_t exp  = (h >> 10) & 0x1f;
  const uint16_t mant = h & 0x03ff;
  float value;

  if (exp == 0)
    value = mant ? ldexpf((float)mant / 1024.0f, -14) : 0.0f;
  else if (exp == 31)
    value = mant ? 0.0f : 65504.0f;
  else
    value = ldexpf(1.0f + (float)mant / 1024.0f, (int)exp - 15);

  return sign ? -value : value;
}

bool wgcUtil_createHString(const WCHAR * str, HSTRING * result)
{
  const HRESULT hr = WindowsCreateString(str, (UINT32)wcslen(str), result);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("WindowsCreateString failed", hr);
    return false;
  }
  return true;
}
