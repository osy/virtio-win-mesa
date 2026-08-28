/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_COM_H
#define NPT_COM_H

#include "npt_common.h"
#include "npt_cs.h"
#include "npt_device.h"
#include "npt_object.h"
#include "npt_ring.h"
#include "npt_tls.h"

#include <stddef.h>

/* Also defined by neptune-protocol headers; guard against redefinition. */
#ifndef NPT_STDMETHODCALLTYPE
#  if defined(_WIN32)
#    define NPT_STDMETHODCALLTYPE __stdcall
#  else
#    define NPT_STDMETHODCALLTYPE
#  endif
#endif

#if defined(_WIN32)
#define NPT_WINAPI __stdcall
#if defined(_MSC_VER)
/* The .def file controls exports under MSVC; emitting __declspec(dllexport)
 * here would conflict with the SDK's __declspec(dllimport) declarations
 * of the same names (D3D11CreateDevice, CreateDXGIFactory, ...). */
#define NPT_DLLEXPORT
#else
#define NPT_DLLEXPORT __attribute__((dllexport))
#endif
#define NPT_API NPT_DLLEXPORT NPT_WINAPI
#else
#define NPT_WINAPI
#define NPT_DLLEXPORT __attribute__((visibility("default")))
#define NPT_API NPT_DLLEXPORT
#endif

struct npt_com_qi_memo_entry;

/*
 * Every Neptune COM wrapper starts with `lpVtbl` followed by struct
 * npt_object.  `base.id` is the host COM pointer cast to uint64_t.
 */
struct npt_com_base {
   const void **lpVtbl;
   struct npt_object base;
   /* Free-form per-family side state: a malloc'd struct, a pool index
    * packed into a pointer, etc.  Populated when the IID's ctor-table
    * entry declares aux_size > 0 (npt_com_register_family). */
   void *aux;
   /* Owns aux's lifetime; called by npt_com_destroy. */
   void (*aux_destroy)(void *aux);
   /* Host QueryInterface verdicts learned for this wrapper, one wire
    * round trip per IID for the wrapper's lifetime (npt_com.c). */
   struct npt_com_qi_memo_entry *qi_memo;
};

/* npt_object_get_id (npt_cs.h) reads host_id through struct npt_com_head;
 * pin the layout so a struct change trips here. */
_Static_assert(offsetof(struct npt_com_base, base) ==
                  offsetof(struct npt_com_head, id),
               "npt_com_base: base must overlay npt_com_head's id");
_Static_assert(offsetof(struct npt_object, id) == 0,
               "npt_object: id must be the first field");

/*
 * Synthetic host_ids for wrappers with no host counterpart (guest-side
 * IDXGIOutput, etc.).  x86_64 user pointers never have bit 63 set, so
 * this bit can't collide with a real host pointer.  npt_com_send_release
 * skips the wire send when this bit is on.
 */
#define NPT_GUEST_FAB_BIT (1ull << 63)
#define NPT_GUEST_KIND_OUTPUT 1u
#define NPT_GUEST_KIND_SWAPCHAIN 2u

static inline uint64_t
npt_com_make_guest_id(uint8_t kind, uint64_t low)
{
   return NPT_GUEST_FAB_BIT |
          ((uint64_t)kind << 56) |
          (low & 0x00ffffffffffffffull);
}

static inline bool
npt_com_id_is_guest_fab(uint64_t id)
{
   return (id & NPT_GUEST_FAB_BIT) != 0;
}

/* host_id = 0 for objects created locally before the host call. */
static inline void
npt_com_base_init(struct npt_com_base *com, const void **vtbl,
                  struct npt_device *dev, uint64_t host_id)
{
   com->lpVtbl = vtbl;
   npt_object_init(&com->base, host_id, dev);
   com->aux = NULL;
   com->aux_destroy = NULL;
   com->qi_memo = NULL;
}

static inline uint64_t
npt_com_self_id(void *self)
{
   return ((struct npt_com_base *)self)->base.id;
}

static inline struct npt_ring *
npt_com_self_ring(void *self)
{
   struct npt_com_base *com = self;
   if (com->base.instance_ring)
      return com->base.instance_ring;
   struct npt_ring *ring = npt_tls_get_ring(com->base.device);
   /* Per-object cross-ring ordering (see npt_object.h ring_ordered):
    * when a flagged object's traffic moves to a new ring, drain the
    * previous ring before the first submission here so the host decodes
    * this object's commands in call order.  Sequential-use-by-contract
    * objects only; the exchange is uncontended in valid API use. */
   if (com->base.ring_ordered && ring) {
      uint64_t prev = atomic_exchange_explicit(
         &com->base.order_ring_id, ring->id, memory_order_acq_rel);
      if (prev && prev != ring->id)
         npt_tls_drain_ring_id(com->base.device, prev);
   }
   return ring;
}

static inline struct npt_device *
npt_com_self_device(void *self)
{
   return ((struct npt_com_base *)self)->base.device;
}

/*
 * Default IUnknown::AddRef: bump pub_ref; on 0->1, bump priv_ref and
 * AddRef the parent wrapper (parent coupling, see npt_object.h).
 * Returns the new public refcount.
 */
uint32_t NPT_STDMETHODCALLTYPE
npt_com_default_addref(void *self);

/*
 * Default IUnknown::Release: dec pub_ref; on 1->0, dec the matching
 * priv_ref pair (and parent's).  Final priv_ref==0 evicts from cache,
 * sends COM_RELEASE, and frees the wrapper.
 */
uint32_t NPT_STDMETHODCALLTYPE
npt_com_default_release(void *self);

/*
 * Tear down a wrapper bypassing refcount checks.  Runs the same
 * destruction sequence as a Release-driven destroy but does NOT touch
 * parent priv_refs, so it's safe to call from a flat foreach over the
 * wrapper cache without parent-first ordering.
 */
void
npt_com_force_destroy(struct npt_com_base *com);

/*
 * Satisfy QueryInterface without a host round-trip by matching `riid`
 * against a NULL-terminated chain of parent IIDs.  On match, bumps
 * refcount and writes *out = self.  E_NOINTERFACE on miss; caller
 * decides whether a host round-trip is worth attempting.
 */
HRESULT
npt_com_default_query_interface_chain(void *self, const GUID *riid,
                                      void **out, const GUID *const *chain);

/*
 * Cache lookup for `host_id` in dev's wrapper cache.  Hit returns the
 * cached wrapper with an extra public ref; miss runs the registered
 * ctor for `iid` and inserts the new wrapper.  `parent_wrapper`
 * enables parent coupling; NULL for top-level / unparented objects.
 * NULL return = OOM or no ctor registered.
 */
void *
npt_com_get_or_wrap(struct npt_device *dev, const GUID *iid,
                    uint64_t host_id, struct npt_com_base *parent_wrapper);

/*
 * Leak-safe variant: on wrapper-allocation failure, sends COM_RELEASE
 * for host_id so the host-side ref the CreateXxx call took doesn't
 * leak.  Treat NULL return as E_OUTOFMEMORY.
 */
void *
npt_com_get_or_wrap_or_release(struct npt_device *dev, const GUID *iid,
                               uint64_t host_id,
                               struct npt_com_base *parent_wrapper);

/*
 * Monotonic 64-bit guest-allocated object id.  Sent in command bodies
 * so the host can register {id -> host_ptr} without a reply round-trip.
 * Starts at 1; 0 stays reserved for null/invalid.
 */
uint64_t npt_com_allocate_next_id(void);

/*
 * Constructors return a wrapper with pub_ref==1, priv_ref==0; the
 * runtime sets parent and does the initial parent priv_ref bump.
 */
typedef void *(*npt_com_ctor_fn)(struct npt_device *dev, uint64_t object_id);

void
npt_com_register_default_ctor(const GUID *iid, npt_com_ctor_fn ctor);

/*
 * Aux state: runtime-allocated per-wrapper side data.  A family
 * declares aux_size and aux_init via npt_com_register_family; on
 * cache-miss the runtime calloc's aux_size bytes into com->aux and
 * calls aux_init, which fills aux and installs com->aux_destroy.
 * aux_destroy owns aux's lifetime (the runtime never free()s aux
 * itself, so aux can be any shape -- pool index, inline data, etc.).
 *
 * Vtbl overrides patch the generator's default vtbl in place via
 * NPT_REGISTER_OVERRIDE; no custom ctor is needed to swap the vtbl.
 */

typedef void (*npt_com_aux_init_fn)(struct npt_com_base *com,
                                    struct npt_device *dev,
                                    uint64_t host_id);

/*
 * Register a tiered family.  `tier_iids` is a NULL-terminated list
 * (e.g. {ID3D11Device, Device1, ..., Device5, NULL}); each wrapper
 * keeps its own tier's vtbl, so callers must QI to obtain a wider-
 * tier pointer.  Must run after the default-ctor populator.
 */
void
npt_com_register_family(const GUID *const *tier_iids,
                        size_t aux_size,
                        npt_com_aux_init_fn aux_init);

/* Resolve a wrapper's per-family aux, tolerating tier aliases.  A
 * QI-minted higher-tier wrapper (e.g. ID3D12Resource1 from an
 * ID3D12Resource) shares its primary's aux with aux_destroy cleared to
 * avoid a double free; a plain `aux_destroy == fn` test would reject
 * it.  Returns com->aux when self carries aux_destroy == fn, or when
 * self is such an alias whose same-family ancestor owns it; NULL
 * otherwise. */
void *
npt_com_family_aux(void *self, void (*aux_destroy)(void *aux));

/* Pin a D3D12 command-queue wrapper onto its host decode ring.  DIRECT
 * queues share the DC/SC ring so their submissions serialize with the
 * swapchain's present copies; non-DIRECT (compute/copy) queues get a
 * private ring (own host decode thread) because their submissions have
 * no ordering contract with Present, and decoding them behind the gfx
 * queue's ECL stream on the shared ring serializes async compute
 * against graphics.  Cross-queue ordering is carried by fence values
 * (Signal/Wait), which are decode-order independent.  Both are no-ops
 * when multi-ring is off or on allocation failure (the wrapper then
 * falls back to the caller's TLS ring). */
void
npt_com_pin_queue_ring(void *self, bool direct);

/* Typed-field assignment gives compile-time signature checking and
 * catches method-name typos. */
#define NPT_REGISTER_OVERRIDE(iface_lower, method_name, override_fn) \
    (npt_##iface_lower##_default_vtbl_storage.method_name = (override_fn))

/* Hard abort: a `skip_default` thunk was invoked without an override. */
void
npt_com_assert_overridden(const char *iid_name, const char *method_name);

/* Idempotent.  Must be called before any wrapper is constructed. */
void
npt_com_init(void);

void
npt_com_send_release(struct npt_device *dev, uint64_t host_id);

/*
 * QueryInterface fallback for IIDs outside the wrapper's parent chain:
 * resolves against the host object, memoized per (wrapper, IID) --
 * one wire round trip for the wrapper's lifetime, after which repeat
 * QIs return the same interface pointer with a fresh public ref and
 * no wire traffic.  Called by the generated default QI thunks.
 */
HRESULT
npt_com_query_interface_host(void *self, const GUID *riid, void **ppvObject);

/*
 * The ring this object's commands are submitted on and, when out_seqno is
 * non-NULL, the seqno the host's decode position must reach for everything
 * submitted on that ring so far to have been applied.  Out of line because
 * Triton's D3D12 files cannot include this header: they take the SDK's
 * d3d12.h, whose types collide with the wire protocol's own declarations.
 */
struct npt_ring *
npt_com_object_ring_seqno(void *self, uint32_t *out_seqno);

#endif /* NPT_COM_H */
