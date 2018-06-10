# LITE on kernel 4.9

This is a port of LITE for `kernel-4.9.103`. It is known to work at `Ubuntu (16.04.4)`.

It is not necessary to use `kernel-4.9`. You can use lower version kernel, but no higher than `4.12`,
which has 5-level pgtable.

## Disable CONFIG_VMAP_STACK
LITE code uses a lot stack variables to do RDMA. On some recent kernels, kernel stack are allocated
by `vmalloc()`. The stack address returned can not be used to do `ib_dma_map` stuff. Hence, please
turn off `CONFIG_VMAP_STACK`, to fall back `kmalloc()`'ed kernel stack.

## Choose IB device
### Server
In `cluster-manager/lite-cd.c: server_init_interface()`, select the ib_dev by changing `dev_list[N]`.
```
ib_dev = dev_list[1];
if (!ib_dev) {
	fprintf(stderr, "No IB devices found\n");
	return 1;
}
```

### Client
In `core/lite_core.c: ibv_add_one()`, select the right `liteapi_dev`, either by
comparing dev_name or simple skip.

## Avoid Building UD between Server and Clients
There is no need to build UD connection between server and clients. Especially
if you want to test back-to-back Infiniband performance. So I modified the code
a little bit, to avoid building `ah` between server and client.


## Personal note on OFED
### OFED Packages
To me, the evil MLNX_OFED mainly includes three different things:
* User space commands, such as `ibv_devinfo`, `ib_write_lat`, and so on.
* User space libraries, such as `libibverbs`, `libmad`, and so on.
    * The user space stuff are basically [rdma-core](https://github.com/linux-rdma/rdma-core) and [perftest](https://www.openfabrics.org/downloads/perftest/).
* Kernel modules, which replaces the original in-box Linux IB driver.

The sweetest thing about MLNX_OFED is: the user space libraries they provided can only work on a kernel with OFED kernel modules loaded. If you do a install like this `./mlnxofedinstall --user-space-only`, which will install both user space commands and libraries, the user space commands you got are just broken.

One (yes it' sme) has to download [rdma-core](https://github.com/linux-rdma/rdma-core), compile it, and use the `LD_PRELOAD` trick to avoid using OFED's user space libraries. Why the hell I got to know this? Because a man got his needs, just like LITE is not compatible with OFED.

### Commands
* Download `rdma-core` and compile.
* `export LD_PRELOAD="~/rdma-core/build/lib/libibverbs.so.1.1.19.0"` (replace the directory and version number)
* `ibv_devinfo ...`
