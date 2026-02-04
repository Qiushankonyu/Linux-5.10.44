/*
 * drivers/gpu/drm/virtio/virtgpu_object.c
 *
 * Modified for Type-1 Hypervisor DMA Coherent Memory
 * Fixed: Segmentation Faults due to uninitialized mutexes and shmem state.
 */

#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>
#include <linux/pfn.h>
/* 必须包含，用于复用原生 helper */
#include <drm/drm_gem_shmem_helper.h>

#include "virtgpu_drv.h"

static int virtio_gpu_virglrenderer_workaround = 1;
module_param_named(virglhack, virtio_gpu_virglrenderer_workaround, int, 0400);

/* ===================================================================
 * 1. 自定义函数区 (DMA 适配)
 * =================================================================== */

/* [自定义 Free] 必须覆盖，因为我们需要释放 DMA 内存 */
static void virtio_gpu_free_shared_object(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;

	/* 1. 释放 SG Table */
	if (bo->base.sgt) {
		sg_free_table(bo->base.sgt);
		kfree(bo->base.sgt);
		bo->base.sgt = NULL;
	}

	/* 2. 释放 DMA 内存 */
	/* 注意：我们在 create 时将 dma_alloc 的地址填入了 base.vaddr */
	if (bo->base.vaddr && bo->dma_addr) {
		dev_dbg(obj->dev->dev, "DEBUG: Freeing DMA mem: vaddr=%p\n",
			bo->base.vaddr);
		dma_free_coherent(vgdev->vdev->dev.parent, obj->size,
				  bo->base.vaddr, bo->dma_addr);
		/* 置空防止基类 shmem_free 再次尝试处理 */
		bo->base.vaddr = NULL;
	}

	/* 3. 调用基类清理 (处理引用计数、剩余锁资源等) */
	drm_gem_shmem_free_object(obj);
}

/* [自定义 Mmap] 必须覆盖，DMA 内存需要专用映射 API */
static int virtio_gpu_gem_mmap(struct drm_gem_object *obj,
			       struct vm_area_struct *vma)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	int ret;

	/* 清除 PFNMAP，因为 dma_mmap_coherent 会重新设置它 */
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_IO;

	/* 使用 DMA Coherent 映射 */
	ret = dma_mmap_coherent(vgdev->vdev->dev.parent, vma, bo->base.vaddr,
				bo->dma_addr, obj->size);

	if (ret)
		dev_err(vgdev->ddev->dev, "DEBUG: mmap failed: %d\n", ret);

	return ret;
}

/* [自定义 Vmap] DMA coherent 内存已具备线性映射，直接返回 vaddr */
static void *virtio_gpu_gem_vmap(struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
	void *vaddr;
	int ret;

	vaddr = shmem->vaddr;
	if (!vaddr)
		return ERR_PTR(-ENOMEM);

	ret = mutex_lock_interruptible(&shmem->vmap_lock);
	if (ret)
		return ERR_PTR(ret);
	shmem->vmap_use_count++;
	mutex_unlock(&shmem->vmap_lock);

	return vaddr;
}

/* [自定义 Vunmap] DMA coherent 内存不需要真正解除映射 */
static void virtio_gpu_gem_vunmap(struct drm_gem_object *obj, void *vaddr)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	if (!vaddr)
		return;

	mutex_lock(&shmem->vmap_lock);
	if (WARN_ON_ONCE(!shmem->vmap_use_count)) {
		mutex_unlock(&shmem->vmap_lock);
		return;
	}
	shmem->vmap_use_count--;
	mutex_unlock(&shmem->vmap_lock);
}

/* [函数操作表] 
 * 关键策略：复用原生 vmap，但覆盖 free 和 mmap。
 */
static const struct drm_gem_object_funcs virtio_gpu_gem_funcs = {
	.free = virtio_gpu_free_shared_object,
	.open = virtio_gpu_gem_object_open,
	.close = virtio_gpu_gem_object_close,

	/* 复用原生函数，只要 create 时初始化好锁和 vaddr，它们就能工作 */
	.print_info = drm_gem_shmem_print_info,
	.pin = drm_gem_shmem_pin,
	.unpin = drm_gem_shmem_unpin,
	.get_sg_table = drm_gem_shmem_get_sg_table,
	.vmap = virtio_gpu_gem_vmap,
	.vunmap = virtio_gpu_gem_vunmap,

	.mmap = virtio_gpu_gem_mmap, /* 自定义 */
};

/* ===================================================================
 * 2. 辅助函数区
 * =================================================================== */

static int virtio_gpu_resource_id_get(struct virtio_gpu_device *vgdev,
				      uint32_t *resid)
{
	if (virtio_gpu_virglrenderer_workaround) {
		static atomic_t seqno = ATOMIC_INIT(0);
		int handle = atomic_inc_return(&seqno);
		*resid = handle + 1;
	} else {
		int handle = ida_alloc(&vgdev->resource_ida, GFP_KERNEL);
		if (handle < 0)
			return handle;
		*resid = handle + 1;
	}
	return 0;
}

static void virtio_gpu_resource_id_put(struct virtio_gpu_device *vgdev,
				       uint32_t id)
{
	if (!virtio_gpu_virglrenderer_workaround) {
		ida_free(&vgdev->resource_ida, id - 1);
	}
}

/* 兼容性修改：支持识别我们新的函数表 */
bool virtio_gpu_is_shmem(struct virtio_gpu_object *bo)
{
	return bo->base.base.funcs == &virtio_gpu_gem_funcs;
}

void virtio_gpu_cleanup_object(struct virtio_gpu_object *bo)
{
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;

	virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
	/* * 这里会调用 drm_gem_object_put -> virtio_gpu_free_shared_object 
     * 所以不需要手动释放内存，交给 release 流程。
     */
	if (virtio_gpu_is_shmem(bo)) {
		drm_gem_object_put(&bo->base.base);
	}
}

/* 旧函数保留并标记未使用，避免删除导致的代码变动过大 */
static void __maybe_unused virtio_gpu_free_object(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;

	if (bo->created) {
		virtio_gpu_cmd_unref_resource(vgdev, bo);
		virtio_gpu_notify(vgdev);
		return;
	}
	virtio_gpu_cleanup_object(bo);
}

static int __maybe_unused virtio_gpu_object_shmem_init(
	struct virtio_gpu_device *vgdev, struct virtio_gpu_object *bo,
	struct virtio_gpu_mem_entry **ents, unsigned int *nents)
{
	return 0;
}

static struct virtio_gpu_mem_entry *
virtio_gpu_mem_entries_from_sgt(struct sg_table *sgt, unsigned int *nents)
{
	struct virtio_gpu_mem_entry *ents;
	struct scatterlist *sg;
	unsigned int i, n;

	if (!sgt || !nents)
		return NULL;

	n = sgt->nents;
	if (!n)
		return NULL;

	ents = kcalloc(n, sizeof(*ents), GFP_KERNEL);
	if (!ents)
		return NULL;

	for_each_sg (sgt->sgl, sg, n, i) {
		dma_addr_t addr = sg_dma_address(sg);
		u32 len = sg_dma_len(sg);

		if (!addr)
			addr = sg_phys(sg);
		if (!len)
			len = sg->length;

		ents[i].addr = cpu_to_le64(addr);
		ents[i].length = cpu_to_le32(len);
	}

	*nents = n;
	return ents;
}

/* ===================================================================
 * 3. 核心创建逻辑
 * =================================================================== */

/* 辅助：只分配结构体 */
struct drm_gem_object *virtio_gpu_create_object(struct drm_device *dev,
						size_t size)
{
	struct virtio_gpu_object *bo;
	struct drm_gem_object *obj;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	obj = &bo->base.base;
	/* 挂载新的函数表 */
	obj->funcs = &virtio_gpu_gem_funcs;

	drm_gem_private_object_init(dev, obj, size);

	/* * [关键修复 - SEGFAULT KILLER]
     * 原生 shmem create 会初始化这些锁。
     * 因为我们绕过了它，必须手动初始化，否则调用 vmap 时必崩。
     */
	mutex_init(&bo->base.pages_lock);
	mutex_init(&bo->base.vmap_lock);
	/* Linux 5.10.44 无需 madv_lock */

	/* 模拟原生行为 */
	bo->base.map_cached = true;
	return obj;
}

/* 主逻辑：DMA 分配 + 原生机制伪装 */
// int virtio_gpu_object_create(struct virtio_gpu_device *vgdev,
// 			     struct virtio_gpu_object_params *params,
// 			     struct virtio_gpu_object **bo_ptr,
// 			     struct virtio_gpu_fence *fence)
// {
// 	struct virtio_gpu_object_array *objs = NULL;
// 	struct drm_gem_object *gem_obj;
// 	struct virtio_gpu_object *bo;
// 	int ret;
// 	struct sg_table *sgt;
// 	struct scatterlist *sg;
// 	void *vaddr;

// 	/* [DEBUG] */
// 	dev_info(vgdev->ddev->dev, "DEBUG: virtio_gpu_object_create size=%lu\n",
// 		 (unsigned long)params->size);

// 	*bo_ptr = NULL;
// 	params->size = roundup(params->size, PAGE_SIZE);

// 	/* 1. 分配对象 (带锁初始化) */
// 	gem_obj = virtio_gpu_create_object(vgdev->ddev, params->size);
// 	if (IS_ERR(gem_obj))
// 		return PTR_ERR(gem_obj);

// 	bo = gem_to_virtio_gpu_obj(gem_obj);

// 	/* 2. DMA 连续内存分配 */
// 	vaddr = dma_alloc_coherent(vgdev->vdev->dev.parent, params->size,
// 				   &bo->dma_addr, GFP_KERNEL);
// 	if (!vaddr) {
// 		dev_err(vgdev->ddev->dev, "DEBUG: dma_alloc_coherent failed\n");
// 		ret = -ENOMEM;
// 		goto err_free_gem;
// 	}

// 	/* * [关键步骤] 填充基类 vaddr
//      * 只要这里有值，原生 drm_gem_shmem_vmap 就会直接返回它，
//      * 从而绕过 pages 检查。配合上面的 mutex_init，彻底解决 Segfault。
//      */
// 	bo->base.vaddr = vaddr;
// 	bo->base.pages_use_count = 1; /* 标记为已映射 */

// 	dev_info(vgdev->ddev->dev, "DEBUG: Alloc success. vaddr=%p dma=%pad\n",
// 		 vaddr, &bo->dma_addr);

// 	/* 3. 手动构建 SG Table (用于 Attach Backing) */
// 	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
// 	if (!sgt) {
// 		ret = -ENOMEM;
// 		goto err_free_dma;
// 	}

// 	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
// 	if (ret) {
// 		kfree(sgt);
// 		goto err_free_dma;
// 	}

// 	sg = sgt->sgl;
// 	sg_init_table(sg, 1);
// 	sg_set_page(sg, pfn_to_page(PFN_DOWN(bo->dma_addr)), params->size, 0);
// 	sg_dma_address(sg) = bo->dma_addr;
// 	sg_dma_len(sg) = params->size;

// 	bo->base.sgt = sgt;

// 	/* 4. 标准后续流程 (Resource ID, Fence, Virgl) */
// 	ret = virtio_gpu_resource_id_get(vgdev, &bo->hw_res_handle);
// 	if (ret < 0)
// 		goto err_free_sgt; /* 注意：sgt 挂在 bo 上，release 会清理 */

// 	bo->dumb = params->dumb;

// 	if (fence) {
// 		ret = -ENOMEM;
// 		objs = virtio_gpu_array_alloc(1);
// 		if (!objs)
// 			goto err_put_id;
// 		virtio_gpu_array_add_obj(objs, &bo->base.base);
// 		ret = virtio_gpu_array_lock_resv(objs);
// 		if (ret != 0)
// 			goto err_put_objs;
// 	}

// 	if (params->virgl) {
// 		virtio_gpu_cmd_resource_create_3d(vgdev, bo, params, objs,
// 						  fence);
// 	} else {
// 		virtio_gpu_cmd_create_resource(vgdev, bo, params, objs, fence);
// 	}

// 	/* 5. 绑定资源 (Host 根据 SG Table 映射) */
// 	/* 传入 NULL，驱动会自动使用我们构建好的 bo->base.sgt */
// 	virtio_gpu_object_attach(vgdev, bo, NULL, 0);

// 	*bo_ptr = bo;
// 	return 0;

// err_put_objs:
// 	virtio_gpu_array_put_free(objs);
// err_put_id:
// 	virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
// err_free_sgt:
// 	/* sg_table 会在 release 中被 sg_free_table 清理 */
// 	goto err_release;
// err_free_dma:
// 	/* 手动回滚 DMA 分配 */
// 	dma_free_coherent(vgdev->vdev->dev.parent, params->size, bo->base.vaddr,
// 			  bo->dma_addr);
// 	bo->base.vaddr = NULL;
// err_free_gem:
// err_release:
// 	drm_gem_object_release(gem_obj);
// 	kfree(bo);
// 	return ret;
// }

int virtio_gpu_object_create(struct virtio_gpu_device *vgdev,
			     struct virtio_gpu_object_params *params,
			     struct virtio_gpu_object **bo_ptr,
			     struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_object_array *objs = NULL;
	struct drm_gem_object *gem_obj;
	struct virtio_gpu_object *bo;
	int ret;
	struct sg_table *sgt;
	struct scatterlist *sg;
	void *vaddr;
	struct virtio_gpu_mem_entry *ents = NULL;
	unsigned int nents = 0;

	dev_info(vgdev->ddev->dev, "DEBUG: virtio_gpu_object_create size=%lu\n",
		 (unsigned long)params->size);

	*bo_ptr = NULL;
	params->size = roundup(params->size, PAGE_SIZE);

	/* 1. 分配对象 */
	gem_obj = virtio_gpu_create_object(vgdev->ddev, params->size);
	if (IS_ERR(gem_obj))
		return PTR_ERR(gem_obj);

	bo = gem_to_virtio_gpu_obj(gem_obj);

	/* 2. DMA 连续内存分配 */
	vaddr = dma_alloc_coherent(vgdev->vdev->dev.parent, params->size,
				   &bo->dma_addr, GFP_KERNEL);
	if (!vaddr) {
		dev_err(vgdev->ddev->dev, "DEBUG: dma_alloc_coherent failed\n");
		ret = -ENOMEM;
		goto err_free_gem;
	}

	bo->base.vaddr = vaddr;
	bo->base.pages_use_count = 1;

	dev_info(vgdev->ddev->dev,
		 "DEBUG: Alloc success. vaddr=%p dma=0x%llx\n", vaddr,
		 (u64)bo->dma_addr);

	/* 3. 手动构建 SG Table - 修复版本 */
	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto err_free_dma;
	}

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		goto err_free_dma;
	}

	sg = sgt->sgl;

	/* [关键修复] 
     * 对于共享内存区域，可能没有 struct page，直接设置物理地址
     */
	sg_init_table(sg, 1);

	/* 检查地址是否在可用内存范围内 */
	if (pfn_valid(PFN_DOWN(bo->dma_addr))) {
		/* 标准内存区域，使用 page */
		sg_set_page(sg, pfn_to_page(PFN_DOWN(bo->dma_addr)),
			    params->size, 0);
		dev_info(vgdev->ddev->dev, "DEBUG: Using page-based SG\n");
	} else {
		/* 共享内存区域，直接设置物理地址 */
		sg->page_link = 0; /* 无 page */
		sg->offset = 0;
		sg->length = params->size;
		dev_info(vgdev->ddev->dev,
			 "DEBUG: Using physical address SG (no page)\n");
	}

	/* 设置 DMA 地址 (这才是 virtio 后端真正使用的) */
	sg_dma_address(sg) = bo->dma_addr;
	sg_dma_len(sg) = params->size;

	bo->base.sgt = sgt;

	dev_info(vgdev->ddev->dev, "DEBUG: SG setup: dma_addr=0x%llx, len=%u\n",
		 (u64)sg_dma_address(sg), sg_dma_len(sg));

	/* 4. 标准后续流程 */
	ret = virtio_gpu_resource_id_get(vgdev, &bo->hw_res_handle);
	if (ret < 0)
		goto err_free_sgt;

	ents = virtio_gpu_mem_entries_from_sgt(bo->base.sgt, &nents);
	if (!ents || !nents) {
		ret = -ENOMEM;
		goto err_put_id;
	}

	bo->dumb = params->dumb;

	if (fence) {
		ret = -ENOMEM;
		objs = virtio_gpu_array_alloc(1);
		if (!objs)
			goto err_put_id;
		virtio_gpu_array_add_obj(objs, &bo->base.base);
		ret = virtio_gpu_array_lock_resv(objs);
		if (ret != 0)
			goto err_put_objs;
	}

	if (params->virgl) {
		virtio_gpu_cmd_resource_create_3d(vgdev, bo, params, objs,
						  fence);
	} else {
		virtio_gpu_cmd_create_resource(vgdev, bo, params, objs, fence);
	}

	/* 5. 绑定资源 */
	virtio_gpu_object_attach(vgdev, bo, ents, nents);

	*bo_ptr = bo;
	return 0;

err_put_objs:
	virtio_gpu_array_put_free(objs);
err_put_id:
	virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
err_free_sgt:
	sg_free_table(sgt);
	kfree(sgt);
	bo->base.sgt = NULL;
err_free_dma:
	dma_free_coherent(vgdev->vdev->dev.parent, params->size, bo->base.vaddr,
			  bo->dma_addr);
	bo->base.vaddr = NULL;
err_free_gem:
	drm_gem_object_release(gem_obj);
	kfree(bo);
	return ret;
}