/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-local ID3DBlob.  Root-signature serialization returns blob
 * BYTES over the wire (a host-side ID3DBlob would hand the guest host
 * pointers), so the guest wraps them in this pure-local COM object.
 * Never crosses the wire and never enters the wrapper cache.
 */

#ifndef NPT_BLOB_H
#define NPT_BLOB_H

#include "npt_com.h"

/* Allocates a blob that owns a copy of `data` (size bytes; data may be
 * NULL when size is 0).  Returns NULL on allocation failure. */
ID3DBlob *
npt_blob_create(const void *data, SIZE_T size);

#endif /* NPT_BLOB_H */
