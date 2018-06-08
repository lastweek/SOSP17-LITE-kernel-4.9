# LITE on kernel 4.9

This is a port of LITE for `kernel-4.9.103`. It is known to work at `Ubuntu (16.04.4)`.

It is not necessary to use `kernel-4.9`. You can use lower version kernel, but no higher than `4.12`,
which has 5-level pgtable.

## Disable CONFIG_VMAP_STACK
LITE code uses a lot stack variables to do RDMA. On some recent kernels, kernel stack are allocated
by `vmalloc()`. The stack address returned can not be used to do `ib_dma_map` stuff. Hence, please
turn off `CONFIG_VMAP_STACK`, to fall back `kmalloc()`'ed kernel stack.
