/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "texture.h"

#include "texture_buffer.h"
#include "common/debug.h"
#include "common/KVMFR.h"
#include "common/rects.h"

struct TexDamage
{
  int             count;
  FrameDamageRect rects[KVMFR_MAX_DAMAGE_RECTS];
};

typedef struct TexFB
{
  TextureBuffer base;
  struct TexDamage damage[EGL_TEX_BUFFER_MAX];
  struct TexDamage uploadDamage[EGL_TEX_BUFFER_MAX];
}
TexFB;

static bool egl_texFBIsPackedYUV(const EGL_Texture * texture)
{
  return texture->format.pixFmt == EGL_PF_NV12 ||
         texture->format.pixFmt == EGL_PF_P010;
}

static int egl_texFBMapPackedYUVRects(const EGL_Texture * texture,
  const FrameDamageRect * src, int srcCount,
  FrameDamageRect * dst, int dstCapacity)
{
  if (srcCount <= 0)
    return 0;

  const int frameWidth  = (int)texture->format.width * 4;
  const int frameHeight = (int)(texture->format.height * 2 / 3);
  int dstCount = 0;

  for(int i = 0; i < srcCount; ++i)
  {
    int left   = clamp(src[i].x, 0, frameWidth);
    int top    = clamp(src[i].y, 0, frameHeight);
    int right  = clamp(src[i].x + src[i].width , 0, frameWidth);
    int bottom = clamp(src[i].y + src[i].height, 0, frameHeight);
    if (right <= left || bottom <= top)
      continue;

    if (dstCount + 2 > dstCapacity)
      return -1;

    const int yLeft  = left / 4;
    const int yRight = (right + 3) / 4;
    dst[dstCount++] = (FrameDamageRect)
    {
      .x      = yLeft,
      .y      = top,
      .width  = yRight - yLeft,
      .height = bottom - top,
    };

    const int pairStart = left / 2;
    const int pairEnd   = (right + 1) / 2;
    const int uvLeft    = (pairStart * 2) / 4;
    const int uvRight   = (pairEnd * 2 + 3) / 4;
    const int uvTop     = frameHeight + top / 2;
    const int uvBottom  = frameHeight + (bottom + 1) / 2;
    dst[dstCount++] = (FrameDamageRect)
    {
      .x      = uvLeft,
      .y      = uvTop,
      .width  = uvRight - uvLeft,
      .height = uvBottom - uvTop,
    };
  }

  return rectsMergeOverlapping(dst, dstCount);
}

static bool egl_texFBInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  TexFB * this = calloc(1, sizeof(*this));
  *texture = &this->base.base;

  EGL_Texture * parent = &this->base.base;
  if (!egl_texBufferStreamInit(&parent, type, display))
  {
    free(this);
    *texture = NULL;
    return false;
  }

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
  {
    this->damage[i].count = -1;
    this->uploadDamage[i].count = 0;
  }

  return true;
}

void egl_texFBFree(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexFB         * this   = UPCAST(TexFB        , parent );

  egl_texBufferFree(texture);
  free(this);
}

bool egl_texFBSetup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexFB         * this   = UPCAST(TexFB        , parent );

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
  {
    this->damage[i].count = -1;
    this->uploadDamage[i].count = 0;
  }

  return egl_texBufferStreamSetup(texture, setup);
}

static bool egl_texFBUpdate(EGL_Texture * texture, const EGL_TexUpdate * update)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexFB         * this   = UPCAST(TexFB        , parent );

  DEBUG_ASSERT(update->type == EGL_TEXTYPE_FRAMEBUFFER);

  LG_LOCK(parent->copyLock);

  FrameDamageRect mappedRects[KVMFR_MAX_DAMAGE_RECTS];
  const FrameDamageRect * updateRects = update->rects;
  int updateRectCount = update->rectCount;
  if (update->rects && update->rectCount > 0 && egl_texFBIsPackedYUV(texture))
  {
    updateRectCount = egl_texFBMapPackedYUVRects(texture,
      update->rects, update->rectCount, mappedRects, KVMFR_MAX_DAMAGE_RECTS);
    if (updateRectCount < 0)
    {
      updateRects = NULL;
      updateRectCount = 0;
    }
    else
      updateRects = mappedRects;
  }

  struct TexDamage * damage = this->damage + parent->bufIndex;
  bool damageAll = !updateRects || updateRectCount == 0 || damage->count < 0 ||
    damage->count + updateRectCount > KVMFR_MAX_DAMAGE_RECTS;

  struct TexDamage * upload = this->uploadDamage + parent->bufIndex;

  if (damageAll)
  {
     framebuffer_read(
      update->frame,
      parent->buf[parent->bufIndex].map,
      texture->format.pitch,
      texture->format.height,
      texture->format.width,
      texture->format.bpp,
      texture->format.pitch
    );

    upload->count = -1;
  }
  else
  {
    memcpy(damage->rects + damage->count, updateRects,
      updateRectCount * sizeof(FrameDamageRect));
    damage->count += updateRectCount;

    if (texture->format.pixFmt == EGL_PF_BGR_32)
    {
      FrameDamageRect scaledDamageRects[damage->count];
      for (int i = 0; i < damage->count; i++)
      {
        FrameDamageRect rect = damage->rects[i];
        int originalX = rect.x;
        int scaledX = originalX * 3 / 4;
        rect.x = scaledX;
        rect.width = (((originalX + rect.width) * 3 + 3) / 4) - scaledX;
        scaledDamageRects[i] = rect;
      }

      rectsFramebufferToBuffer(
        scaledDamageRects,
        damage->count,
        texture->format.bpp,
        parent->buf[parent->bufIndex].map,
        texture->format.pitch,
        texture->format.height,
        update->frame,
        texture->format.pitch
      );

      /* append to any damage still pending upload (the process call may
       * have deferred the upload while a prior sync was in flight) */
      if (upload->count < 0 ||
          upload->count + damage->count > KVMFR_MAX_DAMAGE_RECTS)
        upload->count = -1;
      else
      {
        memcpy(upload->rects + upload->count, scaledDamageRects,
          damage->count * sizeof(FrameDamageRect));
        upload->count += damage->count;
      }
    }
    else
    {
      rectsFramebufferToBuffer(
        damage->rects,
        damage->count,
        texture->format.bpp,
        parent->buf[parent->bufIndex].map,
        texture->format.pitch,
        texture->format.height,
        update->frame,
        texture->format.pitch
      );

      /* append to any damage still pending upload (the process call may
       * have deferred the upload while a prior sync was in flight) */
      if (upload->count < 0 ||
          upload->count + damage->count > KVMFR_MAX_DAMAGE_RECTS)
        upload->count = -1;
      else
      {
        memcpy(upload->rects + upload->count, damage->rects,
          damage->count * sizeof(FrameDamageRect));
        upload->count += damage->count;
      }
    }
  }

  parent->buf[parent->bufIndex].updated = true;

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
  {
    struct TexDamage * damage = this->damage + i;
    if (i == parent->bufIndex)
      damage->count = 0;
    else if (updateRects && updateRectCount > 0 && damage->count >= 0 &&
             damage->count + updateRectCount <= KVMFR_MAX_DAMAGE_RECTS)
    {
      memcpy(damage->rects + damage->count, updateRects,
        updateRectCount * sizeof(FrameDamageRect));
      damage->count += updateRectCount;
    }
    else
      damage->count = -1;
  }

  LG_UNLOCK(parent->copyLock);

  return true;
}

static EGL_TexStatus egl_texFBProcess(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexFB         * this   = UPCAST(TexFB        , parent );

  if (egl_texBufferPollSync(parent) == EGL_TEX_STATUS_ERROR)
    return EGL_TEX_STATUS_ERROR;

  LG_LOCK(parent->copyLock);

  /* If a prior upload is still in flight, do not issue a new one. Doing so
   * would orphan the old fence and prevent the swap from advancing, which
   * leaves rIndex stale and the consumer reading a stuck/older texture. */
  if (parent->sync != 0 || !parent->buf[parent->bufIndex].updated)
  {
    LG_UNLOCK(parent->copyLock);
    return EGL_TEX_STATUS_OK;
  }

  int             uploadIndex = parent->bufIndex;
  GLuint          tex         = parent->tex[parent->bufIndex];
  EGL_TexBuffer * buffer      = &parent->buf[parent->bufIndex];

  parent->rIndex = parent->bufIndex;
  if (++parent->bufIndex == parent->texCount)
    parent->bufIndex = 0;

  struct TexDamage upload = this->uploadDamage[uploadIndex];
  this->uploadDamage[uploadIndex].count = 0;
  buffer->updated = false;

  LG_UNLOCK(parent->copyLock);

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer->pbo);
  glBindTexture(GL_TEXTURE_2D, tex);

  glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->format.stride);

  if (upload.count <= 0)
  {
    glTexSubImage2D(GL_TEXTURE_2D,
        0, 0, 0,
        texture->format.width,
        texture->format.height,
        texture->format.format,
        texture->format.dataType,
        (const void *)0);
  }
  else
  {
    for (int i = 0; i < upload.count; ++i)
    {
      FrameDamageRect rect = upload.rects[i];
      glPixelStorei(GL_UNPACK_SKIP_PIXELS, rect.x);
      glPixelStorei(GL_UNPACK_SKIP_ROWS  , rect.y);
      glTexSubImage2D(GL_TEXTURE_2D,
          0,
          rect.x,
          rect.y,
          rect.width,
          rect.height,
          texture->format.format,
          texture->format.dataType,
          (const void *)0);
    }
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS  , 0);
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  parent->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

  return EGL_TEX_STATUS_OK;
}

EGL_TextureOps EGL_TextureFrameBuffer =
{
  .init    = egl_texFBInit,
  .free    = egl_texFBFree,
  .setup   = egl_texFBSetup,
  .update  = egl_texFBUpdate,
  .process = egl_texFBProcess,
  .get     = egl_texBufferStreamGet,
  .bind    = egl_texBufferBind
};
