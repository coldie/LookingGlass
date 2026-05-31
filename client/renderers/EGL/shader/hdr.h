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

/*
   _______                               _____ __              __             ____             __
  / ____(_)___  ___  ____ ___  ____ _   / ___// /_  ____ _____/ /__  _____   / __ \____ ______/ /__
 / /   / / __ \/ _ \/ __ `__ \/ __ `/   \__ \/ __ \/ __ `/ __  / _ \/ ___/  / /_/ / __ `/ ___/ //_/
/ /___/ / / / /  __/ / / / / / /_/ /   ___/ / / / / /_/ / /_/ /  __/ /     / ____/ /_/ / /__/ ,<
\____/_/_/ /_/\___/_/ /_/ /_/\__,_/   /____/_/ /_/\__,_/\__,_/\___/_/     /_/    \__,_/\___/_/|_|
        http://en.sbence.hu/        Shader: Try to get the SDR part of HDR content
*/

/**
 * Translated to GLSL, original source is:
 * https://github.com/VoidXH/Cinema-Shader-Pack
 */

// Configuration ---------------------------------------------------------------
const float knee          = 0.75;    // Compressor knee position
const float ratio         = 4.0;     // Compressor ratio: 1 = disabled, <1 = expander
// -----------------------------------------------------------------------------

// Precalculated values
const float compressor = 1.0 / ratio;

// PQ constants
const float m1inv = 16384.0 / 2610.0;
const float m2inv = 32.0 / 2523.0;
const float c1    = 3424.0 / 4096.0;
const float c2    = 2413.0 / 128.0;
const float c3    = 2392.0 / 128.0;

#define HDR_MAPPING_SIMPLE   0
#define HDR_MAPPING_REINHARD 1
#define HDR_MAPPING_ACES     2
#define HDR_MAPPING_CLIP     3
#define HDR_MAPPING_OFF      4

#define HDR_VIEW_NORMAL      0
#define HDR_VIEW_FALSE_COLOR 1

float minGain(vec3 pixel) { return min(pixel.r, min(pixel.g, pixel.b)); }
float maxGain(vec3 pixel) { return max(pixel.r, max(pixel.g, pixel.b)); }
float midGain(vec3 pixel)
{
  return pixel.r < pixel.g ?
    (pixel.r < pixel.b ?
      min(pixel.g, pixel.b) : // min = r
      min(pixel.r, pixel.g)) : // min = b
    (pixel.g < pixel.b ?
      min(pixel.r, pixel.b) : // min = g
      min(pixel.r, pixel.g)); // min = b
}

vec3 compress(vec3 pixel)
{
  float maxGain = maxGain(pixel);
  if (maxGain <= 0.0)
    return vec3(0.0);
  return pixel * (maxGain < knee ? maxGain :
      knee + max(maxGain - knee, 0.0) * compressor) / maxGain;
}

vec3 fixClip(vec3 pixel)
{
  // keep the (mid - min) / (max - min) ratio
  float preMin  = minGain(pixel);
  float preMid  = midGain(pixel);
  float preMax  = maxGain(pixel);
  vec3  clip    = clamp(pixel, 0.0, 1.0);
  if (preMax - preMin < 0.0001)
    return clip;
  float postMin = minGain(clip);
  float postMid = midGain(clip);
  float postMax = maxGain(clip);
  float ratio   = (preMid - preMin) / (preMax - preMin);
  float newMid  = ratio * (postMax - postMin) + postMin;
  return vec3(clip.r != postMid ? clip.r : newMid,
                clip.g != postMid ? clip.g : newMid,
                clip.b != postMid ? clip.b : newMid);
}

// Decode PQ to scRGB-relative linear light. The WGC preserve-pq path encodes
// scRGB with 1.0 == 80 nits, so this reconstructs the same scale as RGBA16F.
vec3 pq2lin(vec3 pq, float gain)
{
  vec3 p = pow(pq, vec3(m2inv));
  vec3 d = max(p - c1, vec3(0.0)) / (c2 - c3 * p);
  return pow(d, vec3(m1inv)) * (10000.0 / 80.0);
}

float pq2lin1(float pq)
{
  float p = pow(pq, m2inv);
  float d = max(p - c1, 0.0) / (c2 - c3 * p);
  return pow(d, m1inv) * (10000.0 / 80.0);
}

float hdrPQLuminance(float pq)
{
  return pq2lin1(clamp(pq, 0.0, 1.0));
}

vec3 srgb2lin(vec3 c)
{
  vec3 v = c / 12.92;
  vec3 v2 = pow((c + vec3(0.055)) / 1.055, vec3(2.4));
  vec3 threshold = vec3(0.04045);
  vec3 result = mix(v, v2, greaterThanEqual(c, threshold));
  return result;
}

vec3 lin2srgb(vec3 c)
{
  vec3 v = c * 12.92;
  vec3 v2 = pow(c, vec3(1.0/2.4)) * 1.055 - 0.055;
  vec3 threshold = vec3(0.0031308);
  vec3 result = mix(v, v2, greaterThanEqual(c, threshold));
  return result;
}

// in linear space
vec3 bt2020to709(vec3 bt2020)
{
  return vec3(
    bt2020.r *  1.6605 + bt2020.g * -0.5876 + bt2020.b * -0.0728,
    bt2020.r * -0.1246 + bt2020.g *  1.1329 + bt2020.b * -0.0083,
    bt2020.r * -0.0182 + bt2020.g * -0.1006 + bt2020.b * 1.1187);
}

vec3 linear709to2020(vec3 rgb)
{
  return vec3(
    dot(rgb, vec3(0.6274039, 0.3292829, 0.0433131)),
    dot(rgb, vec3(0.0690973, 0.9195404, 0.0113622)),
    dot(rgb, vec3(0.0163914, 0.0880133, 0.8955953)));
}

vec3 lin2pq(vec3 linear)
{
  const float m1 = 2610.0 / 16384.0;
  const float m2 = 2523.0 / 32.0;
  vec3 n = clamp((max(linear, vec3(0.0)) * 80.0) / 10000.0, 0.0, 1.0);
  vec3 p = pow(n, vec3(m1));
  return pow((vec3(c1) + vec3(c2) * p) / (vec3(1.0) + vec3(c3) * p),
    vec3(m2));
}

vec3 mapHDRLinearToTarget(vec3 linear, float targetNits, float sourceNits,
    int mode)
{
  if (mode == HDR_MAPPING_OFF)
    return linear;

  float target = max(targetNits, 1.0);
  float source = max(sourceNits, target);
  if (source <= target)
    return linear;

  vec3 c = max(linear, vec3(0.0));
  float lum = dot(c, vec3(0.2627, 0.6780, 0.0593)) * 80.0;
  if (lum <= 0.0)
    return c;

  float mapped;
  if (mode == HDR_MAPPING_CLIP)
  {
    mapped = min(lum, target);
  }
  else if (mode == HDR_MAPPING_REINHARD)
  {
    float x = lum / target;
    mapped = target * (x / (1.0 + x));
  }
  else if (mode == HDR_MAPPING_ACES)
  {
    float x = lum / target;
    mapped = target * clamp((x * (2.51 * x + 0.03)) /
      (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
  }
  else
  {
    float kneeNits = target * 0.75;
    if (lum <= kneeNits)
      mapped = lum;
    else
    {
      float t = clamp((lum - kneeNits) / max(source - kneeNits, 1.0),
          0.0, 1.0);
      mapped = kneeNits + (target - kneeNits) *
        (1.0 - (1.0 - t) * (1.0 - t));
    }
  }

  return c * (mapped / lum);
}

vec3 mapPQToTarget(vec3 pq, float targetNits, float sourceNits, int mode)
{
  return lin2pq(mapHDRLinearToTarget(pq2lin(pq, targetNits),
    targetNits, sourceNits, mode));
}

vec3 linear709ToBt2020PQ(vec3 rgb, float targetNits, float sourceNits,
    int mode)
{
  return lin2pq(mapHDRLinearToTarget(linear709to2020(rgb),
    targetNits, sourceNits, mode));
}

vec3 sdr709ToBt2020PQ(vec3 rgb)
{
  return lin2pq(linear709to2020(srgb2lin(clamp(rgb, 0.0, 1.0))));
}

float hdrLuminance(vec3 color, float gain, bool pq)
{
  if (pq)
  {
    color = pq2lin(color.rgb, gain);
    return dot(max(color, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
  }

  return dot(max(color, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
}

vec3 acesApprox(vec3 color)
{
  const float a = 2.51;
  const float b = 0.03;
  const float c = 2.43;
  const float d = 0.59;
  const float e = 0.14;
  return clamp((color * (a * color + b)) /
      (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 falseColorHDR(float lum)
{
  if (lum < 0.5)
    return vec3(0.0, lum * 2.0, 1.0);
  if (lum < 1.0)
    return mix(vec3(0.0, 1.0, 1.0), vec3(0.9), (lum - 0.5) * 2.0);
  if (lum < 2.0)
    return mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), lum - 1.0);
  if (lum < 4.0)
    return mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.45, 0.0), (lum - 2.0) / 2.0);
  if (lum < 8.0)
    return mix(vec3(1.0, 0.45, 0.0), vec3(1.0, 0.0, 0.0), (lum - 4.0) / 4.0);
  if (lum < 16.0)
    return mix(vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 1.0), (lum - 8.0) / 8.0);
  return vec3(1.0);
}

vec3 mapToSDR(vec3 color, float gain, bool pq, int mode)
{
  if (pq)
  {
    color = pq2lin(color.rgb, gain);
  }

  // WGC HDR is scRGB-like linear light. Treat 1.0 as SDR reference white and
  // scale it to the configured SDR peak before applying the display transfer.
  color *= 80.0 / max(gain, 1.0);

  color = max(color, vec3(0.0));

  if (mode == HDR_MAPPING_OFF)
    color = max(color, vec3(0.0));
  else if (mode == HDR_MAPPING_REINHARD)
    color = color / (color + vec3(1.0));
  else if (mode == HDR_MAPPING_ACES)
    color = acesApprox(color);
  else if (mode == HDR_MAPPING_CLIP)
    color = clamp(color, 0.0, 1.0);
  else
    color = compress(color);

  return lin2srgb(clamp(color, 0.0, 1.0));
}
