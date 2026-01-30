/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>

#include "virtgpu_drv.h"

static int virtio_gpu_virglrenderer_workaround = 1;
module_param_named(virglhack, virtio_gpu_virglrenderer_workaround, int, 0400);

static int virtio_gpu_resource_id_get(struct virtio_gpu_device *vgdev,
				       uint32_t *resid)
{
	if (virtio_gpu_virglrenderer_workaround) {
		/*
		 * Hack to avoid re-using resource IDs.
		 *
		 * virglrenderer versions up to (and including) 0.7.0
		 * can't deal with that.  virglrenderer commit
		 * "f91a9dd35715 Fix unlinking resources from hash
		 * table." (Feb 2019) fixes the bug.
		 */
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

static void virtio_gpu_resource_id_put(struct virtio_gpu_device *vgdev, uint32_t id)
{
	if (!virtio_gpu_virglrenderer_workaround) {
		ida_free(&vgdev->resource_ida, id - 1);
	}
}

void virtio_gpu_cleanup_object(struct virtio_gpu_object *bo)
{
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;

	virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
	if (virtio_gpu_is_shmem(bo)) {
		struct virtio_gpu_object_shmem *shmem = to_virtio_gpu_shmem(bo);

		if (shmem->pages) {
			if (shmem->mapped) {
				dma_unmap_sgtable(vgdev->vdev->dev.parent,
					     shmem->pages, DMA_TO_DEVICE, 0);
				shmem->mapped = 0;
			}

			sg_free_table(shmem->pages);
			kfree(shmem->pages);
			shmem->pages = NULL;
			drm_gem_shmem_unpin(&bo->base.base);
		}

		drm_gem_shmem_free_object(&bo->base.base);
	}
}

static void virtio_gpu_free_object(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;

	if (bo->created) {
		virtio_gpu_cmd_unref_resource(vgdev, bo);
		virtio_gpu_notify(vgdev);
		/* completion handler calls virtio_gpu_cleanup_object() */
		return;
	}
	virtio_gpu_cleanup_object(bo);
}

static const struct drm_gem_object_funcs virtio_gpu_shmem_funcs = {
	.free = virtio_gpu_free_object,
	.open = virtio_gpu_gem_object_open,
	.close = virtio_gpu_gem_object_close,

	.print_info = drm_gem_shmem_print_info,
	.pin = drm_gem_shmem_pin,
	.unpin = drm_gem_shmem_unpin,
	.get_sg_table = drm_gem_shmem_get_sg_table,
	.vmap = drm_gem_shmem_vmap,
	.vunmap = drm_gem_shmem_vunmap,
	.mmap = drm_gem_shmem_mmap,
};

bool virtio_gpu_is_shmem(struct virtio_gpu_object *bo)
{
	return bo->base.base.funcs == &virtio_gpu_shmem_funcs;
}

struct drm_gem_object *virtio_gpu_create_object(struct drm_device *dev,
						size_t size)
{
	struct virtio_gpu_object_shmem *shmem;
	struct drm_gem_shmem_object *dshmem;

	shmem = kzalloc(sizeof(*shmem), GFP_KERNEL);
	if (!shmem)
		return NULL;

	dshmem = &shmem->base.base;
	dshmem->base.funcs = &virtio_gpu_shmem_funcs;
	dshmem->map_cached = true;
	return &dshmem->base;
}

static int virtio_gpu_object_shmem_init(struct virtio_gpu_device *vgdev,
					struct virtio_gpu_object *bo,
					struct virtio_gpu_mem_entry **ents,
					unsigned int *nents)
{
	bool use_dma_api = !virtio_has_dma_quirk(vgdev->vdev);
	struct virtio_gpu_object_shmem *shmem = to_virtio_gpu_shmem(bo);
	struct scatterlist *sg;
	int si, ret;

	ret = drm_gem_shmem_pin(&bo->base.base);
	if (ret < 0)
		return -EINVAL;

	/*
	 * virtio_gpu uses drm_gem_shmem_get_sg_table instead of
	 * drm_gem_shmem_get_pages_sgt because virtio has it's own set of
	 * dma-ops. This is discouraged for other drivers, but should be fine
	 * since virtio_gpu doesn't support dma-buf import from other devices.
	 */
	shmem->pages = drm_gem_shmem_get_sg_table(&bo->base.base);
	if (!shmem->pages) {
		drm_gem_shmem_unpin(&bo->base.base);
		return -EINVAL;
	}

	if (use_dma_api) {
		ret = dma_map_sgtable(vgdev->vdev->dev.parent,
				      shmem->pages, DMA_TO_DEVICE, 0);
		if (ret)
			return ret;
		*nents = shmem->mapped = shmem->pages->nents;
	} else {
		*nents = shmem->pages->orig_nents;
	}

	*ents = kvmalloc_array(*nents,
			       sizeof(struct virtio_gpu_mem_entry),
			       GFP_KERNEL);
	if (!(*ents)) {
		DRM_ERROR("failed to allocate ent list\n");
		return -ENOMEM;
	}

	if (use_dma_api) {
		for_each_sgtable_dma_sg(shmem->pages, sg, si) {
			(*ents)[si].addr = cpu_to_le64(sg_dma_address(sg));
			(*ents)[si].length = cpu_to_le32(sg_dma_len(sg));
			(*ents)[si].padding = 0;
		}
	} else {
		for_each_sgtable_sg(shmem->pages, sg, si) {
			(*ents)[si].addr = cpu_to_le64(sg_phys(sg));
			(*ents)[si].length = cpu_to_le32(sg->length);
			(*ents)[si].padding = 0;
		}
	}

	return 0;
}

int virtio_gpu_object_create(struct virtio_gpu_device *vgdev,
			     struct virtio_gpu_object_params *params,
			     struct virtio_gpu_object **bo_ptr,
			     struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_object_array *objs = NULL;
	struct drm_gem_shmem_object *shmem_obj;
	struct virtio_gpu_object *bo;
	struct virtio_gpu_mem_entry *ents;
	unsigned int nents;
	int ret;

	*bo_ptr = NULL;

	params->size = roundup(params->size, PAGE_SIZE);
	shmem_obj = drm_gem_shmem_create(vgdev->ddev, params->size);
	if (IS_ERR(shmem_obj))
		return PTR_ERR(shmem_obj);
	bo = gem_to_virtio_gpu_obj(&shmem_obj->base);

	ret = virtio_gpu_resource_id_get(vgdev, &bo->hw_res_handle);
	if (ret < 0)
		goto err_free_gem;

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
		virtio_gpu_cmd_resource_create_3d(vgdev, bo, params,
						  objs, fence);
	} else {
		virtio_gpu_cmd_create_resource(vgdev, bo, params,
					       objs, fence);
	}

	ret = virtio_gpu_object_shmem_init(vgdev, bo, &ents, &nents);
	if (ret != 0) {
		virtio_gpu_free_object(&shmem_obj->base);
		return ret;
	}

	virtio_gpu_object_attach(vgdev, bo, ents, nents);

	*bo_ptr = bo;
	return 0;

err_put_objs:
	virtio_gpu_array_put_free(objs);
err_put_id:
	virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
err_free_gem:
	drm_gem_shmem_free_object(&shmem_obj->base);
	return ret;
}

// int virtio_gpu_object_create(struct virtio_gpu_device *vgdev,
//                              struct virtio_gpu_object_params *params,
//                              struct virtio_gpu_object **bo_ptr,
//                              struct virtio_gpu_fence *fence)
// {
//     struct virtio_gpu_object_array *objs = NULL;
//     struct drm_gem_shmem_object *shmem_obj;
//     struct virtio_gpu_object *bo;
//     struct virtio_gpu_mem_entry *ents;
//     unsigned int nents;
//     int ret;

//     /* === 新增变量：用于暂存申请到的 VPU 地址 === */
//     unsigned long vpu_vaddr = 0; 

//     *bo_ptr = NULL;

//     params->size = roundup(params->size, PAGE_SIZE);

//     /* === 修改 1: 尝试从 VPU 池申请内存 === */
//     if (vgdev->vpu_pool) {
//         vpu_vaddr = gen_pool_alloc(vgdev->vpu_pool, params->size);
//         if (vpu_vaddr) {
//             printk(KERN_INFO "[VPU] Alloc success from pool. Size: %lu\n", params->size);
// 			/* === 新增实验代码 Start === */
//             /* 0xFF 代表全 1，在像素里通常是白色或亮色 */
//             /* 既然 cat 写不进去，我们就在这里强行把显存涂白！ */
//             memset((void *)vpu_vaddr, 0xFF, params->size);
//             printk(KERN_INFO "[VPU] Debug: Filled memory with 0xFF (White)\n");
//             /* === 新增实验代码 End === */
//         }
//     }
//     /* === 结束修改 1 === */

//     /* 这里的 shmem_create 依然要调用，我们需要利用它创建基本的 GEM 对象结构体 */
//     shmem_obj = drm_gem_shmem_create(vgdev->ddev, params->size);
//     if (IS_ERR(shmem_obj)) {
//         /* 如果对象创建失败，别忘了把刚才申请到的 VPU 内存还回去 */
//         if (vpu_vaddr)
//             gen_pool_free(vgdev->vpu_pool, vpu_vaddr, params->size);
//         return PTR_ERR(shmem_obj);
//     }
//     bo = gem_to_virtio_gpu_obj(&shmem_obj->base);

// 	bo->params = *params;

//     /* === 修改 2: 如果是从 VPU 申请的，填入地址信息 === */
//     if (vpu_vaddr) {
//         bo->vpu_vaddr = vpu_vaddr;
//         /* 计算物理地址：基地址 0xF0000000 + 偏移量 */
//         /* 偏移量 = (当前虚拟地址 - 映射区的起始虚拟地址) */
//         bo->vpu_paddr = 0xF0000000 + (vpu_vaddr - (unsigned long)vgdev->vpu_shm_virt);
        
//         /* 强制标记为 dumb，因为这是物理连续显存 */
//         bo->dumb = true; 
//     } else {
//         /* 如果没申请到，就维持原样 */
//         bo->dumb = params->dumb;
//     }
//     /* === 结束修改 2 === */

//     ret = virtio_gpu_resource_id_get(vgdev, &bo->hw_res_handle);
//     if (ret < 0)
//         goto err_free_gem;

//     /* bo->dumb = params->dumb;  <-- 这行被移到上面处理了 */

//     if (fence) {
//         ret = -ENOMEM;
//         objs = virtio_gpu_array_alloc(1);
//         if (!objs)
//             goto err_put_id;
//         virtio_gpu_array_add_obj(objs, &bo->base.base);

//         ret = virtio_gpu_array_lock_resv(objs);
//         if (ret != 0)
//             goto err_put_objs;
//     }

//     if (params->virgl) {
//         virtio_gpu_cmd_resource_create_3d(vgdev, bo, params,
//                           objs, fence);
//     } else {
//         virtio_gpu_cmd_create_resource(vgdev, bo, params,
//                            objs, fence);
//     }

//     /* === 修改 3: 这里的初始化逻辑分叉 === */
//     if (bo->vpu_vaddr) {
//         /* 如果是 VPU 内存，不需要初始化 shmem sg table (因为我们有物理地址了) */
//         ents = NULL;
//         nents = 0;
//     } else {
//         /* 原有的标准流程：申请系统 RAM 并建立 scatterlist */
//         ret = virtio_gpu_object_shmem_init(vgdev, bo, &ents, &nents);
//         if (ret != 0) {
//             virtio_gpu_free_object(&shmem_obj->base);
//             return ret;
//         }
//     }
//     /* === 结束修改 3 === */

//     /* 注意：virtio_gpu_object_attach 函数我们也需要修改，让它能处理 ents 为 NULL 的情况 */
//     virtio_gpu_object_attach(vgdev, bo, ents, nents);

//     *bo_ptr = bo;
//     return 0;

// err_put_objs:
//     virtio_gpu_array_put_free(objs);
// err_put_id:
//     virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
// err_free_gem:
//     if (vpu_vaddr) /* 如果出错，别忘了释放 VPU 内存 */
//         gen_pool_free(vgdev->vpu_pool, vpu_vaddr, params->size);
//     drm_gem_shmem_free_object(&shmem_obj->base);
//     return ret;
// }