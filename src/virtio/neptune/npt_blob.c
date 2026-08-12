/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_blob.h"

#include "neptune-protocol/npt_protocol_defs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct npt_blob {
   const void **lpVtbl;
   _Atomic uint32_t ref_count;
   void *data;
   SIZE_T size;
};

static HRESULT NPT_STDMETHODCALLTYPE
blob_QueryInterface(void *self, REFIID riid, void **ppvObject)
{
   if (!ppvObject)
      return NPT_E_INVALIDARG;
   if (riid &&
       (memcmp(riid, &NPT_IID_IUnknown, sizeof(GUID)) == 0 ||
        memcmp(riid, &NPT_IID_ID3D10Blob, sizeof(GUID)) == 0)) {
      atomic_fetch_add_explicit(&((struct npt_blob *)self)->ref_count, 1,
                                memory_order_relaxed);
      *ppvObject = self;
      return NPT_S_OK;
   }
   *ppvObject = NULL;
   return NPT_E_NOINTERFACE;
}

static ULONG NPT_STDMETHODCALLTYPE
blob_AddRef(void *self)
{
   struct npt_blob *blob = self;
   return atomic_fetch_add_explicit(&blob->ref_count, 1,
                                    memory_order_relaxed) + 1;
}

static ULONG NPT_STDMETHODCALLTYPE
blob_Release(void *self)
{
   struct npt_blob *blob = self;
   uint32_t prev = atomic_fetch_sub_explicit(&blob->ref_count, 1,
                                             memory_order_acq_rel);
   if (prev == 1) {
      free(blob->data);
      free(blob);
   }
   return prev - 1;
}

static void *NPT_STDMETHODCALLTYPE
blob_GetBufferPointer(void *self)
{
   return ((struct npt_blob *)self)->data;
}

static SIZE_T NPT_STDMETHODCALLTYPE
blob_GetBufferSize(void *self)
{
   return ((struct npt_blob *)self)->size;
}

/* Slot order matches ID3D10BlobVtbl (see NPT_VTBL_ID3D10Blob_*). */
static const void *npt_blob_vtbl[] = {
   (const void *)blob_QueryInterface,
   (const void *)blob_AddRef,
   (const void *)blob_Release,
   (const void *)blob_GetBufferPointer,
   (const void *)blob_GetBufferSize,
};

ID3DBlob *
npt_blob_create(const void *data, SIZE_T size)
{
   struct npt_blob *blob = npt_alloc(sizeof(*blob));
   if (!blob)
      return NULL;

   if (size) {
      blob->data = malloc(size);
      if (!blob->data) {
         free(blob);
         return NULL;
      }
      if (data)
         memcpy(blob->data, data, size);
      else
         memset(blob->data, 0, size);
   }
   blob->size = size;
   blob->lpVtbl = npt_blob_vtbl;
   atomic_store_explicit(&blob->ref_count, 1, memory_order_relaxed);
   return (ID3DBlob *)blob;
}
