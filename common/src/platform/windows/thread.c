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

#include "common/thread.h"
#include "common/debug.h"
#include "common/windebug.h"

#include <windows.h>
#include <stdlib.h>

struct LGThread
{
  const char       * name;
  LGThreadFunction   function;
  void             * opaque;
  HANDLE             handle;
  DWORD              threadID;

  int                resultCode;
};

static DWORD WINAPI threadWrapper(LPVOID lpParameter)
{
  LGThread * handle = (LGThread *)lpParameter;
  handle->resultCode = handle->function(handle->opaque);
  return 0;
}

typedef HRESULT (WINAPI * SetThreadDescriptionFn)(HANDLE hThread,
    PCWSTR lpThreadDescription);

static void setThreadName(HANDLE thread, const char * name)
{
  if (!thread || !name || !*name)
    return;

  HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  if (!kernel32)
    return;

  SetThreadDescriptionFn setThreadDescription =
    (SetThreadDescriptionFn)GetProcAddress(kernel32, "SetThreadDescription");
  if (!setThreadDescription)
    return;

  wchar_t wideName[64];
  const int written = MultiByteToWideChar(CP_UTF8, 0, name, -1,
    wideName, ARRAYSIZE(wideName));
  if (written <= 0)
    return;

  setThreadDescription(thread, wideName);
}

bool lgCreateThread(const char * name, LGThreadFunction function, void * opaque, LGThread ** handle)
{
  *handle             = malloc(sizeof(**handle));
  (*handle)->name     = name;
  (*handle)->function = function;
  (*handle)->opaque   = opaque;
  (*handle)->handle   = CreateThread(NULL, 0, threadWrapper, *handle, 0, &(*handle)->threadID);

  if (!(*handle)->handle)
  {
    free(*handle);
    *handle = NULL;
    DEBUG_WINERROR("CreateThread failed", GetLastError());
    return false;
  }

  setThreadName((*handle)->handle, name);
  return true;
}

bool lgJoinThread(LGThread * handle, int * resultCode)
{
  while(true)
  {
    switch(WaitForSingleObject(handle->handle, INFINITE))
    {
      case WAIT_OBJECT_0:
        if (resultCode)
          *resultCode = handle->resultCode;
        CloseHandle(handle->handle);
        free(handle);
        return true;

      case WAIT_ABANDONED:
      case WAIT_TIMEOUT:
        continue;

      case WAIT_FAILED:
        DEBUG_WINERROR("Wait for thread failed", GetLastError());
        CloseHandle(handle->handle);
        free(handle);
        return false;
    }

    break;
  }

  DEBUG_WINERROR("Unknown failure waiting for thread", GetLastError());
  return false;
}
