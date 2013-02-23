#include "gdev_api.h"
#include "gdev_device.h"
#include "gdev_nvidia.h"
#include "gdev_nvidia_fifo.h"
#include "nvrm.h"
#include "nvrm_def.h"

#define GDEV_DEVICE_MAX_COUNT 32

static struct nvrm_context *nvrm_ctx = 0;

int gdev_raw_query(struct gdev_device *gdev, uint32_t type, uint64_t *result)
{
	struct nvrm_device *dev = gdev->priv;
	uint32_t chip_major, chip_minor;

	switch (type) {
	case GDEV_NVIDIA_QUERY_MP_COUNT:
		/* XXX */
		*result = 4;
		return 0;
		goto fail;
	case GDEV_QUERY_DEVICE_MEM_SIZE:
		/* XXX */
		goto fail;
	case GDEV_QUERY_DMA_MEM_SIZE:
		/* XXX */
		goto fail;
	case GDEV_QUERY_CHIPSET:
		if (nvrm_device_get_chipset(dev, &chip_major, &chip_minor, 0))
			goto fail;
		*result = chip_major | chip_minor;
		break;
	default:
		goto fail;
	}

	return 0;

fail:
	GDEV_PRINT("Failed to query %u\n", type);
	return -EINVAL;
}

/* open a new Gdev object associated with the specified device. */
struct gdev_device *gdev_raw_dev_open(int minor)
{
	struct gdev_device *gdev;
	if (!nvrm_ctx) {
		nvrm_ctx = nvrm_open();
		if (!nvrm_ctx)
			return NULL;
	}

	if (!gdevs) {
		gdevs = MALLOC(sizeof(*gdevs) * GDEV_DEVICE_MAX_COUNT);
		if (!gdevs)
			return NULL;
		memset(gdevs, sizeof(*gdevs) * GDEV_DEVICE_MAX_COUNT, 0);
	}

	gdev = &gdevs[minor];

	if (gdev->users == 0) {
		struct nvrm_device *dev = nvrm_device_open(nvrm_ctx, minor);
		if (!dev)
			return NULL;
		gdev_init_device(gdev, minor, dev);
	}		

	gdev->users++;

	return gdev;
}

/* close the specified Gdev object. */
void gdev_raw_dev_close(struct gdev_device *gdev)
{
	struct nvrm_device *dev = gdev->priv;
	int i;

	gdev->users--;

	if (gdev->users == 0) {
		gdev_exit_device(gdev);
		nvrm_device_close(dev);
		for (i = 0; i < GDEV_DEVICE_MAX_COUNT; i++) {
			if (gdevs[i].users > 0)
				return;
		}
		FREE(gdevs);
		nvrm_close(nvrm_ctx);
		nvrm_ctx = NULL;
		gdevs = NULL;
	}
}

/* allocate a new virual address space object.  */
struct gdev_vas *gdev_raw_vas_new(struct gdev_device *gdev, uint64_t size)
{
	struct nvrm_device *dev = gdev->priv;
	struct nvrm_vspace *nvas;
	struct gdev_vas *vas;

	if (!(vas = malloc(sizeof(*vas))))
		goto fail_vas;
	if (!(nvas = nvrm_vspace_create(dev)))
		goto fail_nvas;

	/* private data */
	vas->pvas = nvas;

	return vas;

fail_nvas:
	free(vas);
fail_vas:
	return NULL;
}

/* free the specified virtual address space object. */
void gdev_raw_vas_free(struct gdev_vas *vas)
{
	struct nvrm_vspace *nvas = vas->pvas;

	nvrm_vspace_destroy(nvas);
	free(vas);
}

/* create a new GPU context object. 
   we don't use @vas->pchan, as a channel is already held by @vas->pvas. */
struct gdev_ctx *gdev_raw_ctx_new
(struct gdev_device *gdev, struct gdev_vas *vas)
{
	struct gdev_ctx *ctx;
	struct nvrm_channel *chan;
	struct nvrm_vspace *nvas = vas->pvas;
	uint32_t chipset = gdev->chipset;
	uint32_t cls;
	uint32_t ccls;

	if (chipset < 0x80)
		cls = 0x506f, ccls = 0x50c0;
	else if (chipset < 0xc0)
		cls = 0x826f, ccls = 0x50c0;
	else if (chipset < 0xe0)
		cls = 0x906f, ccls = 0x90c0;
	else
		cls = 0xa06f, ccls = 0xa0c0;

	if (!(ctx = malloc(sizeof(*ctx))))
		goto fail_ctx;

	/* FIFO indirect buffer setup. */
	ctx->fifo.ib_order = 10;
	ctx->fifo.ib_bo = nvrm_bo_create(nvas, 8 << ctx->fifo.ib_order, 1);
	if (!ctx->fifo.ib_bo)
		goto fail_ib;
	ctx->fifo.ib_map = nvrm_bo_host_map(ctx->fifo.ib_bo);
	ctx->fifo.ib_base = nvrm_bo_gpu_addr(ctx->fifo.ib_bo);
	ctx->fifo.ib_mask = (1 << ctx->fifo.ib_order) - 1;
	ctx->fifo.ib_put = ctx->fifo.ib_get = 0;

	/* FIFO push buffer setup. */
	ctx->fifo.pb_order = 18;
	ctx->fifo.pb_mask = (1 << ctx->fifo.pb_order) - 1;
	ctx->fifo.pb_size = (1 << ctx->fifo.pb_order);
	ctx->fifo.pb_bo = nvrm_bo_create(nvas, ctx->fifo.pb_size, 1);
	if (!ctx->fifo.pb_bo)
		goto fail_pb;
	ctx->fifo.pb_map = nvrm_bo_host_map(ctx->fifo.pb_bo);
	ctx->fifo.pb_base = nvrm_bo_gpu_addr(ctx->fifo.pb_bo);
	ctx->fifo.pb_pos = ctx->fifo.pb_put = ctx->fifo.pb_get = 0;
	ctx->fifo.push = gdev_fifo_push;
	ctx->fifo.update_get = gdev_fifo_update_get;

	/* FIFO init */
	chan = nvrm_channel_create_ib(nvas, cls, ctx->fifo.ib_bo);
	if (!chan)
		goto fail_chan;
	ctx->pctx = chan;

	/* gr init */
	if (!nvrm_eng_create(chan, NVRM_FIFO_ENG_GRAPH, ccls))
		goto fail_eng;

	/* FIFO command queue registers. */
	ctx->fifo.regs = nvrm_channel_host_map_regs(chan);

	/* fence buffer. */
	ctx->fence.bo = nvrm_bo_create(nvas, GDEV_FENCE_BUF_SIZE, 1);
	if (!ctx->fence.bo)
		goto fail_fence_alloc;
	ctx->fence.map = nvrm_bo_host_map(ctx->fence.bo);
	ctx->fence.addr = nvrm_bo_gpu_addr(ctx->fence.bo);
	ctx->fence.seq = 0;

	/* interrupt buffer. */
	ctx->notify.bo = nvrm_bo_create(nvas, 64, 0);
	if (!ctx->notify.bo)
		goto fail_notify_alloc;
	ctx->notify.addr = nvrm_bo_gpu_addr(ctx->notify.bo);

	/* private data */
	ctx->pctx = (void *) chan;

	return ctx;

fail_notify_alloc:
	nvrm_bo_destroy(ctx->fence.bo);
fail_fence_alloc:
fail_eng:
	nvrm_channel_destroy(chan);
fail_chan:
	nvrm_bo_destroy(ctx->fifo.pb_bo);
fail_pb:
	nvrm_bo_destroy(ctx->fifo.ib_bo);
fail_ib:
	free(ctx);
fail_ctx:
	return NULL;
}

/* destroy the specified GPU context object. */
void gdev_raw_ctx_free(struct gdev_ctx *ctx)
{
	nvrm_bo_destroy(ctx->fence.bo);
	nvrm_bo_destroy(ctx->notify.bo);
	nvrm_channel_destroy(ctx->pctx);
	nvrm_bo_destroy(ctx->fifo.pb_bo);
	nvrm_bo_destroy(ctx->fifo.ib_bo);
	free(ctx);
}

/* allocate a new memory object. */
static inline struct gdev_mem *__gdev_raw_mem_alloc(struct gdev_vas *vas, uint64_t size, int sysram, int mappable)
{
	struct gdev_mem *mem;
	struct nvrm_vspace *nvas = vas->pvas;
	struct nvrm_bo *bo;

	if (!(mem = (struct gdev_mem *) malloc(sizeof(*mem))))
		goto fail_mem;

	if (!(bo = nvrm_bo_create(nvas, size, sysram)))
		goto fail_bo;

	/* address, size, and map. */
	mem->addr = nvrm_bo_gpu_addr(bo);
	mem->size = size;
	if (mappable)
		mem->map = nvrm_bo_host_map(bo);
	else
		mem->map = NULL;
	/* private data. */
	mem->bo = (void *) bo;

	return mem;

fail_bo:
	GDEV_PRINT("Failed to allocate NVRM buffer object.\n");
	free(mem);
fail_mem:
	return NULL;
}

/* allocate a new device memory object. size may be aligned. */
struct gdev_mem *gdev_raw_mem_alloc(struct gdev_vas *vas, uint64_t size)
{
	int mappable = 0;

	if (size <= GDEV_MEM_MAPPABLE_LIMIT)
		mappable = 1;

	return __gdev_raw_mem_alloc(vas, size, 0, mappable);
}

/* allocate a new host DMA memory object. size may be aligned. */
struct gdev_mem *gdev_raw_mem_alloc_dma(struct gdev_vas *vas, uint64_t size)
{
	return __gdev_raw_mem_alloc(vas, size, 1, 1);
}

/* free the specified memory object. */
void gdev_raw_mem_free(struct gdev_mem *mem)
{
	struct nvrm_bo *bo = mem->bo;

	nvrm_bo_destroy(bo);
	free(mem);
}

/* allocate a reserved swap memory object. size may be aligned. */
struct gdev_mem *gdev_raw_swap_alloc(struct gdev_device *gdev, uint64_t size)
{
	GDEV_PRINT("Swap memory not implemented\n");
	/* To be implemented. */
	return NULL;
}

/* free the specified swap memory object. */
void gdev_raw_swap_free(struct gdev_mem *mem)
{
	GDEV_PRINT("Swap memory not implemented\n");
	/* To be implemented. */
}

/* create a new memory object sharing memory space with @mem. */
struct gdev_mem *gdev_raw_mem_share(struct gdev_vas *vas, struct gdev_mem *mem)
{
	GDEV_PRINT("Shared memory not implemented\n");
	/* To be implemented. */
	return NULL;
}

/* destroy the memory object by just unsharing memory space. */
void gdev_raw_mem_unshare(struct gdev_mem *mem)
{
	GDEV_PRINT("Shared memory not implemented\n");
	/* To be implemented. */
}

uint32_t gdev_raw_read32(struct gdev_mem *mem, uint64_t addr)
{
	struct nvrm_bo *bo = mem->bo;
	void *ptr;
	if (mem->map) {
		ptr = mem->map;
	} else {
		ptr = nvrm_bo_host_map(bo);
	}

	uint32_t val;
	uint64_t offset = addr - mem->addr;
	val = *(uint32_t*)(ptr + offset);

	if (!mem->map) {
		nvrm_bo_host_unmap(bo);
	}
	return val;
}

void gdev_raw_write32(struct gdev_mem *mem, uint64_t addr, uint32_t val)
{
	struct nvrm_bo *bo = mem->bo;
	void *ptr;
	if (mem->map) {
		ptr = mem->map;
	} else {
		ptr = nvrm_bo_host_map(bo);
	}

	uint64_t offset = addr - mem->addr;
	*(uint32_t*)(ptr + offset) = val;

	if (!mem->map) {
		nvrm_bo_host_unmap(bo);
	}
}

int gdev_raw_read(struct gdev_mem *mem, void *buf, uint64_t addr, uint32_t size)
{
	struct nvrm_bo *bo = mem->bo;
	void *ptr;
	if (mem->map) {
		ptr = mem->map;
	} else {
		ptr = nvrm_bo_host_map(bo);
	}

	uint64_t offset = addr - mem->addr;
	memcpy(buf, ptr + offset, size);

	if (!mem->map) {
		nvrm_bo_host_unmap(bo);
	}
	return 0;
}

int gdev_raw_write(struct gdev_mem *mem, uint64_t addr, const void *buf, uint32_t size)
{
	struct nvrm_bo *bo = mem->bo;
	void *ptr;
	if (mem->map) {
		ptr = mem->map;
	} else {
		ptr = nvrm_bo_host_map(bo);
	}

	uint64_t offset = addr - mem->addr;
	memcpy(ptr + offset, buf, size);

	if (!mem->map) {
		nvrm_bo_host_unmap(bo);
	}
	return 0;
}

/* map device memory to host DMA memory. */
void *gdev_raw_mem_map(struct gdev_mem *mem)
{
	struct nvrm_bo *bo = mem->bo;
	return nvrm_bo_host_map(bo);
}

/* unmap device memory from host DMA memory. */
void gdev_raw_mem_unmap(struct gdev_mem *mem, void *map)
{
	struct nvrm_bo *bo = mem->bo;
	nvrm_bo_host_unmap(bo);
}

/* get physical bus address. */
uint64_t gdev_raw_mem_phys_getaddr(struct gdev_mem *mem, uint64_t offset)
{
	/* XXX */
	return 0;
}

