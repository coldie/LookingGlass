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

  struct TexDamage * damage = this->damage + parent->bufIndex;
  bool damageAll = !update->rects || update->rectCount == 0 || damage->count < 0 ||
    damage->count + update->rectCount > KVMFR_MAX_DAMAGE_RECTS;

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
    memcpy(damage->rects + damage->count, update->rects,
      update->rectCount * sizeof(FrameDamageRect));
    damage->count += update->rectCount;

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

      if (damage->count > KVMFR_MAX_DAMAGE_RECTS)
        upload->count = -1;
      else
      {
        memcpy(upload->rects, scaledDamageRects,
          damage->count * sizeof(FrameDamageRect));
        upload->count = damage->count;
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

      if (damage->count > KVMFR_MAX_DAMAGE_RECTS)
        upload->count = -1;
      else
      {
        memcpy(upload->rects, damage->rects,
          damage->count * sizeof(FrameDamageRect));
        upload->count = damage->count;
      }
    }
  }

  parent->buf[parent->bufIndex].updated = true;

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
  {
    struct TexDamage * damage = this->damage + i;
    if (i == parent->bufIndex)
      damage->count = 0;
    else if (update->rects && update->rectCount > 0 && damage->count >= 0 &&
             damage->count + update->rectCount <= KVMFR_MAX_DAMAGE_RECTS)
    {
      memcpy(damage->rects + damage->count, update->rects,
        update->rectCount * sizeof(FrameDamageRect));
      damage->count += update->rectCount;
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
