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

The sweetest thing about MLNX_OFED is: the user space libraries they provided can only work on a kernel with OFED kernel modules loaded. According to the error message and some digging into kernel source code, it should be some uverbs commands format mismatch (correct me if I'm wrong!).

That means, if you do a install like this `./mlnxofedinstall --user-space-only`, which will install both user space commands and libraries, the user space commands you got _are just broken_.

One has to download [rdma-core](https://github.com/linux-rdma/rdma-core), compile it, and use the `LD_PRELOAD` trick to avoid using OFED's user space libraries. Why the hell I got to know this? Because a man got his needs, just like LITE is not compatible with OFED.

### Switch between OFED
Oh yes, one can switch between OFED kernel and non-OFED kernel. OFED kernel modules are built/loaded on a per-kernel basis. That means, you can have kernel-1 and kernel-2, with kernel-1 has OFED installed while kernel-2 has not. You can switch via reboot.

### Commands
* Download `rdma-core` and compile.
* `export LD_PRELOAD="~/rdma-core/build/lib/libibverbs.so.1.1.19.0"` (replace the directory and version number)
* `ibv_devinfo ...`


## Personal note on LITE
There are 4 (NUM_PARALLEL_CONNECTION) QP connections between each pair of node. That means all applications on node A will use share these 4 QPs to talk with node B, node A use another 4 QPs to talk with node C, and so on.

Each QP has its own `send_cq`, while all QPs in a node share the same `recv_cq`, which is `ctx->cq`. I think it is because it only has 1 polling thread that will poll the receive cq.

Function `client_get_connection_by_atomic_number` determins which QP is used when node A wants to talk with B. Default is round-robin (poor locality).

I spent so much time to find out this minor fact.
