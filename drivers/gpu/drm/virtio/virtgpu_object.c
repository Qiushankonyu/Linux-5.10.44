/*
 * drivers/gpu/drm/virtio/virtgpu_object.c
 *
 * Strategy: Pure DMA-Coherent Object (No Shmem Dependencies)
 * Fixes:
 * 1. MMAP -22: Correctly calculates object-internal offset.
 * 2. PFN Handling: Uses dma_to_phys() for correct IOMMU support.
 * 3. Lifecycle: Fully decouples from drm_gem_shmem_*.
 */

#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/pfn.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_vma_manager.h>
#include "virtgpu_drv.h"

static int virtio_gpu_virglrenderer_workaround = 1;
module_param_named(virglhack, virtio_gpu_virglrenderer_workaround, int, 0400);

/* ===================================================================
 * 1. 核心操作函数 (The Correct Implementation)
 * =================================================================== */

/* [Free] 
 * 逻辑：只释放 DMA 内存和 GEM 句柄。
 * 关键：绝对不调用 drm_gem_shmem_free_object()，防止访问空 pages。
 */
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
	if (bo->base.vaddr && bo->dma_addr) {
		dev_info(obj->dev->dev, "DEBUG: Freeing DMA mem: vaddr=%p\n",
			 bo->base.vaddr);
		dma_free_coherent(vgdev->dma_dev, obj->size,
				  bo->base.vaddr, bo->dma_addr);
		bo->base.vaddr = NULL;
	}

	/* 3. 基础资源释放 */
	drm_gem_object_release(obj);
	kfree(bo);
}

/* [Mmap] 
 * 逻辑：修正 fake offset，计算物理地址，建立映射。
 * 关键：解决 modetest -22 错误。
 */
static int virtio_gpu_gem_mmap(struct drm_gem_object *obj,
			       struct vm_area_struct *vma)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long start_pgoff = drm_vma_node_start(&obj->vma_node);
	unsigned long off_pages;
	unsigned long off;
	phys_addr_t phys_addr;
	unsigned long saved_pgoff = vma->vm_pgoff;
	int ret;

	dev_info(
		vgdev->ddev->dev,
		"MMAP: vm_pgoff=%lx start=%lx size=%lu obj=%zu dma=0x%llx vaddr=%p\n",
		vma->vm_pgoff, start_pgoff, size, obj->size,
		(unsigned long long)bo->dma_addr, bo->base.vaddr);
	dev_info(vgdev->ddev->dev, "MMAP: mmap handler reached\n");

	/* 1. 设置 VMA 标志 (DMA 内存必须设为 IO/PFNMAP) */
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;

	/* 2. [CRITICAL FIX] 计算对象内的真实偏移 
     * DRM 传入的 vm_pgoff 是全局 token，必须减去 node_start。
     */
	if (vma->vm_pgoff < start_pgoff)
		return -EINVAL;

	off_pages = vma->vm_pgoff - start_pgoff;
	off = off_pages << PAGE_SHIFT;

	if (off + size > obj->size)
		return -EINVAL;

	/* 3. 先尝试标准 DMA 映射（使用对象内偏移） */
	vma->vm_pgoff = off_pages;
	ret = dma_mmap_coherent(vgdev->dma_dev, vma, bo->base.vaddr,
				bo->dma_addr, obj->size);
	vma->vm_pgoff = saved_pgoff;
	dev_info(vgdev->ddev->dev,
		 "MMAP: dma_mmap_coherent ret=%d, mmap handler reached\n", ret);
	if (!ret)
		return 0;

	/* 4. 回退：直接 remap 物理 PFN */
	phys_addr = dma_to_phys(vgdev->dma_dev, bo->dma_addr) + off;

	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	if (!dev_is_dma_coherent(vgdev->dma_dev))
		vma->vm_page_prot = pgprot_dmacoherent(vma->vm_page_prot);

	ret = remap_pfn_range(vma, vma->vm_start, phys_addr >> PAGE_SHIFT,
			      PAGE_ALIGN(size), vma->vm_page_prot);
	dev_info(vgdev->ddev->dev,
		 "MMAP: remap_pfn_range ret=%d, mmap handler reached\n", ret);
	return ret;
}

/* [Vmap] 
 * 逻辑：直接返回已有的内核虚拟地址。
 * 关键：去除所有 shmem 锁依赖，实现最小一致性。
 */
static void *virtio_gpu_gem_vmap(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	if (!bo->base.vaddr)
		return ERR_PTR(-ENOMEM);
	return bo->base.vaddr;
}

/* [Vunmap] 
 * 逻辑：空实现。因为 DMA 内存常驻，不需要 unmap。
 */
static void virtio_gpu_gem_vunmap(struct drm_gem_object *obj, void *vaddr)
{
}

/* [Pin/Unpin] 空实现，满足 API 要求 */
static int virtio_gpu_gem_pin(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);

	if (!bo->base.vaddr || !bo->base.sgt)
		return -EINVAL;

	return 0;
}
static void virtio_gpu_gem_unpin(struct drm_gem_object *obj)
{
}

/* [SG Table] 返回 SG 表 */
static struct sg_table *virtio_gpu_gem_get_sg_table(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct sg_table *sgt;
	struct scatterlist *sg;
	int ret;

	if (!bo->base.sgt)
		return ERR_PTR(-EINVAL);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	sg = sgt->sgl;
	sg_init_table(sg, 1);
	if (pfn_valid(PFN_DOWN(bo->dma_addr))) {
		sg_set_page(sg, pfn_to_page(PFN_DOWN(bo->dma_addr)), obj->size,
			    0);
	} else {
		sg->page_link = 0;
		sg->offset = 0;
		sg->length = obj->size;
	}

	sg_dma_address(sg) = bo->dma_addr;
	sg_dma_len(sg) = obj->size;

	return sgt;
}

static void virtio_gpu_gem_print_info(struct drm_printer *p,
				      unsigned int indent,
				      const struct drm_gem_object *obj)
{
	const struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);

	drm_printf_indent(p, indent, "dma_addr=0x%llx size=%zu\n",
			  (unsigned long long)bo->dma_addr, obj->size);
}

/* 函数表：完全自定义，不复用 drm_gem_shmem_* */
static const struct drm_gem_object_funcs virtio_gpu_gem_funcs = {
	.free = virtio_gpu_free_shared_object,
	.open = virtio_gpu_gem_object_open,
	.close = virtio_gpu_gem_object_close,
	.print_info = virtio_gpu_gem_print_info,
	.pin = virtio_gpu_gem_pin,
	.unpin = virtio_gpu_gem_unpin,
	.get_sg_table = virtio_gpu_gem_get_sg_table,
	.vmap = virtio_gpu_gem_vmap,
	.vunmap = virtio_gpu_gem_vunmap,
	.mmap = virtio_gpu_gem_mmap,
};

/* ===================================================================
 * 2. 辅助函数 (保持精简)
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

bool virtio_gpu_is_shmem(struct virtio_gpu_object *bo)
{
	return bo->base.base.funcs == &virtio_gpu_gem_funcs;
}

void virtio_gpu_cleanup_object(struct virtio_gpu_object *bo)
{
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;
	virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
	if (virtio_gpu_is_shmem(bo)) {
		drm_gem_object_put(&bo->base.base);
	}
}

/* Dummy 占位符 */
static void __maybe_unused virtio_gpu_free_object(struct drm_gem_object *obj)
{
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
		ents[i].addr = cpu_to_le64(sg_dma_address(sg));
		ents[i].length = cpu_to_le32(sg_dma_len(sg));
	}
	*nents = n;
	return ents;
}

/* ===================================================================
 * 3. 创建逻辑 (Create)
 * =================================================================== */

struct drm_gem_object *virtio_gpu_create_object(struct drm_device *dev,
						size_t size)
{
	struct virtio_gpu_object *bo;
	struct drm_gem_object *obj;
	int ret;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	obj = &bo->base.base;
	obj->funcs = &virtio_gpu_gem_funcs;

	drm_gem_private_object_init(dev, obj, size);
	bo->base.map_cached = true;
	dev_info(dev->dev, "GEM_CREATE: obj=%p size=%zu\n", obj, size);

	ret = drm_gem_create_mmap_offset(obj);
	dev_info(dev->dev, "GEM_CREATE: mmap_offset ret=%d offset=0x%lx\n", ret,
		 drm_vma_node_start(&obj->vma_node));
	if (ret) {
		drm_gem_object_release(obj);
		kfree(bo);
		return ERR_PTR(ret);
	}
	return obj;
}

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

	*bo_ptr = NULL;
	params->size = roundup(params->size, PAGE_SIZE);

	/* 1. Create */
	gem_obj = virtio_gpu_create_object(vgdev->ddev, params->size);
	if (IS_ERR(gem_obj))
		return PTR_ERR(gem_obj);
	bo = gem_to_virtio_gpu_obj(gem_obj);

	dev_info(vgdev->ddev->dev,
		 "OBJ_CREATE: size=%lu width=%u height=%u fmt=0x%x dumb=%d\n",
		 (unsigned long)params->size, params->width, params->height,
		 params->format, params->dumb);
	/* 2. DMA Alloc */
	vaddr = dma_alloc_coherent(vgdev->dma_dev, params->size,
				   &bo->dma_addr, GFP_KERNEL);
	if (!vaddr) {
		ret = -ENOMEM;
		goto err_free_gem;
	}

	/* Type-1/shared-memory 场景：必须严格落在 reserved-memory 池子里 */
	if (vgdev->vpu_shm_size > 0) {
		if (bo->dma_addr < vgdev->vpu_shm_start ||
		    bo->dma_addr + params->size >
			    vgdev->vpu_shm_start + vgdev->vpu_shm_size) {
			dev_err(vgdev->ddev->dev,
				"FAIL: OBJ_CREATE outside shared-memory (dma=0x%llx size=0x%lx shm=[0x%llx-0x%llx))\n",
				(unsigned long long)bo->dma_addr,
				(unsigned long)params->size,
				(unsigned long long)vgdev->vpu_shm_start,
				(unsigned long long)(vgdev->vpu_shm_start + vgdev->vpu_shm_size));
			dma_free_coherent(vgdev->dma_dev, params->size, vaddr,
					  bo->dma_addr);
			ret = -EINVAL;
			goto err_free_gem;
		}
	}
	bo->base.vaddr = vaddr;

	/* 3. Manual SG Table */
	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto err_free_dma;
	}
	dev_info(vgdev->ddev->dev,
		 "OBJ_CREATE: dma_alloc vaddr=%p dma=0x%llx\n", vaddr,
		 (unsigned long long)bo->dma_addr);
	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		goto err_free_dma;
	}

	sg = sgt->sgl;
	sg_init_table(sg, 1);
	sg->page_link = 0; /* No struct page */
	sg->offset = 0;
	sg->length = params->size;
	sg_dma_address(sg) = bo->dma_addr;
	sg_dma_len(sg) = params->size;
	bo->base.sgt = sgt;

	/* 4. GPU Resource Init */
	ret = virtio_gpu_resource_id_get(vgdev, &bo->hw_res_handle);
	if (ret < 0)
		goto err_free_sgt;

	ents = virtio_gpu_mem_entries_from_sgt(bo->base.sgt, &nents);
	dev_info(vgdev->ddev->dev, "OBJ_CREATE: sgt dma=0x%llx len=%u\n",
		 (unsigned long long)bo->dma_addr, sg_dma_len(sg));
	if (!ents) {
		ret = -ENOMEM;
		goto err_put_id;
	}

	dev_info(vgdev->ddev->dev, "OBJ_CREATE: hw_res_handle=%u\n",
		 bo->hw_res_handle);
	bo->dumb = params->dumb;

	if (fence) {
		objs = virtio_gpu_array_alloc(1);
		if (!objs) {
			ret = -ENOMEM;
			goto err_put_id;
		}
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

	virtio_gpu_object_attach(vgdev, bo, ents, nents);
	*bo_ptr = bo;
	return 0;

err_put_objs:
	virtio_gpu_array_put_free(objs);
	dev_info(vgdev->ddev->dev, "OBJ_CREATE: resource created\n");
err_put_id:
	virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
	dev_info(vgdev->ddev->dev, "OBJ_CREATE: object attached\n");
err_free_sgt:
	sg_free_table(sgt);
	kfree(sgt);
	bo->base.sgt = NULL;
err_free_dma:
	dma_free_coherent(vgdev->dma_dev, params->size, bo->base.vaddr,
			  bo->dma_addr);
err_free_gem:
	drm_gem_object_release(gem_obj);
	kfree(bo);
	return ret;
}