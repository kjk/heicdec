/* fuzz_win_md_shim.c -- satisfy MSVC /MD import refs under clang ASan.
 *
 * libdav1d (and other meson/cmake deps) are built with MSVC /MD, so objects
 * reference __imp__aligned_malloc / __imp__aligned_free. Clang ASan cannot
 * pull in dynamic ucrt.lib for those without also defining malloc/free and
 * clashing with the ASan runtime thunk. Provide just the aligned-alloc
 * import pointers, implemented with the process heap (ASan-intercepted
 * malloc/free via the usual path).
 *
 * Windows + fuzz build only. */
#if defined(_WIN32)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static void *shim_aligned_malloc(size_t size, size_t alignment)
{
    size_t pad;
    void *raw;
    uintptr_t addr;
    void *aligned;

    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    /* power-of-two alignment (dav1d always passes one) */
    if (alignment & (alignment - 1)) alignment = sizeof(void *);

    pad = alignment - 1 + sizeof(void *);
    if (size > (size_t)-1 - pad) return NULL;
    raw = malloc(size + pad);
    if (!raw) return NULL;
    addr = ((uintptr_t)raw + sizeof(void *) + (alignment - 1)) &
           ~(uintptr_t)(alignment - 1);
    aligned = (void *)addr;
    ((void **)aligned)[-1] = raw;
    return aligned;
}

static void shim_aligned_free(void *p)
{
    if (!p) return;
    free(((void **)p)[-1]);
}

/* IAT pointers expected by /MD object files (x64 MSVC name decoration). */
void *(*__imp__aligned_malloc)(size_t, size_t) = shim_aligned_malloc;
void (*__imp__aligned_free)(void *) = shim_aligned_free;

#endif /* _WIN32 */
