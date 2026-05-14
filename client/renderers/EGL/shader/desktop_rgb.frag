#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;
precision highp int;

#define EGL_SCALE_AUTO    0
#define EGL_SCALE_NEAREST 1
#define EGL_SCALE_LINEAR  2
#define EGL_SCALE_MAX     3

#define FRAME_TYPE_NV12 7
#define FRAME_TYPE_YUY2 8
#define FRAME_TYPE_UYVY 9

#include "color_blind.h"
#include "hdr.h"

in  vec2 uv;
out vec4 color;

uniform sampler2D sampler1;

uniform int   scaleAlgo;
uniform int   frameType;
uniform vec2  desktopSize;

uniform float nvGain;
uniform int   cbMode;
uniform bool  isHDR;
uniform bool  mapHDRtoSDR;
uniform float mapHDRGain;
uniform bool  mapHDRPQ;

vec3 yuvToRgb(float y, float u, float v)
{
  u -= 0.5;
  v -= 0.5;
  return vec3(
    y + 1.5748 * v,
    y - 0.1873 * u - 0.4681 * v,
    y + 1.8556 * u);
}

vec4 sampleNV12(vec2 pos)
{
  ivec2 pixel = ivec2(clamp(pos * desktopSize,
    vec2(0.0), max(desktopSize - vec2(1.0), vec2(0.0))));
  int yHeight = int(desktopSize.y);
  int uvX = (pixel.x / 2) * 2;
  int uvY = yHeight + pixel.y / 2;

  vec4 yv = texelFetch(sampler1, ivec2(pixel.x / 4, pixel.y), 0);
  vec4 uvv0 = texelFetch(sampler1, ivec2(uvX / 4, uvY), 0);
  vec4 uvv1 = texelFetch(sampler1, ivec2((uvX + 1) / 4, uvY), 0);

  float y = pixel.x % 4 == 0 ? yv.r :
            pixel.x % 4 == 1 ? yv.g :
            pixel.x % 4 == 2 ? yv.b : yv.a;
  float u = uvX % 4 == 0 ? uvv0.r :
            uvX % 4 == 1 ? uvv0.g :
            uvX % 4 == 2 ? uvv0.b : uvv0.a;
  float v = (uvX + 1) % 4 == 0 ? uvv1.r :
            (uvX + 1) % 4 == 1 ? uvv1.g :
            (uvX + 1) % 4 == 2 ? uvv1.b : uvv1.a;
  return vec4(clamp(yuvToRgb(y, u, v), 0.0, 1.0), 1.0);
}

vec4 sampleYUY2(vec2 pos)
{
  ivec2 pixel = ivec2(clamp(pos * desktopSize,
    vec2(0.0), max(desktopSize - vec2(1.0), vec2(0.0))));
  vec4 p = texelFetch(sampler1, ivec2(pixel.x / 2, pixel.y), 0);
  float y = (pixel.x & 1) == 0 ? p.r : p.b;
  return vec4(clamp(yuvToRgb(y, p.g, p.a), 0.0, 1.0), 1.0);
}

vec4 sampleUYVY(vec2 pos)
{
  ivec2 pixel = ivec2(clamp(pos * desktopSize,
    vec2(0.0), max(desktopSize - vec2(1.0), vec2(0.0))));
  vec4 p = texelFetch(sampler1, ivec2(pixel.x / 2, pixel.y), 0);
  float y = (pixel.x & 1) == 0 ? p.g : p.a;
  return vec4(clamp(yuvToRgb(y, p.r, p.b), 0.0, 1.0), 1.0);
}

void main()
{
  if (frameType == FRAME_TYPE_NV12)
    color = sampleNV12(uv);
  else if (frameType == FRAME_TYPE_YUY2)
    color = sampleYUY2(uv);
  else if (frameType == FRAME_TYPE_UYVY)
    color = sampleUYVY(uv);
  else switch (scaleAlgo)
  {
    case EGL_SCALE_NEAREST:
    {
      vec2 ts = vec2(textureSize(sampler1, 0));
      color   = texelFetch(sampler1, ivec2(uv * ts), 0);
      break;
    }

    case EGL_SCALE_LINEAR:
    {
      color = texture(sampler1, uv);
      break;
    }
  }

  if (isHDR && mapHDRtoSDR)
    color.rgb = mapToSDR(color.rgb, mapHDRGain, mapHDRPQ);

  if (cbMode > 0)
    color = cbTransform(color, cbMode);

  if (nvGain > 0.0)
  {
    highp float lumi = (0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b);
    if (lumi < 0.5)
      color *= atanh((1.0 - lumi) * 2.0 - 1.0) + 1.0;
    color *= nvGain;
  }

  color.a = 1.0;
}
