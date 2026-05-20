#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Vertex
{
  float x, y;
  float r, g, b;
}
Vertex;

static HWND hwnd;
static bool running = true;
static bool fullscreen = false;
static RECT windowRect;
static DWORD windowStyle;
static int winW = 1920;
static int winH = 1080;

#define REF_W 3840
#define REF_H 2160

static ID3D11Device * dev;
static ID3D11DeviceContext * ctx;
static IDXGISwapChain * swap;
static ID3D11RenderTargetView * rtv;
static ID3D11VertexShader * vs;
static ID3D11PixelShader * ps;
static ID3D11InputLayout * layout;
static ID3D11Buffer * vb;

static const char shaderSrc[] =
  "struct VSIn { float2 pos : POSITION; float3 color : COLOR0; };"
  "struct VSOut { float4 pos : SV_POSITION; float3 color : COLOR0; };"
  "VSOut vs_main(VSIn input) {"
  "  VSOut output;"
  "  output.pos = float4(input.pos, 0.0, 1.0);"
  "  output.color = input.color;"
  "  return output;"
  "}"
  "float4 ps_main(VSOut input) : SV_Target {"
  "  return float4(input.color, 1.0);"
  "}";

static void die(const char * msg, HRESULT hr)
{
  fprintf(stderr, "%s failed: 0x%08lx\n", msg, (unsigned long)hr);
  ExitProcess(1);
}

static float sx(float x)
{
  return (x / (float)winW) * 2.0f - 1.0f;
}

static float sy(float y)
{
  return 1.0f - (y / (float)winH) * 2.0f;
}

static void drawQuad(float x, float y, float w, float h,
  float r0, float g0, float b0, float r1, float g1, float b1)
{
  Vertex v[6] =
  {
    { sx(x    ), sy(y    ), r0, g0, b0 },
    { sx(x + w), sy(y    ), r1, g1, b1 },
    { sx(x    ), sy(y + h), r0, g0, b0 },
    { sx(x + w), sy(y    ), r1, g1, b1 },
    { sx(x + w), sy(y + h), r1, g1, b1 },
    { sx(x    ), sy(y + h), r0, g0, b0 }
  };

  D3D11_MAPPED_SUBRESOURCE map;
  HRESULT hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vb, 0,
    D3D11_MAP_WRITE_DISCARD, 0, &map);
  if (FAILED(hr))
    die("Map vertex buffer", hr);
  memcpy(map.pData, v, sizeof(v));
  ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vb, 0);
  ID3D11DeviceContext_Draw(ctx, 6, 0);
}

static void drawQuad4(float x, float y, float w, float h,
  float rtl, float gtl, float btl,
  float rtr, float gtr, float btr,
  float rbl, float gbl, float bbl,
  float rbr, float gbr, float bbr)
{
  Vertex v[6] =
  {
    { sx(x    ), sy(y    ), rtl, gtl, btl },
    { sx(x + w), sy(y    ), rtr, gtr, btr },
    { sx(x    ), sy(y + h), rbl, gbl, bbl },
    { sx(x + w), sy(y    ), rtr, gtr, btr },
    { sx(x + w), sy(y + h), rbr, gbr, bbr },
    { sx(x    ), sy(y + h), rbl, gbl, bbl }
  };

  D3D11_MAPPED_SUBRESOURCE map;
  HRESULT hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vb, 0,
    D3D11_MAP_WRITE_DISCARD, 0, &map);
  if (FAILED(hr))
    die("Map vertex buffer", hr);
  memcpy(map.pData, v, sizeof(v));
  ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vb, 0);
  ID3D11DeviceContext_Draw(ctx, 6, 0);
}

static void drawSolid(float x, float y, float w, float h,
  float r, float g, float b)
{
  drawQuad(x, y, w, h, r, g, b, r, g, b);
}

static const uint8_t * glyphRows(char ch)
{
  static const uint8_t blank[7] = { 0, 0, 0, 0, 0, 0, 0 };
  static const uint8_t glyphs[][7] =
  {
    ['0'] = { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e },
    ['1'] = { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e },
    ['2'] = { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f },
    ['3'] = { 0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e },
    ['4'] = { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 },
    ['5'] = { 0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e },
    ['6'] = { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e },
    ['7'] = { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    ['8'] = { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e },
    ['9'] = { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c },
    ['A'] = { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },
    ['B'] = { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e },
    ['C'] = { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e },
    ['D'] = { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e },
    ['E'] = { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f },
    ['F'] = { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 },
    ['G'] = { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f },
    ['H'] = { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },
    ['I'] = { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e },
    ['L'] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f },
    ['M'] = { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 },
    ['N'] = { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
    ['O'] = { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },
    ['P'] = { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 },
    ['Q'] = { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d },
    ['R'] = { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 },
    ['S'] = { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e },
    ['T'] = { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    ['U'] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },
    ['V'] = { 0x11, 0x11, 0x11, 0x11, 0x0a, 0x0a, 0x04 },
    ['W'] = { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a },
    ['X'] = { 0x11, 0x0a, 0x04, 0x04, 0x04, 0x0a, 0x11 },
    ['Y'] = { 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04, 0x04 },
    ['.'] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c },
    ['-'] = { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 },
    ['/'] = { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 },
    [':'] = { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 },
    ['='] = { 0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00 },
    ['+'] = { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 },
  };

  if (ch >= 'a' && ch <= 'z')
    ch = (char)(ch - 'a' + 'A');
  if ((unsigned char)ch >= sizeof(glyphs) / sizeof(glyphs[0]))
    return blank;
  if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
      ch == '.' || ch == '-' || ch == '/' || ch == ':' ||
      ch == '=' || ch == '+')
    return glyphs[(unsigned char)ch];
  return blank;
}

static float textWidth(const char * text, float scale)
{
  return (float)strlen(text) * 6.0f * scale;
}

static void drawText(float x, float y, float scale, const char * text,
  float r, float g, float b)
{
  for(const char * p = text; *p; ++p)
  {
    const uint8_t * rows = glyphRows(*p);
    for(int gy = 0; gy < 7; ++gy)
      for(int gx = 0; gx < 5; ++gx)
        if (rows[gy] & (1 << (4 - gx)))
          drawSolid(x + gx * scale, y + gy * scale,
            scale, scale, r, g, b);
    x += 6.0f * scale;
  }
}

static void drawLabel(float x, float y, float scale, const char * text)
{
  const float pad = scale * 2.0f;
  drawSolid(x - pad, y - pad,
    textWidth(text, scale) + pad * 2.0f, scale * 7.0f + pad * 2.0f,
    0.0f, 0.0f, 0.0f);
  drawText(x, y, scale, text, 0.92f, 0.92f, 0.92f);
}

static void drawCenteredLabel(float x, float y, float w, float scale,
  const char * text)
{
  drawLabel(x + (w - textWidth(text, scale)) * 0.5f, y, scale, text);
}

static void drawLabel3(float x, float y, float scale,
  const char * line0, const char * line1, const char * line2)
{
  const float pad = scale * 2.0f;
  const float lineH = scale * 9.0f;
  const float w = max(textWidth(line0, scale),
    max(textWidth(line1, scale), textWidth(line2, scale)));
  drawSolid(x - pad, y - pad, w + pad * 2.0f,
    lineH * 3.0f - scale * 2.0f + pad * 2.0f, 0.0f, 0.0f, 0.0f);
  drawText(x, y, scale, line0, 0.92f, 0.92f, 0.92f);
  drawText(x, y + lineH, scale, line1, 0.92f, 0.92f, 0.92f);
  drawText(x, y + lineH * 2.0f, scale, line2, 0.92f, 0.92f, 0.92f);
}

static void drawMarker(float x, float y, float h, const char * label)
{
  drawSolid(x - 2.0f, y - 8.0f, 4.0f, h + 16.0f, 32.0f, 32.0f, 32.0f);
  drawSolid(x - 8.0f, y - 8.0f, 16.0f, 4.0f, 32.0f, 32.0f, 32.0f);
  drawSolid(x - 8.0f, y + h + 4.0f, 16.0f, 4.0f, 32.0f, 32.0f, 32.0f);
  drawLabel(x + 8.0f, y + h + 8.0f, 2.0f, label);
}

static void drawRampMarks(float x, float y, float w, float h, float maxValue,
  float textScale, bool includeSdrMark)
{
  const int divisions = maxValue <= 1.0f ? 10 : (int)maxValue;
  for(int i = 0; i <= divisions; ++i)
  {
    const float value = maxValue * (float)i / (float)divisions;
    const float px = x + (value / maxValue) * w;
    const bool major = maxValue <= 1.0f ? (i % 5) == 0 : (i % 4) == 0;
    const float tickH = major ? 26.0f : 12.0f;
    drawSolid(px - 1.0f, y, 2.0f, tickH, 16.0f, 16.0f, 16.0f);
    drawSolid(px - 1.0f, y + h - tickH, 2.0f, tickH, 16.0f, 16.0f, 16.0f);
  }

  const struct
  {
    float value;
    const char * scrgb;
    const char * nits;
    const char * ev;
  } marks[] =
  {
    { 0.0f,  "scRGB 0",    "0N",     "EV -"    },
    { 0.18f, "scRGB 0.18", "14N",    "EV 6.8"  },
    { 0.5f,  "scRGB 0.5",  "40N",    "EV 8.3"  },
    { 1.0f,  "scRGB 1",    "80N",    "EV 9.3"  },
    { 2.0f,  "scRGB 2",    "160N",   "EV 10.3" },
    { 4.0f,  "scRGB 4",    "320N",   "EV 11.3" },
    { 8.0f,  "scRGB 8",    "640N",   "EV 12.3" },
    { 16.0f, "scRGB 16",   "1280N",  "EV 13.3" },
    { 32.0f, "scRGB 32",   "2560N",  "EV 14.3" }
  };

  for(unsigned i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i)
  {
    if (marks[i].value > maxValue)
      continue;
    const float px = x + (marks[i].value / maxValue) * w;
    const float labelW = max(textWidth(marks[i].scrgb, textScale),
      max(textWidth(marks[i].nits, textScale),
          textWidth(marks[i].ev, textScale)));
    const float labelX = max(x + 4.0f, min(px - labelW * 0.5f,
      x + w - labelW - 4.0f));
    drawSolid(px - 1.0f, y + h - 18.0f, 2.0f, 18.0f, 16.0f, 16.0f, 16.0f);
    drawLabel3(labelX, y + h + 26.0f + (i & 1 ? 34.0f : 0.0f),
      textScale, marks[i].scrgb, marks[i].nits, marks[i].ev);
  }

  if (includeSdrMark && maxValue > 1.0f)
    drawMarker(x + w / maxValue, y, h, "SDR MAX 1.0 / 80N / EV9.3");
}

static void drawMaxStripe(float x, float y, float w, float h,
  float r, float g, float b)
{
  const float stripeH = max(6.0f, h * 0.16f);
  const float stripeY = y + h * 0.5f - stripeH * 0.5f;
  drawSolid(x, stripeY, w, stripeH, r, g, b);
  drawLabel(x + 8.0f, stripeY - 28.0f, 1.7f, "MAX STRIPE");
}

static void drawColorSpaceSquare(float x, float y, float size,
  const char * label,
  float rtl, float gtl, float btl,
  float rtr, float gtr, float btr,
  float rbl, float gbl, float bbl,
  float rbr, float gbr, float bbr)
{
  drawQuad4(x, y, size, size,
    rtl, gtl, btl, rtr, gtr, btr, rbl, gbl, bbl, rbr, gbr, bbr);
  drawLabel(x + 8.0f, y + 8.0f, 1.8f, label);
}

typedef struct CpuPattern
{
  float * pixels;
  int width;
  int height;
}
CpuPattern;

static int iRound(float v)
{
  return (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

static bool cpuInit(CpuPattern * p, int width, int height)
{
  p->width = width;
  p->height = height;
  p->pixels = calloc((size_t)width * (size_t)height * 3, sizeof(*p->pixels));
  return p->pixels != NULL;
}

static void cpuFree(CpuPattern * p)
{
  free(p->pixels);
  p->pixels = NULL;
}

static void cpuRect(CpuPattern * p, float x, float y, float w, float h,
  float r, float g, float b)
{
  int x0 = max(0, iRound(x));
  int y0 = max(0, iRound(y));
  int x1 = min(p->width, iRound(x + w));
  int y1 = min(p->height, iRound(y + h));
  for(int yy = y0; yy < y1; ++yy)
  {
    float * row = p->pixels + ((size_t)yy * (size_t)p->width + x0) * 3;
    for(int xx = x0; xx < x1; ++xx)
    {
      row[0] = r;
      row[1] = g;
      row[2] = b;
      row += 3;
    }
  }
}

static void cpuHGrad(CpuPattern * p, float x, float y, float w, float h,
  float r0, float g0, float b0, float r1, float g1, float b1)
{
  int x0 = max(0, iRound(x));
  int y0 = max(0, iRound(y));
  int x1 = min(p->width, iRound(x + w));
  int y1 = min(p->height, iRound(y + h));
  const int denom = max(1, x1 - x0 - 1);
  for(int yy = y0; yy < y1; ++yy)
  {
    float * row = p->pixels + ((size_t)yy * (size_t)p->width + x0) * 3;
    for(int xx = x0; xx < x1; ++xx)
    {
      const float t = (float)(xx - x0) / (float)denom;
      row[0] = r0 * (1.0f - t) + r1 * t;
      row[1] = g0 * (1.0f - t) + g1 * t;
      row[2] = b0 * (1.0f - t) + b1 * t;
      row += 3;
    }
  }
}

static void cpuQuad4(CpuPattern * p, float x, float y, float w, float h,
  float rtl, float gtl, float btl,
  float rtr, float gtr, float btr,
  float rbl, float gbl, float bbl,
  float rbr, float gbr, float bbr)
{
  int x0 = max(0, iRound(x));
  int y0 = max(0, iRound(y));
  int x1 = min(p->width, iRound(x + w));
  int y1 = min(p->height, iRound(y + h));
  const int denomX = max(1, x1 - x0 - 1);
  const int denomY = max(1, y1 - y0 - 1);
  for(int yy = y0; yy < y1; ++yy)
  {
    const float ty = (float)(yy - y0) / (float)denomY;
    float * row = p->pixels + ((size_t)yy * (size_t)p->width + x0) * 3;
    for(int xx = x0; xx < x1; ++xx)
    {
      const float tx = (float)(xx - x0) / (float)denomX;
      const float rt = rtl * (1.0f - tx) + rtr * tx;
      const float gt = gtl * (1.0f - tx) + gtr * tx;
      const float bt = btl * (1.0f - tx) + btr * tx;
      const float rb = rbl * (1.0f - tx) + rbr * tx;
      const float gb = gbl * (1.0f - tx) + gbr * tx;
      const float bb = bbl * (1.0f - tx) + bbr * tx;
      row[0] = rt * (1.0f - ty) + rb * ty;
      row[1] = gt * (1.0f - ty) + gb * ty;
      row[2] = bt * (1.0f - ty) + bb * ty;
      row += 3;
    }
  }
}

static void cpuText(CpuPattern * p, float x, float y, float scale,
  const char * text, float r, float g, float b)
{
  int ix = iRound(x);
  const int iy = iRound(y);
  const int is = max(1, iRound(scale));
  for(const char * c = text; *c; ++c)
  {
    const uint8_t * rows = glyphRows(*c);
    for(int gy = 0; gy < 7; ++gy)
      for(int gx = 0; gx < 5; ++gx)
        if (rows[gy] & (1 << (4 - gx)))
          cpuRect(p, ix + gx * is, iy + gy * is, is, is, r, g, b);
    ix += 6 * is;
  }
}

static void cpuLabel(CpuPattern * p, float x, float y, float scale,
  const char * text)
{
  const float pad = scale * 2.0f;
  cpuRect(p, x - pad, y - pad,
    textWidth(text, scale) + pad * 2.0f, scale * 7.0f + pad * 2.0f,
    0.0f, 0.0f, 0.0f);
  cpuText(p, x, y, scale, text, 0.92f, 0.92f, 0.92f);
}

static void cpuCenteredLabel(CpuPattern * p, float x, float y, float w,
  float scale, const char * text)
{
  cpuLabel(p, x + (w - textWidth(text, scale)) * 0.5f, y, scale, text);
}

static void cpuLabel3(CpuPattern * p, float x, float y, float scale,
  const char * line0, const char * line1, const char * line2)
{
  const float pad = scale * 2.0f;
  const float lineH = scale * 9.0f;
  const float w = max(textWidth(line0, scale),
    max(textWidth(line1, scale), textWidth(line2, scale)));
  cpuRect(p, x - pad, y - pad, w + pad * 2.0f,
    lineH * 3.0f - scale * 2.0f + pad * 2.0f, 0.0f, 0.0f, 0.0f);
  cpuText(p, x, y, scale, line0, 0.92f, 0.92f, 0.92f);
  cpuText(p, x, y + lineH, scale, line1, 0.92f, 0.92f, 0.92f);
  cpuText(p, x, y + lineH * 2.0f, scale, line2, 0.92f, 0.92f, 0.92f);
}

static void cpuMarker(CpuPattern * p, float x, float y, float h,
  const char * label)
{
  cpuRect(p, x - 2.0f, y - 8.0f, 4.0f, h + 16.0f, 32.0f, 32.0f, 32.0f);
  cpuRect(p, x - 8.0f, y - 8.0f, 16.0f, 4.0f, 32.0f, 32.0f, 32.0f);
  cpuRect(p, x - 8.0f, y + h + 4.0f, 16.0f, 4.0f, 32.0f, 32.0f, 32.0f);
  cpuLabel(p, x + 8.0f, y + h + 8.0f, 2.0f, label);
}

static void cpuRampMarks(CpuPattern * p, float x, float y, float w, float h,
  float maxValue, float textScale, bool includeSdrMark)
{
  const int divisions = maxValue <= 1.0f ? 10 : (int)maxValue;
  for(int i = 0; i <= divisions; ++i)
  {
    const float value = maxValue * (float)i / (float)divisions;
    const float px = x + (value / maxValue) * w;
    const bool major = maxValue <= 1.0f ? (i % 5) == 0 : (i % 4) == 0;
    const float tickH = major ? 26.0f : 12.0f;
    cpuRect(p, px - 1.0f, y, 2.0f, tickH, 16.0f, 16.0f, 16.0f);
    cpuRect(p, px - 1.0f, y + h - tickH, 2.0f, tickH, 16.0f, 16.0f, 16.0f);
  }

  const struct
  {
    float value;
    const char * scrgb;
    const char * nits;
    const char * ev;
  } marks[] =
  {
    { 0.0f,  "scRGB 0",    "0N",     "EV -"    },
    { 0.18f, "scRGB 0.18", "14N",    "EV 6.8"  },
    { 0.5f,  "scRGB 0.5",  "40N",    "EV 8.3"  },
    { 1.0f,  "scRGB 1",    "80N",    "EV 9.3"  },
    { 2.0f,  "scRGB 2",    "160N",   "EV 10.3" },
    { 4.0f,  "scRGB 4",    "320N",   "EV 11.3" },
    { 8.0f,  "scRGB 8",    "640N",   "EV 12.3" },
    { 16.0f, "scRGB 16",   "1280N",  "EV 13.3" },
    { 32.0f, "scRGB 32",   "2560N",  "EV 14.3" }
  };

  for(unsigned i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i)
  {
    if (marks[i].value > maxValue)
      continue;
    const float px = x + (marks[i].value / maxValue) * w;
    const float labelW = max(textWidth(marks[i].scrgb, textScale),
      max(textWidth(marks[i].nits, textScale),
          textWidth(marks[i].ev, textScale)));
    const float labelX = max(x + 4.0f, min(px - labelW * 0.5f,
      x + w - labelW - 4.0f));
    cpuRect(p, px - 1.0f, y + h - 18.0f, 2.0f, 18.0f, 16.0f, 16.0f, 16.0f);
    cpuLabel3(p, labelX, y + h + 26.0f + (i & 1 ? 34.0f : 0.0f),
      textScale, marks[i].scrgb, marks[i].nits, marks[i].ev);
  }

  if (includeSdrMark && maxValue > 1.0f)
    cpuMarker(p, x + w / maxValue, y, h, "SDR MAX 1.0 / 80N / EV9.3");
}

static void cpuMaxStripe(CpuPattern * p, float x, float y, float w, float h,
  float r, float g, float b)
{
  const float stripeH = max(6.0f, h * 0.16f);
  const float stripeY = y + h * 0.5f - stripeH * 0.5f;
  cpuRect(p, x, stripeY, w, stripeH, r, g, b);
  cpuLabel(p, x + 8.0f, stripeY - 28.0f, 1.7f, "MAX STRIPE");
}

static void cpuColorSpaceSquare(CpuPattern * p, float x, float y, float size,
  const char * label,
  float rtl, float gtl, float btl,
  float rtr, float gtr, float btr,
  float rbl, float gbl, float bbl,
  float rbr, float gbr, float bbr)
{
  cpuQuad4(p, x, y, size, size,
    rtl, gtl, btl, rtr, gtr, btr, rbl, gbl, bbl, rbr, gbr, bbr);
  cpuLabel(p, x + 8.0f, y + 8.0f, 1.8f, label);
}

static void cpuRender(CpuPattern * p)
{
  cpuRect(p, 0, 0, (float)p->width, (float)p->height, 0.015f, 0.015f, 0.015f);
  const float margin = 48.0f;
  const float width = (float)p->width - margin * 2.0f;

  cpuHGrad(p, margin, 55, width, 70, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
  cpuMaxStripe(p, margin, 55, width, 70, 1.0f, 1.0f, 1.0f);
  cpuLabel(p, margin, 24, 2.6f, "GRAYSCALE SDR RAMP: scRGB 0.0-1.0 / 0-80 NITS");
  cpuMarker(p, margin + width, 55, 70, "SDR MAX");

  cpuHGrad(p, margin, 170, width, 70, 0.0f, 0.0f, 0.0f, 32.0f, 32.0f, 32.0f);
  cpuMaxStripe(p, margin, 170, width, 70, 32.0f, 32.0f, 32.0f);
  cpuLabel(p, margin, 139, 2.6f, "GRAYSCALE HDR RAMP: scRGB 0.0-32.0 / 0-2560 NITS");
  cpuRampMarks(p, margin, 170, width, 70, 32.0f, 1.8f, true);

  const float colorRampY = 305.0f;
  const float colorRampH = 54.0f;
  const float colorRampGap = 145.0f;
  cpuLabel(p, margin, colorRampY - 34.0f, 2.6f,
    "COLOUR HDR RAMPS: EACH CHANNEL scRGB 0.0-32.0, SDR MAX MARKED AT 1.0");
  cpuHGrad(p, margin, colorRampY, width, colorRampH, 0.0f, 0.0f, 0.0f, 32.0f, 0.0f, 0.0f);
  cpuMaxStripe(p, margin, colorRampY, width, colorRampH, 32.0f, 0.0f, 0.0f);
  cpuLabel(p, margin + 8.0f, colorRampY + 14.0f, 2.0f, "RED");
  cpuRampMarks(p, margin, colorRampY, width, colorRampH, 32.0f, 1.3f, true);
  cpuHGrad(p, margin, colorRampY + colorRampGap, width, colorRampH, 0.0f, 0.0f, 0.0f, 0.0f, 32.0f, 0.0f);
  cpuMaxStripe(p, margin, colorRampY + colorRampGap, width, colorRampH, 0.0f, 32.0f, 0.0f);
  cpuLabel(p, margin + 8.0f, colorRampY + colorRampGap + 14.0f, 2.0f, "GREEN");
  cpuRampMarks(p, margin, colorRampY + colorRampGap, width, colorRampH, 32.0f, 1.3f, true);
  cpuHGrad(p, margin, colorRampY + colorRampGap * 2.0f, width, colorRampH, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 32.0f);
  cpuMaxStripe(p, margin, colorRampY + colorRampGap * 2.0f, width, colorRampH, 0.0f, 0.0f, 32.0f);
  cpuLabel(p, margin + 8.0f, colorRampY + colorRampGap * 2.0f + 14.0f, 2.0f, "BLUE");
  cpuRampMarks(p, margin, colorRampY + colorRampGap * 2.0f, width, colorRampH, 32.0f, 1.3f, true);

  const float vals[] = { 0.18f, 0.5f, 1.0f, 1.5f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
  const char * valLabels[] = { "0.18", "0.5", "1.0", "1.5", "2.0", "4.0", "8.0", "16.0", "32.0" };
  const char * nitLabels[] = { "14 NIT", "40 NIT", "80 NIT", "120 NIT", "160 NIT", "320 NIT", "640 NIT", "1280 NIT", "2560 NIT" };
  const int count = (int)(sizeof(vals) / sizeof(vals[0]));
  const float gap = 12.0f;
  const float patchW = (width - gap * (count - 1)) / count;
  for(int i = 0; i < count; ++i)
  {
    const float x = margin + i * (patchW + gap);
    cpuRect(p, x, 780, patchW, 125, vals[i], vals[i], vals[i]);
    cpuCenteredLabel(p, x, 788, patchW, 2.0f, valLabels[i]);
    cpuCenteredLabel(p, x, 873, patchW, 2.0f, nitLabels[i]);
  }
  for(int i = 0; i < count; ++i)
  {
    const float base = (i & 1) ? 0.18f : 0.5f;
    const float x = margin + i * (patchW + gap);
    cpuRect(p, x, 940, patchW, 125, base, base, base);
    cpuRect(p, x + patchW * 0.34f, 973, patchW * 0.32f, 58,
      vals[i], vals[i], vals[i]);
    cpuCenteredLabel(p, x, 947, patchW, 2.0f,
      (i & 1) ? "BASE 0.18" : "BASE 0.5");
    cpuCenteredLabel(p, x, 1034, patchW, 2.0f, valLabels[i]);
  }

  const float colorY = 1130.0f;
  const float colorW = (width - gap * 5.0f) / 6.0f;
  const float patches[][3] =
  {
    { 4.0f, 0.0f, 0.0f }, { 0.0f, 4.0f, 0.0f }, { 0.0f, 0.0f, 4.0f },
    { 8.0f, 4.0f, 0.0f }, { 0.0f, 8.0f, 8.0f }, { 8.0f, 8.0f, 8.0f }
  };
  const char * patchLabels[] = { "R 4.0", "G 4.0", "B 4.0", "R8 G4", "G8 B8", "W 8.0" };
  for(int i = 0; i < 6; ++i)
  {
    const float x = margin + i * (colorW + gap);
    cpuRect(p, x, colorY, colorW, 115, patches[i][0], patches[i][1], patches[i][2]);
    cpuCenteredLabel(p, x, colorY + 88, colorW, 2.0f, patchLabels[i]);
  }

  const float squareMax = 4.0f;
  const float squareY = 1310.0f;
  const float squareGap = 24.0f;
  const float square = (width - squareGap * 3.0f) / 4.0f;
  cpuLabel(p, margin, squareY - 42.0f, 2.6f,
    "COLOUR SPACE SQUARES: 2D CHANNEL MIXES, AXES 0.0-4.0 scRGB");
  cpuColorSpaceSquare(p, margin + (square + squareGap) * 0.0f, squareY, square,
    "R/G  B0", 0.0f, 0.0f, 0.0f, squareMax, 0.0f, 0.0f,
    0.0f, squareMax, 0.0f, squareMax, squareMax, 0.0f);
  cpuColorSpaceSquare(p, margin + (square + squareGap) * 1.0f, squareY, square,
    "R/B  G0", 0.0f, 0.0f, 0.0f, squareMax, 0.0f, 0.0f,
    0.0f, 0.0f, squareMax, squareMax, 0.0f, squareMax);
  cpuColorSpaceSquare(p, margin + (square + squareGap) * 2.0f, squareY, square,
    "G/B  R0", 0.0f, 0.0f, 0.0f, 0.0f, squareMax, 0.0f,
    0.0f, 0.0f, squareMax, 0.0f, squareMax, squareMax);
  cpuColorSpaceSquare(p, margin + (square + squareGap) * 3.0f, squareY, square,
    "R/G  B1", 0.0f, 0.0f, 1.0f, squareMax, 0.0f, 1.0f,
    0.0f, squareMax, 1.0f, squareMax, squareMax, 1.0f);
}

static bool saveReference(const char * path)
{
  CpuPattern p;
  if (!cpuInit(&p, REF_W, REF_H))
    return false;
  cpuRender(&p);
  FILE * f = fopen(path, "wb");
  if (!f)
  {
    cpuFree(&p);
    return false;
  }
  const bool ok = fwrite(p.pixels, sizeof(float),
      (size_t)p.width * (size_t)p.height * 3, f) ==
    (size_t)p.width * (size_t)p.height * 3;
  fclose(f);
  cpuFree(&p);
  return ok;
}

static void render(void)
{
  float bg[4] = { 0.015f, 0.015f, 0.015f, 1.0f };
  ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, bg);
  ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);

  D3D11_VIEWPORT vp =
  {
    .TopLeftX = 0.0f,
    .TopLeftY = 0.0f,
    .Width    = (FLOAT)winW,
    .Height   = (FLOAT)winH,
    .MinDepth = 0.0f,
    .MaxDepth = 1.0f
  };
  ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
  ID3D11DeviceContext_IASetInputLayout(ctx, layout);
  ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  UINT stride = sizeof(Vertex), offset = 0;
  ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
  ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
  ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

  const float margin = 48.0f;
  const float width = (float)winW - margin * 2.0f;

  drawQuad(margin, 55, width, 70, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
  drawMaxStripe(margin, 55, width, 70, 1.0f, 1.0f, 1.0f);
  drawLabel(margin, 24, 2.6f, "GRAYSCALE SDR RAMP: scRGB 0.0-1.0 / 0-80 NITS");
  drawMarker(margin + width, 55, 70, "SDR MAX");

  drawQuad(margin, 170, width, 70,
    0.0f, 0.0f, 0.0f, 32.0f, 32.0f, 32.0f);
  drawMaxStripe(margin, 170, width, 70, 32.0f, 32.0f, 32.0f);
  drawLabel(margin, 139, 2.6f, "GRAYSCALE HDR RAMP: scRGB 0.0-32.0 / 0-2560 NITS");
  drawRampMarks(margin, 170, width, 70, 32.0f, 1.8f, true);

  const float colorRampY = 305.0f;
  const float colorRampH = 54.0f;
  const float colorRampGap = 145.0f;
  drawLabel(margin, colorRampY - 34.0f, 2.6f,
    "COLOUR HDR RAMPS: EACH CHANNEL scRGB 0.0-32.0, SDR MAX MARKED AT 1.0");

  drawQuad(margin, colorRampY + colorRampGap * 0, width, colorRampH,
    0.0f, 0.0f, 0.0f, 32.0f, 0.0f, 0.0f);
  drawMaxStripe(margin, colorRampY + colorRampGap * 0, width, colorRampH,
    32.0f, 0.0f, 0.0f);
  drawLabel(margin + 8.0f, colorRampY + colorRampGap * 0 + 14.0f, 2.0f,
    "RED");
  drawRampMarks(margin, colorRampY + colorRampGap * 0, width, colorRampH,
    32.0f, 1.3f, true);

  drawQuad(margin, colorRampY + colorRampGap * 1, width, colorRampH,
    0.0f, 0.0f, 0.0f, 0.0f, 32.0f, 0.0f);
  drawMaxStripe(margin, colorRampY + colorRampGap * 1, width, colorRampH,
    0.0f, 32.0f, 0.0f);
  drawLabel(margin + 8.0f, colorRampY + colorRampGap * 1 + 14.0f, 2.0f,
    "GREEN");
  drawRampMarks(margin, colorRampY + colorRampGap * 1, width, colorRampH,
    32.0f, 1.3f, true);

  drawQuad(margin, colorRampY + colorRampGap * 2, width, colorRampH,
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 32.0f);
  drawMaxStripe(margin, colorRampY + colorRampGap * 2, width, colorRampH,
    0.0f, 0.0f, 32.0f);
  drawLabel(margin + 8.0f, colorRampY + colorRampGap * 2 + 14.0f, 2.0f,
    "BLUE");
  drawRampMarks(margin, colorRampY + colorRampGap * 2, width, colorRampH,
    32.0f, 1.3f, true);

  const float vals[] = { 0.18f, 0.5f, 1.0f, 1.5f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
  const int count = (int)(sizeof(vals) / sizeof(vals[0]));
  const char * valLabels[] =
  {
    "0.18", "0.5", "1.0", "1.5", "2.0", "4.0", "8.0", "16.0", "32.0"
  };
  const char * nitLabels[] =
  {
    "14 NIT", "40 NIT", "80 NIT", "120 NIT", "160 NIT",
    "320 NIT", "640 NIT", "1280 NIT", "2560 NIT"
  };
  const float gap = 12.0f;
  const float patchW = (width - gap * (count - 1)) / count;
  for(int i = 0; i < count; ++i)
  {
    drawSolid(margin + i * (patchW + gap), 780, patchW, 125,
      vals[i], vals[i], vals[i]);
    drawCenteredLabel(margin + i * (patchW + gap), 788, patchW, 2.0f,
      valLabels[i]);
    drawCenteredLabel(margin + i * (patchW + gap), 873, patchW, 2.0f,
      nitLabels[i]);
  }

  for(int i = 0; i < count; ++i)
  {
    float base = (i & 1) ? 0.18f : 0.5f;
    float hi = vals[i];
    float x = margin + i * (patchW + gap);
    drawSolid(x, 940, patchW, 125, base, base, base);
    drawSolid(x + patchW * 0.34f, 973, patchW * 0.32f, 58, hi, hi, hi);
    drawCenteredLabel(x, 947, patchW, 2.0f,
      (i & 1) ? "BASE 0.18" : "BASE 0.5");
    drawCenteredLabel(x, 1034, patchW, 2.0f, valLabels[i]);
  }

  const float colorY = 1130.0f;
  const float colorW = (width - gap * 5.0f) / 6.0f;
  drawSolid(margin + 0 * (colorW + gap), colorY, colorW, 115, 4.0f, 0.0f, 0.0f);
  drawSolid(margin + 1 * (colorW + gap), colorY, colorW, 115, 0.0f, 4.0f, 0.0f);
  drawSolid(margin + 2 * (colorW + gap), colorY, colorW, 115, 0.0f, 0.0f, 4.0f);
  drawSolid(margin + 3 * (colorW + gap), colorY, colorW, 115, 8.0f, 4.0f, 0.0f);
  drawSolid(margin + 4 * (colorW + gap), colorY, colorW, 115, 0.0f, 8.0f, 8.0f);
  drawSolid(margin + 5 * (colorW + gap), colorY, colorW, 115, 8.0f, 8.0f, 8.0f);
  drawCenteredLabel(margin + 0 * (colorW + gap), colorY + 88, colorW, 2.0f, "R 4.0");
  drawCenteredLabel(margin + 1 * (colorW + gap), colorY + 88, colorW, 2.0f, "G 4.0");
  drawCenteredLabel(margin + 2 * (colorW + gap), colorY + 88, colorW, 2.0f, "B 4.0");
  drawCenteredLabel(margin + 3 * (colorW + gap), colorY + 88, colorW, 2.0f, "R8 G4");
  drawCenteredLabel(margin + 4 * (colorW + gap), colorY + 88, colorW, 2.0f, "G8 B8");
  drawCenteredLabel(margin + 5 * (colorW + gap), colorY + 88, colorW, 2.0f, "W 8.0");

  const float squareMax = 4.0f;
  const float squareY = 1310.0f;
  const float squareGap = 24.0f;
  const float square = (width - squareGap * 3.0f) / 4.0f;
  drawLabel(margin, squareY - 42.0f, 2.6f,
    "COLOUR SPACE SQUARES: 2D CHANNEL MIXES, AXES 0.0-4.0 scRGB");

  drawColorSpaceSquare(margin + (square + squareGap) * 0.0f, squareY, square,
    "R/G  B0", 0.0f, 0.0f, 0.0f, squareMax, 0.0f, 0.0f,
    0.0f, squareMax, 0.0f, squareMax, squareMax, 0.0f);
  drawColorSpaceSquare(margin + (square + squareGap) * 1.0f, squareY, square,
    "R/B  G0", 0.0f, 0.0f, 0.0f, squareMax, 0.0f, 0.0f,
    0.0f, 0.0f, squareMax, squareMax, 0.0f, squareMax);
  drawColorSpaceSquare(margin + (square + squareGap) * 2.0f, squareY, square,
    "G/B  R0", 0.0f, 0.0f, 0.0f, 0.0f, squareMax, 0.0f,
    0.0f, 0.0f, squareMax, 0.0f, squareMax, squareMax);
  drawColorSpaceSquare(margin + (square + squareGap) * 3.0f, squareY, square,
    "R/G  B1", 0.0f, 0.0f, 1.0f, squareMax, 0.0f, 1.0f,
    0.0f, squareMax, 1.0f, squareMax, squareMax, 1.0f);

  IDXGISwapChain_Present(swap, 1, 0);
}

static void resize(UINT w, UINT h)
{
  if (!swap || w == 0 || h == 0)
    return;

  winW = (int)w;
  winH = (int)h;

  if (rtv)
  {
    ID3D11RenderTargetView_Release(rtv);
    rtv = NULL;
  }

  ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
  HRESULT hr = IDXGISwapChain_ResizeBuffers(swap, 2, w, h,
    DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
  if (FAILED(hr))
    die("ResizeBuffers", hr);

  ID3D11Texture2D * back = NULL;
  hr = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back);
  if (FAILED(hr))
    die("GetBuffer", hr);
  hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back,
    NULL, &rtv);
  ID3D11Texture2D_Release(back);
  if (FAILED(hr))
    die("CreateRenderTargetView", hr);
}

static void toggleFullscreen(void)
{
  fullscreen = !fullscreen;
  if (fullscreen)
  {
    windowStyle = GetWindowLong(hwnd, GWL_STYLE);
    GetWindowRect(hwnd, &windowRect);
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLong(hwnd, GWL_STYLE, windowStyle & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(hwnd, HWND_TOP,
      mi.rcMonitor.left, mi.rcMonitor.top,
      mi.rcMonitor.right - mi.rcMonitor.left,
      mi.rcMonitor.bottom - mi.rcMonitor.top,
      SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  else
  {
    SetWindowLong(hwnd, GWL_STYLE, windowStyle);
    SetWindowPos(hwnd, NULL,
      windowRect.left, windowRect.top,
      windowRect.right - windowRect.left,
      windowRect.bottom - windowRect.top,
      SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
}

static LRESULT CALLBACK wndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch(msg)
  {
    case WM_DESTROY:
      running = false;
      PostQuitMessage(0);
      return 0;
    case WM_SIZE:
      resize(LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE)
        DestroyWindow(wnd);
      else if (wp == VK_F11)
        toggleFullscreen();
      return 0;
  }
  return DefWindowProc(wnd, msg, wp, lp);
}

static void initD3D(void)
{
  DXGI_SWAP_CHAIN_DESC sd =
  {
    .BufferDesc = {
      .Width = (UINT)winW,
      .Height = (UINT)winH,
      .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
      .RefreshRate = { .Numerator = 60, .Denominator = 1 }
    },
    .SampleDesc = { .Count = 1, .Quality = 0 },
    .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
    .BufferCount = 2,
    .OutputWindow = hwnd,
    .Windowed = TRUE,
    .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD
  };

  HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE,
    NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
    &sd, &swap, &dev, NULL, &ctx);
  if (FAILED(hr))
    die("D3D11CreateDeviceAndSwapChain", hr);

  IDXGISwapChain3 * swap3 = NULL;
  if (SUCCEEDED(IDXGISwapChain_QueryInterface(swap, &IID_IDXGISwapChain3,
      (void **)&swap3)))
  {
    IDXGISwapChain3_SetColorSpace1(swap3,
      DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    IDXGISwapChain3_Release(swap3);
  }

  resize((UINT)winW, (UINT)winH);

  ID3DBlob * vsBlob = NULL, * psBlob = NULL, * error = NULL;
  hr = D3DCompile(shaderSrc, strlen(shaderSrc), NULL, NULL, NULL,
    "vs_main", "vs_5_0", 0, 0, &vsBlob, &error);
  if (FAILED(hr))
    die(error ? (const char *)ID3D10Blob_GetBufferPointer(error) : "D3DCompile VS", hr);
  hr = D3DCompile(shaderSrc, strlen(shaderSrc), NULL, NULL, NULL,
    "ps_main", "ps_5_0", 0, 0, &psBlob, &error);
  if (FAILED(hr))
    die(error ? (const char *)ID3D10Blob_GetBufferPointer(error) : "D3DCompile PS", hr);

  hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsBlob),
    ID3D10Blob_GetBufferSize(vsBlob), NULL, &vs);
  if (FAILED(hr))
    die("CreateVertexShader", hr);
  hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psBlob),
    ID3D10Blob_GetBufferSize(psBlob), NULL, &ps);
  if (FAILED(hr))
    die("CreatePixelShader", hr);

  D3D11_INPUT_ELEMENT_DESC elems[] =
  {
    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
      D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8,
      D3D11_INPUT_PER_VERTEX_DATA, 0 }
  };
  hr = ID3D11Device_CreateInputLayout(dev, elems, 2,
    ID3D10Blob_GetBufferPointer(vsBlob), ID3D10Blob_GetBufferSize(vsBlob),
    &layout);
  if (FAILED(hr))
    die("CreateInputLayout", hr);
  ID3D10Blob_Release(vsBlob);
  ID3D10Blob_Release(psBlob);

  D3D11_BUFFER_DESC bd =
  {
    .ByteWidth = sizeof(Vertex) * 6,
    .Usage = D3D11_USAGE_DYNAMIC,
    .BindFlags = D3D11_BIND_VERTEX_BUFFER,
    .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
  };
  hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &vb);
  if (FAILED(hr))
    die("CreateBuffer", hr);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
  (void)prev;
  const char * saveRef = strstr(cmd, "--save-reference=");
  if (saveRef)
  {
    char path[MAX_PATH];
    saveRef += strlen("--save-reference=");
    size_t len = 0;
    while(saveRef[len] && saveRef[len] != ' ' && len + 1 < sizeof(path))
      ++len;
    memcpy(path, saveRef, len);
    path[len] = '\0';
    if (!saveReference(path))
      return 1;
    if (strstr(cmd, "--exit-after-save"))
      return 0;
  }

  if (strstr(cmd, "--fullscreen"))
    fullscreen = true;

  WNDCLASS wc =
  {
    .lpfnWndProc = wndProc,
    .hInstance = inst,
    .lpszClassName = "LGHDRPattern",
    .hCursor = LoadCursor(NULL, IDC_ARROW)
  };
  RegisterClass(&wc);

  RECT r = { 0, 0, winW, winH };
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  hwnd = CreateWindowEx(0, wc.lpszClassName, "Looking Glass HDR scRGB Pattern",
    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
    r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
  if (!hwnd)
    return 1;

  ShowWindow(hwnd, show);
  initD3D();
  if (fullscreen)
  {
    fullscreen = false;
    toggleFullscreen();
  }

  while(running)
  {
    MSG msg;
    while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    render();
  }

  return 0;
}
