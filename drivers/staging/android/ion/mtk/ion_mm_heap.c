/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <asm/page.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/mutex.h>
//#include <mmprofile.h>
//#include <mmprofile_function.h>
#include <linux/debugfs.h>
#include <linux/kthread.h>
#include <linux/fdtable.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/sched/clock.h>
#include "mtk/mtk_ion.h"
#include "ion_profile.h"
#include "ion_drv_priv.h"
#include "ion_fb_heap.h"
#include "ion_priv.h"
#include "mtk/ion_drv.h"
#include "ion_sec_heap.h"
#include "aee.h"

//tablet
#ifdef CONFIG_MTK_IOMMU
#include "pseudo_m4u.h"
#endif
//smart phone m4u
#ifdef CONFIG_MTK_M4U
#include <m4u.h>
#endif
//smart phone iommu
#ifdef CONFIG_MTK_IOMMU_V2
#include <mach/pseudo_m4u.h>
#include "mtk_iommu_ext.h"
#endif

struct ion_mm_buffer_info {
	int module_id;
	int fix_module_id;
	unsigned int security;
	unsigned int coherent;
	unsigned int mva_cnt;
	void *VA;
	unsigned int MVA[DOMAIN_NUM];
	unsigned int FIXED_MVA[DOMAIN_NUM];
	unsigned int iova_start[DOMAIN_NUM];
	unsigned int iova_end[DOMAIN_NUM];
	/*  same as module_id for 1 domain */
	int port[DOMAIN_NUM];
#if defined(CONFIG_MTK_IOMMU_PGTABLE_EXT) && \
	(CONFIG_MTK_IOMMU_PGTABLE_EXT > 32)
	struct sg_table table[DOMAIN_NUM];
#endif
	struct ion_mm_buf_debug_info dbg_info;
	ion_mm_buf_destroy_callback_t *destroy_fn;
	pid_t pid;
	struct mutex lock;/* buffer lock */
};

#define ION_DUMP(seq_files, fmt, args...) \
	do {\
		struct seq_file *file = (struct seq_file *)seq_files;\
	    char *fmat = fmt;\
		if (file)\
			seq_printf(file, fmat, ##args);\
		else\
			printk(fmat, ##args);\
	} while (0)

static unsigned int order_gfp_flags[] = {
	(GFP_HIGHUSER | __GFP_ZERO | __GFP_NOWARN | __GFP_NORETRY) &
	    ~__GFP_RECLAIM,
	(GFP_HIGHUSER | __GFP_ZERO | __GFP_NOWARN | __GFP_NORETRY) &
	    ~__GFP_DIRECT_RECLAIM,
	(GFP_HIGHUSER | __GFP_ZERO)
};

static const unsigned int orders[] = { 4, 1, 0 };

/* static const unsigned int orders[] = {8, 4, 0}; */
static const int num_orders = ARRAY_SIZE(orders);
static int order_to_index(unsigned int order)
{
	int i;

	for (i = 0; i < num_orders; i++)
		if (order == orders[i])
			return i;
	/*BUG(); */
	return -1;
}

static unsigned int order_to_size(int order)
{
	return PAGE_SIZE << order;
}

struct ion_system_heap {
	struct ion_heap heap;
	struct ion_page_pool **pools;
	struct ion_page_pool **cached_pools;
};

struct page_info {
	struct page *page;
	unsigned int order;
	struct list_head list;
};

unsigned int caller_pid;
unsigned int caller_tid;
unsigned long long alloc_large_fail_ts;

static struct page *alloc_buffer_page(struct ion_system_heap *heap,
				      struct ion_buffer *buffer,
				      unsigned long order)
{
	bool cached = ion_buffer_cached(buffer);
	struct ion_page_pool *pool;
	struct page *page;

	if (!cached)
		pool = heap->pools[order_to_index(order)];
	else
		pool = heap->cached_pools[order_to_index(order)];

	page = ion_page_pool_alloc(pool);

	if (!page) {
		IONMSG("[ion_dbg] alloc_pages order=%lu cache=%d\n",
		       order, cached);
		alloc_large_fail_ts = sched_clock();
		return NULL;
	}

	return page;
}

static void free_buffer_page(struct ion_system_heap *heap,
			     struct ion_buffer *buffer, struct page *page,
			     unsigned int order)
{
	bool cached = ion_buffer_cached(buffer);
	int order_idx = order_to_index(order);
	unsigned long private_flags = buffer->private_flags;

	if (!cached && !(private_flags & ION_PRIV_FLAG_SHRINKER_FREE)) {
		struct ion_page_pool *pool = heap->pools[order_idx];

		ion_page_pool_free(pool, page);
	} else if (cached && !(private_flags & ION_PRIV_FLAG_SHRINKER_FREE)) {
		struct ion_page_pool *pool = heap->cached_pools[order_idx];

		ion_page_pool_free(pool, page);
	} else {
		__free_pages(page, order);
		if (atomic64_sub_return((1 << order), &page_sz_cnt) < 0) {
			IONMSG("underflow!, total_now[%ld]free[%lu]\n",
			       atomic64_read(&page_sz_cnt),
			       (unsigned long)(1 << order));
			atomic64_set(&page_sz_cnt, 0);
		}
	}
}

static struct page_info *alloc_largest_available(struct ion_system_heap *heap,
						 struct ion_buffer *buffer,
						 unsigned long size,
						 unsigned int max_order)
{
	struct page *page;
	struct page_info *info;
	int i;

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		IONMSG("%s kmalloc failed info is null.\n", __func__);
		return NULL;
	}

	for (i = 0; i < num_orders; i++) {
		if (size < order_to_size(orders[i]))
			continue;
		if (max_order < orders[i])
			continue;

		page = alloc_buffer_page(heap, buffer, orders[i]);
		if (!page)
			continue;

		info->page = page;
		info->order = orders[i];
		INIT_LIST_HEAD(&info->list);
		return info;
	}
	kfree(info);

	return NULL;
}

static int ion_mm_pool_total(struct ion_system_heap *heap,
			     unsigned long order, bool cached)
{
	struct ion_page_pool *pool;
	int count;

	if (!cached) {
		pool = heap->pools[order_to_index(order)];
		count = pool->low_count + pool->high_count;
	} else {
		pool = heap->cached_pools[order_to_index(order)];
		count = (pool->low_count + pool->high_count);
	}

	return count;
}

static int ion_get_domain_id(int from_kernel, int *port)
{
	int domain_idx = 0;
	unsigned int port_id = *port;

#ifdef CONFIG_MTK_IOMMU_V2
	if (port_id >= M4U_PORT_UNKNOWN) {
#if defined(CONFIG_MTK_IOMMU_PGTABLE_EXT) && \
	(CONFIG_MTK_IOMMU_PGTABLE_EXT > 32)
		IONMSG("invalid port%d\n", *port);
		return -EFAULT;
#else
		return 0;
#endif
	}

	if (!from_kernel) {
		*port =	m4u_user2kernel_port(port_id);
		if (*port < 0 ||
		    *port >= M4U_PORT_UNKNOWN) {
			IONMSG("err, convert port%d to %d\n",
			       port_id, *port);
			return -EFAULT;
		}
	}
#if defined(CONFIG_MTK_IOMMU_PGTABLE_EXT) && \
	(CONFIG_MTK_IOMMU_PGTABLE_EXT > 32)
	domain_idx = m4u_get_boundary(*port);
	if (domain_idx < 0) {
		IONMSG("%s: invalid boundary:%d of port:%d\n",
		       __func__, domain_idx, *port);
		return -EFAULT;
	}
#else
	domain_idx = 0;
#endif //CONFIG_MTK_IOMMU_PGTABLE_EXT
#else  //CONFIG_MTK_IOMMU_V2
#if defined(CONFIG_MACH_MT6779) || defined(CONFIG_MACH_MT6785)
	if (port_id >= M4U_PORT_VPU)
		domain_idx = 1;
	else
		domain_idx = 0;
#else
	domain_idx = 0;
#endif
#endif //CONFIG_MTK_IOMMU_V2

	return domain_idx;
}

static int ion_mm_heap_phys(struct ion_heap *heap, struct ion_buffer *buffer,
			    ion_phys_addr_t *addr, size_t *len);

static int ion_mm_heap_allocate(struct ion_heap *heap,
				struct ion_buffer *buffer, unsigned long size,
				unsigned long align, unsigned long flags)
{
	struct ion_system_heap
	*sys_heap = container_of(heap,
				 struct ion_system_heap,
				 heap);
	struct sg_table *table = NULL;
	struct scatterlist *sg;
	int ret;
	struct list_head pages;
	struct page_info *info = NULL;
	struct page_info *tmp_info = NULL;
	int i = 0;
	unsigned long size_remaining = PAGE_ALIGN(size);
	unsigned int max_order = orders[0];
	struct ion_mm_buffer_info *buffer_info = NULL;
	unsigned long long start, end;
	unsigned long user_va = 0;
#ifdef CONFIG_MTK_PSEUDO_M4U
	struct page *page;
#endif

	INIT_LIST_HEAD(&pages);

#if (defined(CONFIG_MTK_M4U) || defined(CONFIG_MTK_PSEUDO_M4U))
	if (heap->id == ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA) {
		/*for va-->mva case, align is used for va value */
		table = m4u_create_sgtable(align, (unsigned int)size);
		user_va = align;
		if (size % PAGE_SIZE != 0)
			IONDBG("%s va(0x%lx)size(%ld) not align page.\n",
			       __func__, user_va, size);
		if (IS_ERR_OR_NULL(table)) {
			IONMSG("%s create table error 0x%p!!\n",
			       __func__, table);
			return -ENOMEM;
		}

		goto map_mva_exit;
	}

	if (heap->id == ION_HEAP_TYPE_MULTIMEDIA_PA2MVA) {
		table = kzalloc(sizeof(*table), GFP_KERNEL);
		if (!table) {
			IONMSG("%s kzalloc failed table is null.\n", __func__);
			goto err;
		}
		ret = sg_alloc_table(table, 1, GFP_KERNEL);
		if (ret) {
			IONMSG("%s PA2MVA sg table fail %d\n", __func__, ret);
			goto err1;
		}
		sg_dma_address(table->sgl) = align;
		sg_dma_len(table->sgl) = size;
		table->sgl->length = size;
#ifdef CONFIG_MTK_PSEUDO_M4U
		page = phys_to_page(align);
		sg_set_page(table->sgl, page, size, 0);
#endif

		goto map_mva_exit;
	}
#endif
	if (align > PAGE_SIZE) {
		IONMSG("%s align %lu is larger than PAGE_SIZE.\n", __func__,
		       align);
		return -EINVAL;
	}

	if (size / PAGE_SIZE > totalram_pages / 2) {
		IONMSG("%s size %lu is larger than totalram_pages.\n", __func__,
		       size);
		return -ENOMEM;
	}

	start = sched_clock();

	/* add time interval to alloc 64k page in low memory status*/
	if (((start - alloc_large_fail_ts) < 1000000000) &&
	    (ion_mm_pool_total(sys_heap, orders[0],
				ion_buffer_cached(buffer)) < 10))
		max_order = orders[1];

	caller_pid = (unsigned int)current->pid;
	caller_tid = (unsigned int)current->tgid;

	while (size_remaining > 0) {
		info = alloc_largest_available(sys_heap, buffer, size_remaining,
					       max_order);
		if (!info) {
			IONMSG("%s alloc_largest_available failed info\n",
			       __func__);
			goto err;
		}
		list_add_tail(&info->list, &pages);
		size_remaining -= (1 << info->order) * PAGE_SIZE;
		max_order = info->order;
		i++;
	}
	end = sched_clock();

	if (end - start > 10000000ULL) {	/* unit is ns, 10ms */
		IONMSG(" %s warn: size: %lu time: %lld ns --%d\n",
		       __func__, size, end - start, heap->id);
	}
	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		IONMSG("%s kzalloc failed table is null.\n", __func__);
		goto err;
	}

	ret = sg_alloc_table(table, i, GFP_KERNEL);
	if (ret) {
		IONMSG("%s sg alloc table failed %d.\n", __func__, ret);
		goto err1;
	}

	sg = table->sgl;
	list_for_each_entry_safe(info, tmp_info, &pages, list) {
		struct page *page = info->page;

		sg_set_page(sg, page, (1 << info->order) * PAGE_SIZE, 0);
		sg = sg_next(sg);
		list_del(&info->list);
		kfree(info);
	}

#if (defined(CONFIG_MTK_M4U) || defined(CONFIG_MTK_PSEUDO_M4U))
map_mva_exit:
#endif
	/* create MM buffer info for it */
	buffer_info = kzalloc(sizeof(*buffer_info), GFP_KERNEL);
	if (IS_ERR_OR_NULL(buffer_info)) {
		IONMSG(" %s: Error. alloc ion_buffer failed.\n", __func__);
		goto err1;
	}

	buffer->sg_table = table;
	buffer_info->VA = (void *)user_va;
	for (i = 0; i < DOMAIN_NUM; i++) {
		buffer_info->MVA[i] = 0;
		buffer_info->FIXED_MVA[i] = 0;
		buffer_info->iova_start[i] = 0;
		buffer_info->iova_end[i] = 0;
		buffer_info->port[i] = -1;
#if defined(CONFIG_MTK_IOMMU_PGTABLE_EXT) && \
	(CONFIG_MTK_IOMMU_PGTABLE_EXT > 32)
		clone_sg_table(buffer->sg_table,
			       &buffer_info->table[i]);
#endif
	}
	buffer_info->module_id = -1;
	buffer_info->fix_module_id = -1;
	buffer_info->mva_cnt = 0;
	buffer_info->dbg_info.value1 = 0;
	buffer_info->dbg_info.value2 = 0;
	buffer_info->dbg_info.value3 = 0;
	buffer_info->dbg_info.value4 = 0;
	buffer_info->pid = buffer->pid;
	mutex_init(&buffer_info->lock);
	strncpy((buffer_info->dbg_info.dbg_name), "nothing",
		ION_MM_DBG_NAME_LEN);
	buffer->size = size;
	buffer->priv_virt = buffer_info;

	caller_pid = 0;
	caller_tid = 0;

	return 0;
err1:
	kfree(table);
	IONMSG("error: alloc for sg_table fail\n");
err:
	if (!list_empty(&pages)) {
		list_for_each_entry_safe(info, tmp_info, &pages, list) {
			free_buffer_page(sys_heap, buffer, info->page,
					 info->order);
			kfree(info);
		}
	}
	IONMSG("error: mm_alloc fail: size=%lu, flag=%lu.\n", size, flags);
	caller_pid = 0;
	caller_tid = 0;

	return -ENOMEM;
}

int ion_mm_heap_register_buf_destroy_cb(struct ion_buffer *buffer,
					ion_mm_buf_destroy_callback_t *fn)
{
	struct ion_mm_buffer_info *buffer_info =
	    (struct ion_mm_buffer_info *)buffer->priv_virt;

	if (buffer_info)
		buffer_info->destroy_fn = fn;
	return 0;
}

void ion_mm_heap_free_buffer_info(struct ion_buffer *buffer)
{
	struct sg_table *table = buffer->sg_table;
	struct ion_mm_buffer_info *buffer_info =
	    (struct ion_mm_buffer_info *)buffer->priv_virt;
	unsigned int free_mva = 0;
	int domain_idx = 0, port = -1, ret = 0;

	buffer->priv_virt = NULL;
	if (!buffer_info)
		return;
	if (buffer_info->mva_cnt == 0)
		goto out;

	IONDBG(
		"ion free mva_cnt:%u, domian:%d",
		buffer_info->mva_cnt, domain_idx);

	for (domain_idx = 0;
		domain_idx < DOMAIN_NUM; domain_idx++) {
		IONDBG("mva[%d]:0x%x -- 0x%x,", domain_idx,
		       buffer_info->MVA[domain_idx],
		       buffer_info->FIXED_MVA[domain_idx]);
		if (buffer_info->MVA[domain_idx] == 0 &&
		    buffer_info->FIXED_MVA[domain_idx] == 0)
			continue;

		if (buffer_info->destroy_fn &&
		    buffer_info->MVA[domain_idx])
			buffer_info->destroy_fn(buffer,
				buffer_info->MVA[domain_idx]);

#if (defined(CONFIG_MTK_M4U) || defined(CONFIG_MTK_PSEUDO_M4U))
		if (buffer_info->MVA[domain_idx]) {
			free_mva = buffer_info->MVA[domain_idx];
			port = buffer_info->port[domain_idx];
			ret = m4u_dealloc_mva_sg(port, table,
						 buffer->size, free_mva);
		}
		if (buffer_info->FIXED_MVA[domain_idx]) {
			free_mva =
				buffer_info->FIXED_MVA[domain_idx];
			port = buffer_info->port[domain_idx];
			ret = m4u_dealloc_mva_sg(port, table,
						 buffer->size, free_mva);
		}

		if (!ret) {
			buffer_info->mva_cnt--;
			buffer_info->port[domain_idx] = -1;
		} else {
			IONMSG(
			       "%s, %d, err free:0x%lx, mva:0x%lx, fix:0x%lx, port:%d\n",
			       __func__, __LINE__, free_mva,
			       buffer_info->MVA[domain_idx],
			       buffer_info->FIXED_MVA[domain_idx],
			       port);
		}

#ifdef CONFIG_MTK_PSEUDO_M4U
		if (free_mva)
			mmprofile_log_ex(
				ion_mmp_events[PROFILE_MVA_DEALLOC],
				MMPROFILE_FLAG_PULSE,
				free_mva,
				free_mva + buffer->size);
#endif
#endif
	}

out:
	if (buffer_info->mva_cnt == 0)
		kfree(buffer_info);
	else
		IONMSG("there are %d MVA not mapped, check MVA leakage\n",
		       buffer_info->mva_cnt);
}

void ion_mm_heap_free(struct ion_buffer *buffer)
{
	struct ion_heap *heap = buffer->heap;
	struct ion_system_heap *sys_heap =
	    container_of(heap, struct ion_system_heap, heap);
	struct sg_table *table = buffer->sg_table;
	struct scatterlist *sg;
	LIST_HEAD(pages);
	int i;

#if (defined(CONFIG_MTK_M4U) || defined(CONFIG_MTK_PSEUDO_M4U))
	if (heap->id == ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA ||
	    heap->id == ION_HEAP_TYPE_MULTIMEDIA_PA2MVA) {
		ion_mm_heap_free_buffer_info(buffer);
		return;
	}
#endif

	/* uncached pages come from the page pools, zero them before return */
	/*for security purposes (other allocations are zerod at alloc time */
	if (!(buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE))
		ion_heap_buffer_zero(buffer);

	ion_mm_heap_free_buffer_info(buffer);

	for_each_sg(table->sgl, sg, table->nents, i)
		free_buffer_page(sys_heap, buffer, sg_page(sg),
				 get_order(sg->length));

	sg_free_table(table);
	kfree(table);
}

struct sg_table *ion_mm_heap_map_dma(struct ion_heap *heap,
				     struct ion_buffer *buffer)
{
	return buffer->sg_table;
}

void ion_mm_heap_unmap_dma(struct ion_heap *heap, struct ion_buffer *buffer)
{
}

static int ion_mm_heap_shrink(struct ion_heap *heap, gfp_t gfp_mask,
			      int nr_to_scan)
{
	struct ion_system_heap *sys_heap;
	int nr_total = 0;
	int i;

	sys_heap = container_of(heap, struct ion_system_heap, heap);

	for (i = 0; i < num_orders; i++) {
		struct ion_page_pool *pool = sys_heap->pools[i];

		nr_total += ion_page_pool_shrink(pool, gfp_mask, nr_to_scan);
		/* shrink cached pool */
		nr_total +=
		    ion_page_pool_shrink(sys_heap->cached_pools[i], gfp_mask,
					 nr_to_scan);
	}

	return nr_total;
}

static void ion_buffer_dump(struct ion_buffer *buffer, struct seq_file *s)
{
	struct ion_mm_buffer_info *bug_info;
	struct ion_mm_buf_debug_info *pdbg;
	int val;

	if (!buffer)
		return;

	bug_info = (struct ion_mm_buffer_info *)buffer->priv_virt;

	if (!bug_info)
		return;
	pdbg = &bug_info->dbg_info;

	if (bug_info->fix_module_id >= 0)
		val = bug_info->fix_module_id;
	else
		val = bug_info->module_id;
#if (DOMAIN_NUM == 1)
	ION_DUMP(s,
		 "0x%p %8zu %3d %3d %3d %3d %3d %3lu(%3lu) 0x%x, 0x%x, %5d(%5d) %16s 0x%x 0x%x 0x%x 0x%x %s\n",
		 buffer, buffer->size, buffer->kmap_cnt,
		 atomic_read(&buffer->ref.refcount.refs),
		 buffer->handle_count, val,
		 bug_info->mva_cnt,
		 bug_info->MVA[0], bug_info->FIXED_MVA[0],
		 bug_info->security,
		 buffer->flags, buffer->pid, bug_info->pid,
		 buffer->task_comm, pdbg->value1,
		 pdbg->value2, pdbg->value3, pdbg->value4,
		 pdbg->dbg_name);
#elif (DOMAIN_NUM == 2)
	ION_DUMP(s,
		 "0x%p %8zu %3d %3d %3d %3d %3d %3lu(%3lu) %3lu(%3lu) 0x%x, 0x%x, %5d(%5d) %16s 0x%x 0x%x 0x%x 0x%x %s\n",
		 buffer, buffer->size, buffer->kmap_cnt,
		 atomic_read(&buffer->ref.refcount.refs),
		 buffer->handle_count, val,
		 bug_info->mva_cnt,
		 bug_info->MVA[0], bug_info->FIXED_MVA[0],
		 bug_info->MVA[1], bug_info->FIXED_MVA[1],
		 bug_info->security,
		 buffer->flags, buffer->pid, bug_info->pid,
		 buffer->task_comm, pdbg->value1,
		 pdbg->value2, pdbg->value3, pdbg->value4,
		 pdbg->dbg_name);
#elif (DOMAIN_NUM == 4)
	ION_DUMP(s,
		 "0x%p %8zu %3d %3d %3d %3d %3d %3lu %3lu %3lu %3lu 0x%x, 0x%x, %5d(%5d) %16s 0x%x 0x%x 0x%x 0x%x %s\n",
		 buffer, buffer->size, buffer->kmap_cnt,
		 atomic_read(&buffer->ref.refcount.refs),
		 buffer->handle_count, val,
		 bug_info->mva_cnt,
		 bug_info->MVA[0],
		 bug_info->MVA[1],
		 bug_info->MVA[2],
		 bug_info->MVA[3],
		 bug_info->security,
		 buffer->flags, buffer->pid, bug_info->pid,
		 buffer->task_comm, pdbg->value1,
		 pdbg->value2, pdbg->value3, pdbg->value4,
		 pdbg->dbg_name);
#endif
}

static int ion_mm_heap_phys(struct ion_heap *heap, struct ion_buffer *buffer,
			    ion_phys_addr_t *addr, size_t *len)
{
	struct ion_mm_buffer_info *buffer_info =
	    (struct ion_mm_buffer_info *)buffer->priv_virt;
	struct port_mva_info_t port_info;
	int ret = 0;
	bool non_vmalloc_request = false;
	int domain_idx = 0;

	if (!buffer_info) {
		IONMSG("[%s] Error. Invalid buffer.\n", __func__);
		return -EFAULT;	/* Invalid buffer */
	}

	if ((buffer_info->module_id == -1) &&
	    (buffer_info->fix_module_id == -1)) {
		IONMSG("[%s] warning. Buffer not configured.\n", __func__);
#if 1 //defined(CONFIG_MTK_IOMMU_PGTABLE_EXT) && \
	//(CONFIG_MTK_IOMMU_PGTABLE_EXT > 32)
		ion_buffer_dump(buffer, NULL);
#ifdef ION_DEBUG_IOMMU_34BIT_BUFFER
		aee_kernel_warning_api(__FILE__, __LINE__,
				       DB_OPT_DEFAULT |
				       DB_OPT_NATIVE_BACKTRACE,
				       "port name not matched",
				       "dump user backtrace");
#endif
#else
		//return -EFAULT;	/* Buffer not configured. */
#endif
	}

	memset((void *)&port_info, 0, sizeof(port_info));
	port_info.cache_coherent = buffer_info->coherent;
	port_info.security = buffer_info->security;
	port_info.buf_size = buffer->size;

	if (((*(unsigned int *)addr & 0xffff) == ION_FLAG_GET_FIXED_PHYS) &&
	    ((*(unsigned int *)len) == ION_FLAG_GET_FIXED_PHYS)) {
		port_info.flags = M4U_FLAGS_FIX_MVA;
		port_info.emoduleid =
		    buffer_info->fix_module_id;

		domain_idx = ion_get_domain_id(1,
					       &port_info.emoduleid);
		if (domain_idx < 0 ||
		    domain_idx >= DOMAIN_NUM) {
			IONMSG("%s, err, %d(%d)-%d\n",
			       __func__, port_info.emoduleid,
			       domain_idx, buffer->heap->type);
			ret = -EFAULT;
			goto out;
		}

		port_info.iova_start = buffer_info->iova_start[domain_idx];
		port_info.iova_end = buffer_info->iova_end[domain_idx];
		if (port_info.iova_start == port_info.iova_end)
			port_info.mva = port_info.iova_start;
	} else {
		port_info.emoduleid = buffer_info->module_id;

		domain_idx =
			ion_get_domain_id(1,
					  &port_info.emoduleid);
		if (domain_idx < 0 ||
		    domain_idx >= DOMAIN_NUM) {
			IONMSG("%s, err, %d(%d)-%d\n",
			       __func__, port_info.emoduleid,
			       domain_idx, buffer->heap->type);
			ret = -EFAULT;
			goto out;
		}
	}

#if defined(CONFIG_MTK_IOMMU_PGTABLE_EXT) && \
	(CONFIG_MTK_IOMMU_PGTABLE_EXT > 32)
	buffer->sg_table = &buffer_info->table[domain_idx];
#endif

	if ((buffer_info->MVA[domain_idx] == 0 && port_info.flags == 0) ||
	    (buffer_info->FIXED_MVA[domain_idx] == 0 &&
	    port_info.flags > 0)) {
		if (port_info.flags == 0 && buffer_info->module_id == -1) {
			IONMSG("%s: warning not config buffer\n", __func__);
#if defined(CONFIG_MTK_IOMMU_PGTABLE_EXT) && \
	(CONFIG_MTK_IOMMU_PGTABLE_EXT > 32)
			ion_buffer_dump(buffer, NULL);
#else
			//ret = -EFAULT;
			//goto out;
#endif
		}

#if (defined(CONFIG_MTK_M4U) || defined(CONFIG_MTK_PSEUDO_M4U))
		if (heap->id == ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA ||
		    heap->id == ION_HEAP_TYPE_MULTIMEDIA_PA2MVA) {
			port_info.va = (unsigned long)buffer_info->VA;
			port_info.flags |= M4U_FLAGS_SG_READY;
			/*userspace va without vmalloc, has no page struct */
			if (port_info.va < PAGE_OFFSET &&
			    (port_info.va < VMALLOC_START ||
			     port_info.va > VMALLOC_END))
				non_vmalloc_request = true;
		}
#endif

#if (defined(CONFIG_MTK_M4U) || defined(CONFIG_MTK_PSEUDO_M4U))
		ret = m4u_alloc_mva_sg(&port_info, buffer->sg_table);
#endif
		if (ret < 0) {
			IONMSG("[%s]Error: port %d MVA(0x%x), domain:%d",
			       __func__, port_info.emoduleid,
			       *(unsigned int *)addr, domain_idx);
			IONMSG("(region 0x%x-0x%x)(VA 0x%lx-%zu-%d)\n",
			       port_info.iova_start, port_info.iova_end,
			       (unsigned long)buffer_info->VA, buffer->size,
			       non_vmalloc_request);
			*addr = 0;
			if (port_info.flags > 0)
				buffer_info->FIXED_MVA[domain_idx] = 0;
			ret = -EFAULT;
			goto out;
		}

		*addr = port_info.mva;

		if ((port_info.flags & M4U_FLAGS_FIX_MVA) == 0)
			buffer_info->MVA[domain_idx] = port_info.mva;
		else
			buffer_info->FIXED_MVA[domain_idx] = port_info.mva;

		buffer_info->mva_cnt++;
		buffer_info->port[domain_idx] = port_info.emoduleid;
		IONDBG(
		       "%d, iova mapping done, buffer:0x%lx, port:%d, mva:0x%lx, fix:0x%lx, return:0x%lx, cnt=%d, domain%d\n",
		       __LINE__, buffer, buffer_info->port[domain_idx],
		       buffer_info->MVA[domain_idx],
		       buffer_info->FIXED_MVA[domain_idx],
		       port_info.mva, buffer_info->mva_cnt, domain_idx);

#ifdef CONFIG_MTK_PSEUDO_M4U
		mmprofile_log_ex(ion_mmp_events[PROFILE_MVA_ALLOC],
				 MMPROFILE_FLAG_PULSE,
				 port_info.mva,
				 port_info.mva + port_info.buf_size);
#endif

	} else {
		*(unsigned int *)addr = (port_info.flags
					 == M4U_FLAGS_FIX_MVA) ?
		    buffer_info->FIXED_MVA[domain_idx] :
				buffer_info->MVA[domain_idx];
	}

	if (port_info.flags > 0) {
		IONDBG("[%s] Port %d, in_len 0x%x, MVA(0x%x-%zu)",
		       __func__, port_info.emoduleid, *(unsigned int *)len,
		       *(unsigned int *)addr, buffer->size);
		IONDBG("(region 0x%x--0x%x) (VA 0x%lx-%d)\n",
		       buffer_info->iova_start[domain_idx],
		       buffer_info->iova_end[domain_idx],
		       (unsigned long)buffer_info->VA, non_vmalloc_request);
	}

	*len = buffer->size;
	ret = 0;

out:
#if defined(CONFIG_MTK_IOMMU_PGTABLE_EXT) && \
	(CONFIG_MTK_IOMMU_PGTABLE_EXT > 32)
	if ((port_info.flags & M4U_FLAGS_FIX_MVA) == 0)
		buffer_info->module_id = -1;
	else
		buffer_info->fix_module_id = -1;
#endif
	return ret;
}

int ion_mm_heap_pool_total(struct ion_heap *heap)
{
	struct ion_system_heap *sys_heap;
	int total = 0;
	int i;

	sys_heap = container_of(heap, struct ion_system_heap, heap);

	for (i = 0; i < num_orders; i++) {
		struct ion_page_pool *pool = sys_heap->pools[i];

		total +=
		    (pool->high_count + pool->low_count) * (1 << pool->order);
		pool = sys_heap->cached_pools[i];
		total +=
		    (pool->high_count + pool->low_count) * (1 << pool->order);
	}

	return total;
}

static struct ion_heap_ops ion_mm_heap_ops = {
	.allocate = ion_mm_heap_allocate,
	.free = ion_mm_heap_free,
	///.map_dma = ion_mm_heap_map_dma,
	///.unmap_dma = ion_mm_heap_unmap_dma,
	.map_kernel = ion_heap_map_kernel,
	.unmap_kernel = ion_heap_unmap_kernel,
	.map_user = ion_heap_map_user,
	.phys = ion_mm_heap_phys,
	.shrink = ion_mm_heap_shrink,
#if defined(VENDOR_EDIT)
//wangtao@Swdp.shanghai, 2019/02/11, fix null point error when port elsa
	.page_pool_total = ion_mm_heap_pool_total,
#endif
};

struct dump_fd_data {
	struct task_struct *p;
	struct seq_file *s;
};

static int __do_dump_share_fd(const void *data, struct file *file,
			      unsigned int fd)
{
	const struct dump_fd_data *d = data;
	struct seq_file *s = d->s;
	struct task_struct *p = d->p;
	struct ion_buffer *buffer;
	struct ion_mm_buffer_info *bug_info;
	unsigned int block_nr[DOMAIN_NUM] = {0};
	int port[DOMAIN_NUM] = {0};
	unsigned int mva[DOMAIN_NUM] = {0};
	unsigned int mva_fix[DOMAIN_NUM] = {0};
	unsigned int i;
	int pid;
	#define MVA_SIZE_ORDER     20	/* 1M */
	#define MVA_SIZE      BIT(MVA_SIZE_ORDER)
	#define MVA_ALIGN_MASK (MVA_SIZE - 1)

	buffer = ion_drv_file_to_buffer(file);
	if (IS_ERR_OR_NULL(buffer))
		return 0;

	bug_info = (struct ion_mm_buffer_info *)buffer->priv_virt;
	if (bug_info) {
		pid = bug_info->pid;
		for (i = 0; i < DOMAIN_NUM; i++) {
			if (bug_info->MVA[i] ||
			    bug_info->FIXED_MVA[i]) {
				port[i] = bug_info->port[i];
				mva[i] =  bug_info->MVA[i];
				mva_fix[i] =  bug_info->FIXED_MVA[i];
				block_nr[i] = (buffer->size +
					MVA_ALIGN_MASK) >> MVA_SIZE_ORDER;
			}
		}
	} else {
		pid = -1;
		for (i = 0; i < DOMAIN_NUM; i++) {
			mva[i] = 0;
			mva_fix[i] = 0;
			block_nr[i] = 0;
		}
	}
	if (!buffer->handle_count) {
#if (DOMAIN_NUM == 1)
		ION_DUMP(s,
			 "0x%p %9d %16s %5d %5d %16s %4d %8x(%8x) %8d\n",
			 buffer, pid,
			 buffer->alloc_dbg,
			 p->pid, p->tgid,
			 p->comm, fd,
			 mva[0],
			 mva_fix[0],
			 block_nr[0]);
#elif (DOMAIN_NUM == 2)
		ION_DUMP(s,
			 "0x%p %9d %16s %5d %5d %16s %4d %8x(%8x) %8d %8x(%8x) %8d\n",
			 buffer, pid,
			 buffer->alloc_dbg,
			 p->pid, p->tgid,
			 p->comm, fd,
			 mva[0],
			 mva_fix[0],
			 block_nr[0],
			 mva[1],
			 mva_fix[1],
			 block_nr[1]);
#elif (DOMAIN_NUM == 4)
		ION_DUMP(s,
			 "0x%p %9d %16s %5d %5d %16s %4d %8x %8d %8x %8d %8x %8d %8x %8d\n",
			 buffer, pid,
			 buffer->alloc_dbg,
			 p->pid, p->tgid,
			 p->comm, fd,
			 mva[0],
			 block_nr[0],
			 mva[1],
			 block_nr[1],
			 mva[2],
			 block_nr[2],
			 mva[3],
			 block_nr[3]);
#endif
	}
	return 0;
}

static int ion_dump_all_share_fds(struct seq_file *s)
{
	struct task_struct *p;
	int res;
	struct dump_fd_data data;

	/* function is not available, just return */
	if (ion_drv_file_to_buffer(NULL) == ERR_PTR(-EPERM))
		return 0;

	ION_DUMP(s,
		 "%18s %9s %16s %5s %5s %16s %4s %8s %8s %8s %9s\n",
		 "buffer", "alloc_pid", "alloc_client", "pid",
		 "tgid", "process", "fd",
		 "mva1", "nr1", "mva2", "nr2");
	data.s = s;

	read_lock(&tasklist_lock);
	for_each_process(p) {
		task_lock(p);
		data.p = p;
		res = iterate_fd(p->files, 0, __do_dump_share_fd, &data);
		if (res)
			IONMSG("%s failed somehow\n", __func__);
		task_unlock(p);
	}
	read_unlock(&tasklist_lock);
	return 0;
}

static int ion_mm_heap_debug_show(struct ion_heap *heap, struct seq_file *s,
				  void *unused)
{
	struct ion_system_heap
	*sys_heap = container_of(heap, struct ion_system_heap, heap);
	struct ion_device *dev = heap->dev;
	struct rb_node *n;
	int i;
	bool has_orphaned = false;
	struct ion_mm_buffer_info *bug_info;
	unsigned long long current_ts;

	current_ts = sched_clock();
	do_div(current_ts, 1000000);
	ION_DUMP(s, "time 3 %lld ms\n", current_ts);

	for (i = 0; i < num_orders; i++) {
		struct ion_page_pool *pool = sys_heap->pools[i];

		ION_DUMP(s,
			 "%d order %u highmem pages in pool = %lu total, dev, 0x%p, heap id: %d\n",
			 pool->high_count, pool->order,
			 (1 << pool->order) * PAGE_SIZE *
			 pool->high_count, dev, heap->id);
		ION_DUMP(s,
			 "%d order %u lowmem pages in pool = %lu total\n",
			 pool->low_count, pool->order,
			 (1 << pool->order) * PAGE_SIZE *
			 pool->low_count);
		pool = sys_heap->cached_pools[i];
		ION_DUMP(s,
			 "%d order %u highmem pages in cached_pool = %lu total\n",
			 pool->high_count, pool->order,
			 (1 << pool->order) * PAGE_SIZE *
				     pool->high_count);
		ION_DUMP(s,
			 "%d order %u lowmem pages in cached_pool = %lu total\n",
			 pool->low_count, pool->order,
			 (1 << pool->order) * PAGE_SIZE *
			 pool->low_count);
	}
	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		ION_DUMP(s, "mm_heap_freelist total_size=%zu\n",
			 ion_heap_freelist_size(heap));
	else
		ION_DUMP(s, "mm_heap defer free disabled\n");

	ION_DUMP(s,
		 "----------------------------------------------------\n");
#if (DOMAIN_NUM == 1)
	ION_DUMP(s,
		 "%18.s %8.s %4.s %3.s %3.s %3.s %3.s %s %3.s %4.s %s %s %4.s %4.s %4.s %4.s %s\n",
		 "buffer", "size", "kmap", "ref", "hdl", "mod",
		 "mva_cnt", "mva(dom0)", "sec", "flag",
		 "pid(alloc_pid)",
		 "comm(client)", "v1", "v2", "v3", "v4",
		 "dbg_name");
#elif (DOMAIN_NUM == 2)
	ION_DUMP(s,
		 "%18.s %8.s %4.s %3.s %3.s %3.s %3.s %s %s %3.s %4.s %s %s %4.s %4.s %4.s %4.s %s\n",
		 "buffer", "size", "kmap", "ref", "hdl", "mod",
		 "mva_cnt", "mva(dom0)", "mva(dom1)", "sec", "flag",
		 "pid(alloc_pid)",
		 "comm(client)", "v1", "v2", "v3", "v4",
		 "dbg_name");
#elif (DOMAIN_NUM == 4)
	ION_DUMP(s,
		 "%18.s %8.s %4.s %3.s %3.s %3.s %3.s %s %s %s %s %3.s %4.s %s %s %4.s %4.s %4.s %4.s %s\n",
		 "buffer", "size", "kmap", "ref", "hdl", "mod",
		 "mva_cnt", "mva(b0)", "mva(b1)", "mva(b2)", "mva(b3)",
		 "sec", "flag", "pid(alloc_pid)",
		 "comm(client)", "v1", "v2", "v3", "v4",
		 "dbg_name");
#endif

	mutex_lock(&dev->buffer_lock);

	current_ts = sched_clock();
	do_div(current_ts, 1000000);
	ION_DUMP(s, "time 4 %lld ms\n", current_ts);

	for (n = rb_first(&dev->buffers); n; n = rb_next(n)) {
		struct ion_buffer
		*buffer = rb_entry(n, struct ion_buffer, node);
		if (buffer->heap->type != heap->type)
			continue;
		bug_info = (struct ion_mm_buffer_info *)buffer->priv_virt;
		if (heap->id == ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA &&
		    buffer->heap->id != ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA)
			continue;
		if (heap->id == ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA &&
		    buffer->heap->id != ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA)
			continue;

		ion_buffer_dump(buffer, s);

		if (!buffer->handle_count)
			has_orphaned = true;
	}

	current_ts = sched_clock();
	do_div(current_ts, 1000000);
	ION_DUMP(s, "time 5 %lld ms\n", current_ts);

	if (has_orphaned) {
		ION_DUMP(s,
			 "-----orphaned buffer list:------------------\n");
		ion_dump_all_share_fds(s);
	}

	current_ts = sched_clock();
	do_div(current_ts, 1000000);
	ION_DUMP(s, "time 6 %lld ms\n", current_ts);

	mutex_unlock(&dev->buffer_lock);

	ION_DUMP(s,
		 "----------------------------------------------------\n");

	/* dump all handle's backtrace */
	down_read(&dev->lock);
	for (n = rb_first(&dev->clients); n; n = rb_next(n)) {
		struct ion_client
		*client = rb_entry(n, struct ion_client, node);
		struct rb_node *m;

		if (client->task) {
			char task_comm[TASK_COMM_LEN];

			get_task_comm(task_comm, client->task);
			ION_DUMP(s,
				 "client(0x%p) %s (%s) pid(%u) ================>\n",
				 client, task_comm,
				 (*client->dbg_name) ?
				 client->dbg_name :
				 client->name,
				 client->pid);
		} else {
			ION_DUMP(s,
				 "client(0x%p) %s (from_kernel) pid(%u) ================>\n",
				 client, client->name, client->pid);
		}

		mutex_lock(&client->lock);
		for (m = rb_first(&client->handles); m; m = rb_next(m)) {
			struct ion_handle
			*handle = rb_entry(m, struct ion_handle, node);
			if (heap->id == ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA &&
			    handle->buffer->heap->id !=
				ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA)
				continue;
			if (heap->id == ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA &&
			    handle->buffer->heap->id !=
				ION_HEAP_TYPE_MULTIMEDIA_MAP_MVA)
				continue;

			ION_DUMP(s,
				 "\thandle=0x%p, buffer=0x%p, heap=%u, fd=%4d, ts: %lldms\n",
				 handle, handle->buffer,
				 handle->buffer->heap->id,
				 handle->dbg.fd,
				 handle->dbg.user_ts);
		}
		mutex_unlock(&client->lock);
	}
	current_ts = sched_clock();
	do_div(current_ts, 1000000);
	ION_DUMP(s, "current time %llu ms, total: %llu!!\n",
		 current_ts,
		 (unsigned long long)(atomic64_read(&page_sz_cnt) * 4096));
#ifdef CONFIG_MTK_IOMMU_V2
	mtk_iommu_log_dump(s);
#endif
	up_read(&dev->lock);

	return 0;
}

int ion_mm_heap_for_each_pool(int (*fn)(int high, int order,
					int cache, size_t size))
{
	struct ion_heap *heap =
		ion_drv_get_heap(g_ion_device, ION_HEAP_TYPE_MULTIMEDIA, 1);
	struct ion_system_heap
	*sys_heap = container_of(heap, struct ion_system_heap, heap);
	int i;

	if (!heap)
		return -1;

	for (i = 0; i < num_orders; i++) {
		struct ion_page_pool *pool = sys_heap->pools[i];

		fn(1, pool->order, 0,
		   (1 << pool->order) * PAGE_SIZE * pool->high_count);
		fn(0, pool->order, 0,
		   (1 << pool->order) * PAGE_SIZE * pool->low_count);

		pool = sys_heap->cached_pools[i];
		fn(1, pool->order, 1,
		   (1 << pool->order) * PAGE_SIZE * pool->high_count);
		fn(0, pool->order, 1,
		   (1 << pool->order) * PAGE_SIZE * pool->low_count);
	}
	return 0;
}

static size_t ion_debug_mm_heap_total(struct ion_client *client,
				      unsigned int id)
{
	size_t size = 0;
	struct rb_node *n;

	if (mutex_trylock(&client->lock)) {
		/* mutex_lock(&client->lock); */
		for (n = rb_first(&client->handles); n; n = rb_next(n)) {
			struct ion_handle
			*handle = rb_entry(n, struct ion_handle, node);
			if (handle->buffer->heap->id == id)
				size += handle->buffer->size;
		}
		mutex_unlock(&client->lock);
	}
	return size;
}

void ion_mm_heap_memory_detail(void)
{
	struct ion_device *dev = g_ion_device;
	/* struct ion_heap *heap = NULL; */
	size_t mm_size = 0;
	size_t cam_size = 0;
	size_t total_orphaned_size = 0;
	struct rb_node *n;
	bool need_dev_lock = true;
	bool has_orphaned = false;
	struct ion_mm_buffer_info *bug_info;
	struct ion_mm_buf_debug_info *pdbg;
	char seq_log[448];
	char seq_fmt[] = "|0x%p %10zu %5d(%5d) %16s %2d %5u-%-6u %48s |";
	int seq_log_count = 0;
	unsigned int heapid;
	struct ion_heap *mm_heap = NULL;
	struct ion_heap *camera_heap =
		ion_drv_get_heap(g_ion_device,
				 ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA, 0);
	int i;

	ION_DUMP(NULL, "%16s(%16s) %6s %12s %s\n",
		 "client", "dbg_name", "pid", "size", "address");
	ION_DUMP(NULL, "--------------------------------------\n");

	if (!down_read_trylock(&dev->lock)) {
		ION_DUMP(NULL,
			 "detail trylock fail, alloc pid(%d-%d)\n",
				     caller_pid, caller_tid);
		ION_DUMP(NULL, "current(%d-%d)\n",
			 (unsigned int)current->pid,
			 (unsigned int)current->tgid);
		if (caller_pid != (unsigned int)current->pid ||
		    caller_tid != (unsigned int)current->tgid)
			goto skip_client_entry;
		else
			need_dev_lock = false;
	}

	memset(seq_log, 0, 448);
	for (n = rb_first(&dev->clients); n; n = rb_next(n)) {
		struct ion_client
		*client = rb_entry(n, struct ion_client, node);
		size_t size =
		    ion_debug_mm_heap_total(client, ION_HEAP_TYPE_MULTIMEDIA);

		heapid = ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA;
		if (!size) {
			size = ion_debug_mm_heap_total(client, heapid);
			if (!size)
				continue;
		}

		seq_log_count++;
		if (client->task) {
			char task_comm[TASK_COMM_LEN];

			get_task_comm(task_comm, client->task);
			sprintf(seq_log + strlen(seq_log),
				"|%16s(%16s) %6u %12zu 0x%p |",
				task_comm, client->dbg_name,
				client->pid, size, client);
		} else {
			sprintf(seq_log + strlen(seq_log),
				"|%16s(%16s) %6u %12zu 0x%p |",
				client->name, "from_kernel",
				client->pid, size, client);
		}

		if ((seq_log_count % 3) == 0) {
			ION_DUMP(NULL, "%s\n", seq_log);
			memset(seq_log, 0, 448);
		}
	}

	ION_DUMP(NULL, "%s\n", seq_log);

	if (need_dev_lock)
		up_read(&dev->lock);

	ION_DUMP(NULL, "---------ion_mm_heap buffer info------\n");

skip_client_entry:

	ION_DUMP(NULL,
		 "%s %8s %s %16s %6s %10s %32s\n",
		 "buffer	", "size",
		 "pid(alloc_pid)", "comm(client)",
		 "heapid", "v1-v2", "dbg_name");

	if (mutex_trylock(&dev->buffer_lock)) {
		seq_log_count = 0;

		memset(seq_log, 0, 448);
		for (n = rb_first(&dev->buffers); n; n = rb_next(n)) {
			struct ion_buffer
			*buffer = rb_entry(n, struct ion_buffer, node);
			int cam_heap;

			heapid = buffer->heap->id;
			cam_heap = ((1 << heapid) & ION_HEAP_CAMERA_MASK);
			bug_info =
				(struct ion_mm_buffer_info *)buffer->priv_virt;
			pdbg = &bug_info->dbg_info;

			if (((1 << heapid) & ION_HEAP_MULTIMEDIA_MASK) ||
			    ((1 << heapid) & ION_HEAP_CAMERA_MASK)) {
				if ((1 << heapid) & ION_HEAP_MULTIMEDIA_MASK) {
					mm_size += buffer->size;
					mm_heap = buffer->heap;
				}
				if ((1 << heapid) & ION_HEAP_CAMERA_MASK)
					cam_size += buffer->size;

				if (!buffer->handle_count) {
					total_orphaned_size += buffer->size;
					has_orphaned = true;
				}

				if ((strstr(pdbg->dbg_name, "nothing") &&
				     (cam_heap == 0)) && need_dev_lock)
					continue;

				seq_log_count++;
				sprintf(seq_log + strlen(seq_log), seq_fmt,
					buffer, buffer->size,
					buffer->pid, bug_info->pid,
					buffer->task_comm, buffer->heap->id,
					pdbg->value1, pdbg->value2,
					pdbg->dbg_name);

				if ((seq_log_count % 3) == 0) {
					ION_DUMP(NULL, "%s\n",
						 seq_log);
					memset(seq_log, 0, 448);
				}
			}
		}

		ION_DUMP(NULL, "%s\n", seq_log);

		if (has_orphaned) {
			ION_DUMP(NULL, "-orphaned buffer list:\n");
			ion_dump_all_share_fds(NULL);
		}

		mutex_unlock(&dev->buffer_lock);

		if (mm_heap) {
			if (mm_heap->flags & ION_HEAP_FLAG_DEFER_FREE)
				ION_DUMP(NULL, "%16.s %u %16zu\n",
					 "deferred free heap_id",
				mm_heap->id,
				mm_heap->free_list_size);

			for (i = 0; i < num_orders; i++) {
				struct ion_system_heap *sys_heap =
					container_of(mm_heap,
						     struct ion_system_heap,
						     heap);
				struct ion_page_pool *pool = sys_heap->pools[i];

				ION_DUMP(NULL,
					 "%d order %u highmem pages in pool = %lu total, dev, 0x%p, heap id: %d\n",
				pool->high_count, pool->order,
				(1 << pool->order) * PAGE_SIZE *
				pool->high_count, dev, mm_heap->id);
				ION_DUMP(NULL,
					 "%d order %u lowmem pages in pool = %lu total\n",
				pool->low_count, pool->order,
				(1 << pool->order) * PAGE_SIZE *
				pool->low_count);
				pool = sys_heap->cached_pools[i];
				ION_DUMP(NULL,
					 "%d order %u highmem pages in cached_pool = %lu total\n",
				pool->high_count, pool->order,
				(1 << pool->order) * PAGE_SIZE *
				pool->high_count);
				ION_DUMP(NULL,
					 "%d order %u lowmem pages in cached_pool = %lu total\n",
				pool->low_count, pool->order,
				(1 << pool->order) * PAGE_SIZE *
				pool->low_count);
			}
		}
		if (camera_heap) {
			if (camera_heap->flags & ION_HEAP_FLAG_DEFER_FREE)
				ION_DUMP(NULL, "%16.s %u %16zu\n",
					 "cam heap deferred free heap_id",
					 camera_heap->id,
					 camera_heap->free_list_size);

			for (i = 0; i < num_orders; i++) {
				struct ion_system_heap *sys_heap =
					container_of(camera_heap,
						     struct ion_system_heap,
						     heap);
				struct ion_page_pool *pool = sys_heap->pools[i];

				ION_DUMP(NULL,
					 "%d order %u highmem pages in pool = %lu total, dev, 0x%p, heap id: %d\n",
				pool->high_count, pool->order,
				(1 << pool->order) * PAGE_SIZE *
				pool->high_count, dev, camera_heap->id);
				ION_DUMP(NULL,
					 "%d order %u lowmem pages in pool = %lu total\n",
				pool->low_count, pool->order,
				(1 << pool->order) * PAGE_SIZE *
				pool->low_count);
				pool = sys_heap->cached_pools[i];
				ION_DUMP(NULL,
					 "%d order %u highmem pages in cached_pool = %lu total\n",
				pool->high_count, pool->order,
				(1 << pool->order) * PAGE_SIZE *
				pool->high_count);
				ION_DUMP(NULL,
					 "%d order %u lowmem pages in cached_pool = %lu total\n",
				pool->low_count, pool->order,
				(1 << pool->order) * PAGE_SIZE *
							 pool->low_count);
			}
		}

		ION_DUMP(NULL, "------------------------------\n");
		ION_DUMP(NULL, "total orphaned: %16zu\n",
			 total_orphaned_size);
		ION_DUMP(NULL, "mm total: %16zu, cam: %16zu\n",
			 mm_size, cam_size);
		ION_DUMP(NULL, "ion heap total memory: %llu\n",
			 (unsigned long long)(
			 atomic64_read(&page_sz_cnt) * 4096));
		ION_DUMP(NULL, "------------------------------\n");
	} else {
		ION_DUMP(NULL, "ion heap total memory: %llu\n",
			 (unsigned long long)(
			 atomic64_read(&page_sz_cnt) * 4096));
	}
}

size_t ion_mm_heap_total_memory(void)
{
	return (size_t)(atomic64_read(&page_sz_cnt) * 4096);
}

#ifdef VENDOR_EDIT
//fangpan@Swdp.shanghai, 2016/02/02, add ion memory status interface
size_t ion_mm_heap_pool_total_size(void)
{
	struct ion_heap *heap = ion_drv_get_heap(g_ion_device, ION_HEAP_TYPE_MULTIMEDIA, 1);
        if (NULL != heap) {
                return heap->ops->page_pool_total(heap);
        } else {
                return 0;
        }
}
EXPORT_SYMBOL(ion_mm_heap_pool_total_size);
#endif

#ifdef VENDOR_EDIT
/* Wen.Luo@BSP.Kernel.Stability, 2019/04/26, Add for Process memory statistics */
size_t get_ion_heap_by_pid(pid_t pid)
{
	struct ion_device *dev = g_ion_device;
	struct rb_node *n, *m;
	int buffer_size = 0;
	unsigned int id = 0;
	enum mtk_ion_heap_type cam_heap = ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA;
	enum mtk_ion_heap_type mm_heap = ION_HEAP_TYPE_MULTIMEDIA;

	if (!down_read_trylock(&dev->lock))
		return 0;
	for (n = rb_first(&dev->clients); n; n = rb_next(n)) {
		struct ion_client *client = rb_entry(n, struct ion_client, node);
		if(client->pid == pid) {
			mutex_lock(&client->lock);
			for (m = rb_first(&client->handles); m;
			     m = rb_next(m)) {
				struct ion_handle *handle =
				    rb_entry(m, struct ion_handle,
					     node);
				id = handle->buffer->heap->id;

				if ((id == mm_heap || id == cam_heap) &&
				    (handle->buffer->handle_count) != 0) {
					buffer_size +=
					    (int)(handle->buffer->size) /
					    (handle->buffer->handle_count);
				}
			}
			mutex_unlock(&client->lock);
		}
	}
	up_read(&dev->lock);
	return buffer_size/1024;
}
EXPORT_SYMBOL(get_ion_heap_by_pid);
#endif

struct ion_heap *ion_mm_heap_create(struct ion_platform_heap *unused)
{
	struct ion_system_heap *heap;
	int i;

	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap) {
		IONMSG("%s kzalloc failed heap is null.\n", __func__);
		return ERR_PTR(-ENOMEM);
	}
	heap->heap.ops = &ion_mm_heap_ops;
	heap->heap.type = (unsigned int)ION_HEAP_TYPE_MULTIMEDIA;
	heap->heap.flags = ION_HEAP_FLAG_DEFER_FREE;
	heap->pools =
	    kcalloc(num_orders, sizeof(struct ion_page_pool *), GFP_KERNEL);
	if (!heap->pools)
		goto err_alloc_pools;
	heap->cached_pools =
	    kcalloc(num_orders, sizeof(struct ion_page_pool *), GFP_KERNEL);
	if (!heap->cached_pools) {
		kfree(heap->pools);
		goto err_alloc_pools;
	}

	for (i = 0; i < num_orders; i++) {
		struct ion_page_pool *pool;
		gfp_t gfp_flags = order_gfp_flags[i];

		if (unused->id == ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA)
			gfp_flags |= __GFP_HIGHMEM;

		pool = ion_page_pool_create(gfp_flags, orders[i], false);
		if (!pool)
			goto err_create_pool;
		heap->pools[i] = pool;

		pool = ion_page_pool_create(gfp_flags, orders[i], true);
		if (!pool)
			goto err_create_pool;
		heap->cached_pools[i] = pool;
	}

	heap->heap.debug_show = ion_mm_heap_debug_show;
	ion_comm_init();
	return &heap->heap;

err_create_pool:
	IONMSG("[ion_mm_heap]: error to create pool\n");
	for (i = 0; i < num_orders; i++) {
		if (heap->pools[i])
			ion_page_pool_destroy(heap->pools[i]);
		if (heap->cached_pools[i])
			ion_page_pool_destroy(heap->cached_pools[i]);
	}
	kfree(heap->pools);
	kfree(heap->cached_pools);

err_alloc_pools:
	IONMSG("[ion_mm_heap]: error to allocate pool\n");
	kfree(heap);
	return ERR_PTR(-ENOMEM);
}

void ion_mm_heap_destroy(struct ion_heap *heap)
{
	struct ion_system_heap
	*sys_heap = container_of(heap, struct ion_system_heap, heap);
	int i;

	for (i = 0; i < num_orders; i++)
		ion_page_pool_destroy(sys_heap->pools[i]);
	kfree(sys_heap->pools);
	kfree(sys_heap);
}

int ion_mm_cp_dbg_info(struct ion_mm_buf_debug_info *src,
		       struct ion_mm_buf_debug_info *dst)
{
	int i;

	dst->handle = src->handle;
	for (i = 0; i < ION_MM_DBG_NAME_LEN; i++)
		dst->dbg_name[i] = src->dbg_name[i];

	dst->dbg_name[ION_MM_DBG_NAME_LEN - 1] = '\0';
	dst->value1 = src->value1;
	dst->value2 = src->value2;
	dst->value3 = src->value3;
	dst->value4 = src->value4;

	return 0;
}

int ion_mm_cp_sf_buf_info(struct ion_mm_sf_buf_info *src,
			  struct ion_mm_sf_buf_info *dst)
{
	int i;

	dst->handle = src->handle;
	for (i = 0; i < ION_MM_SF_BUF_INFO_LEN; i++)
		dst->info[i] = src->info[i];

	return 0;
}

long ion_mm_ioctl(struct ion_client *client, unsigned int cmd,
		  unsigned long arg, int from_kernel)
{
	struct ion_mm_data param;
	long ret = 0;
	/* char dbgstr[256]; */
	unsigned long ret_copy;
	unsigned int buffer_sec = 0;
	enum ion_heap_type buffer_type = 0;
	struct ion_buffer *buffer;
	struct ion_handle *kernel_handle;
	int domain_idx = 0;

	if (from_kernel)
		param = *(struct ion_mm_data *)arg;
	else
		ret_copy = copy_from_user(&param, (void __user *)arg,
					  sizeof(struct ion_mm_data));

	switch (param.mm_cmd) {
	case ION_MM_CONFIG_BUFFER:
	case ION_MM_CONFIG_BUFFER_EXT:
		if ((from_kernel && param.config_buffer_param.kernel_handle) ||
		    (from_kernel == 0 && param.config_buffer_param.handle)) {
			;
		} else {
			IONMSG(": Error config buf with invalid handle.\n");
			ret = -EFAULT;
			break;
		}

		kernel_handle = ion_drv_get_handle(
					client,
					param.config_buffer_param.handle,
					param.config_buffer_param.kernel_handle,
					from_kernel);
		if (IS_ERR(kernel_handle)) {
			IONMSG("ion config buffer fail! port=%d.\n",
			       param.config_buffer_param.module_id);
			ret = -EINVAL;
			break;
		}

		buffer = ion_handle_buffer(kernel_handle);
		buffer_type = buffer->heap->type;
		domain_idx =
			ion_get_domain_id(from_kernel,
					  &param.config_buffer_param.module_id);
		if (domain_idx < 0 ||
		    domain_idx >= DOMAIN_NUM) {
			IONMSG("config err:%d(%d)-%d,%16.s\n",
			       param.config_buffer_param.module_id,
			       domain_idx,
			       buffer->heap->type, client->name);
			ret = -EINVAL;
			break;
		}

		if ((int)buffer->heap->type == ION_HEAP_TYPE_MULTIMEDIA) {
			struct ion_mm_buffer_info *buffer_info =
			    buffer->priv_virt;
			enum ION_MM_CMDS mm_cmd = param.mm_cmd;

			buffer_sec = buffer_info->security;

#if defined(CONFIG_MTK_IOMMU_PGTABLE_EXT) && \
	(CONFIG_MTK_IOMMU_PGTABLE_EXT > 32)
			if (mm_cmd == ION_MM_CONFIG_BUFFER &&
			    buffer_info->module_id != -1) {
				IONMSG
				    ("corrupt with %d, %d-%d,name %16.s!!!\n",
				     buffer_info->module_id,
				     param.config_buffer_param.module_id,
				     buffer->heap->type, client->name);
				return -EFAULT;
			}

			if (mm_cmd == ION_MM_CONFIG_BUFFER_EXT &&
			    buffer_info->fix_module_id != -1) {
				IONMSG
				    ("corrupt with %d, %d-%d,name %16.s!!!\n",
				     buffer_info->fix_module_id,
				     param.config_buffer_param.module_id,
				     buffer->heap->type, client->name);
				return -EFAULT;
			}
#endif
			if (param.config_buffer_param.module_id < 0) {
				IONMSG
				    ("config error:%d-%d,name %16.s!!!\n",
				     param.config_buffer_param.module_id,
				     buffer->heap->type, client->name);
				return -EFAULT;
			}

#ifndef CONFIG_MTK_IOMMU_V2
			if ((buffer_info->MVA[domain_idx] == 0 &&
			     mm_cmd == ION_MM_CONFIG_BUFFER) ||
			    (buffer_info->FIXED_MVA[domain_idx] == 0 &&
				mm_cmd == ION_MM_CONFIG_BUFFER_EXT)) {
#endif
				buffer_info->security =
				    param.config_buffer_param.security;
				buffer_info->coherent =
				    param.config_buffer_param.coherent;
				if (mm_cmd == ION_MM_CONFIG_BUFFER_EXT) {
					buffer_info->iova_start[domain_idx] =
				param.config_buffer_param.reserve_iova_start;
					buffer_info->iova_end[domain_idx] =
				param.config_buffer_param.reserve_iova_end;
					buffer_info->fix_module_id =
				param.config_buffer_param.module_id;
				} else {
					buffer_info->module_id =
					    param.config_buffer_param.module_id;
				}
#ifndef CONFIG_MTK_IOMMU_V2
			}
#endif
		} else if ((int)buffer->heap->type == ION_HEAP_TYPE_FB) {
			struct ion_fb_buffer_info *buffer_info =
			    buffer->priv_virt;

			buffer_sec = buffer_info->security;
#ifndef CONFIG_MTK_IOMMU_V2
			if (buffer_info->MVA == 0) {
#endif
				buffer_info->module_id =
				    param.config_buffer_param.module_id;
				buffer_info->security =
				    param.config_buffer_param.security;
				buffer_info->coherent =
				    param.config_buffer_param.coherent;
				if (param.mm_cmd == ION_MM_CONFIG_BUFFER_EXT) {
					buffer_info->iova_start =
				param.config_buffer_param.reserve_iova_start;
					buffer_info->iova_end =
				param.config_buffer_param.reserve_iova_end;
				}
#ifndef CONFIG_MTK_IOMMU_V2
			}
#endif
		} else if ((int)buffer->heap->type ==
			   ION_HEAP_TYPE_MULTIMEDIA_SEC) {
			struct ion_sec_buffer_info *buffer_info =
			    buffer->priv_virt;

			buffer_sec = buffer_info->security;
#ifndef CONFIG_MTK_IOMMU_V2
			if (buffer_info->MVA == 0) {
#endif
				buffer_info->module_id =
				    param.config_buffer_param.module_id;
				buffer_info->security =
				    param.config_buffer_param.security;
				buffer_info->coherent =
				    param.config_buffer_param.coherent;
				if (param.mm_cmd == ION_MM_CONFIG_BUFFER_EXT) {
					buffer_info->iova_start =
				param.config_buffer_param.reserve_iova_start;
					buffer_info->iova_end =
				param.config_buffer_param.reserve_iova_end;
				}
#ifndef CONFIG_MTK_IOMMU_V2
			}
#endif
		} else {
			IONMSG
			    (": Error. config buffer is not from %c heap.\n",
			     buffer->heap->type);
			ret = 0;
		}
		ion_drv_put_kernel_handle(kernel_handle);

		break;
	case ION_MM_GET_IOVA:
	case ION_MM_GET_IOVA_EXT:
		if ((from_kernel && param.get_phys_param.kernel_handle) ||
		    (from_kernel == 0 && param.get_phys_param.handle)) {
			;
		} else {
			IONMSG(": Error get iova buf with invalid handle.\n");
			ret = -EFAULT;
			break;
		}

		kernel_handle =
		    ion_drv_get_handle(client,
				       param.get_phys_param.handle,
				       param.get_phys_param.kernel_handle,
				       from_kernel);
		if (IS_ERR(kernel_handle)) {
			IONMSG("ion get iova fail! port=%d.\n",
			       param.get_phys_param.module_id);
			ret = -EINVAL;
			break;
		}

		buffer = ion_handle_buffer(kernel_handle);
		buffer_type = buffer->heap->type;
		domain_idx =
			ion_get_domain_id(from_kernel,
					  &param.config_buffer_param.module_id);
		if (domain_idx < 0 ||
		    domain_idx >= DOMAIN_NUM) {
			IONMSG("get err:%d(%d)-%d,%16.s\n",
			       param.config_buffer_param.module_id,
			       domain_idx,
			       buffer->heap->type, client->name);
			ret = -EINVAL;
			break;
		}

		if ((int)buffer->heap->type == ION_HEAP_TYPE_MULTIMEDIA) {
			struct ion_mm_buffer_info *buffer_info =
			    buffer->priv_virt;
			enum ION_MM_CMDS mm_cmd = param.mm_cmd;
			ion_phys_addr_t phy_addr;

			mutex_lock(&buffer_info->lock);

			if (param.get_phys_param.module_id < 0) {
				IONMSG(
					"get iova error:%d-%d,name %16.s!!!\n",
				     param.get_phys_param.module_id,
				     buffer->heap->type, client->name);
				mutex_unlock(&buffer_info->lock);
				ion_drv_put_kernel_handle(kernel_handle);
				return -EFAULT;
			}

			if ((buffer_info->MVA[domain_idx] == 0 &&
			     mm_cmd == ION_MM_GET_IOVA) ||
			    (buffer_info->FIXED_MVA[domain_idx] == 0 &&
				mm_cmd == ION_MM_GET_IOVA_EXT)) {
				buffer_info->security =
				    param.get_phys_param.security;
				buffer_info->coherent =
				    param.get_phys_param.coherent;
				if (mm_cmd == ION_MM_GET_IOVA_EXT) {
					buffer_info->iova_start[domain_idx] =
				param.get_phys_param.reserve_iova_start;
					buffer_info->iova_end[domain_idx] =
				param.get_phys_param.reserve_iova_end;
					buffer_info->fix_module_id =
					    param.get_phys_param.module_id;
				} else {
					buffer_info->module_id =
					    param.get_phys_param.module_id;
				}
			}

			/* get mva */
			phy_addr = param.get_phys_param.phy_addr;

			if (ion_phys(client, kernel_handle, &phy_addr,
				     (size_t *)&param.get_phys_param.len) <
			    0) {
				param.get_phys_param.phy_addr = 0;
				param.get_phys_param.len = 0;
				IONMSG(" %s: Error. Cannot get iova.\n",
				       __func__);
				ret = -EFAULT;
			}
			param.get_phys_param.phy_addr = (unsigned int)phy_addr;

			mutex_unlock(&buffer_info->lock);

		} else {
			IONMSG
			    (": Error. get iova is not from %c heap.\n",
			     buffer->heap->type);
			ret = -EFAULT;
		}
		ion_drv_put_kernel_handle(kernel_handle);
		break;
	case ION_MM_SET_DEBUG_INFO:

		if (param.buf_debug_info_param.handle == 0) {
			IONMSG(" Error. set dbg buffer with invalid handle\n");
			ret = -EFAULT;
			break;
		}

		kernel_handle = ion_drv_get_handle(
			client,
			param.buf_debug_info_param.handle,
			param.buf_debug_info_param.kernel_handle,
			from_kernel);

		if (IS_ERR(kernel_handle)) {
			IONMSG(" set debug info fail! kernel_handle=0x%p\n",
			       kernel_handle);
			ret = -EINVAL;
			break;
		}

		buffer = ion_handle_buffer(kernel_handle);
		buffer_type = buffer->heap->type;
		if ((int)buffer->heap->type == ION_HEAP_TYPE_MULTIMEDIA) {
			struct ion_mm_buffer_info *buffer_info =
			    buffer->priv_virt;

			buffer_sec = buffer_info->security;
			ion_mm_cp_dbg_info(&param.buf_debug_info_param,
					   &buffer_info->dbg_info);
		} else if ((int)buffer->heap->type == ION_HEAP_TYPE_FB) {
			struct ion_fb_buffer_info *buffer_info =
			    buffer->priv_virt;

			buffer_sec = buffer_info->security;
			ion_mm_cp_dbg_info(&param.buf_debug_info_param,
					   &buffer_info->dbg_info);
		} else if ((int)buffer->heap->type ==
			   ION_HEAP_TYPE_MULTIMEDIA_SEC) {
			struct ion_sec_buffer_info *buffer_info =
			    buffer->priv_virt;

			buffer_sec = buffer_info->security;
			ion_mm_cp_dbg_info(&param.buf_debug_info_param,
					   &buffer_info->dbg_info);
		} else {
			IONMSG
			    (" set dbg buffer error: not from %c heap.\n",
			     buffer->heap->type);
			ret = -EFAULT;
		}
		ion_drv_put_kernel_handle(kernel_handle);
		break;
	case ION_MM_GET_DEBUG_INFO:

		if (param.buf_debug_info_param.handle == 0) {
			IONMSG("Error. ION_MM_GET_DEBUG_INFO invalid\n");
			ret = -EFAULT;
			break;
		}

		kernel_handle = ion_drv_get_handle(
				client,
				param.buf_debug_info_param.handle,
				param.buf_debug_info_param.kernel_handle,
				from_kernel);
		if (IS_ERR(kernel_handle)) {
			IONMSG("ion get debug info fail! kernel_handle=0x%p\n",
			       kernel_handle);
			ret = -EINVAL;
			break;
		}
		buffer = ion_handle_buffer(kernel_handle);
		buffer_type = buffer->heap->type;
		if ((int)buffer->heap->type == ION_HEAP_TYPE_MULTIMEDIA) {
			struct ion_mm_buffer_info *buffer_info =
			    buffer->priv_virt;

			buffer_sec = buffer_info->security;
			ion_mm_cp_dbg_info(&buffer_info->dbg_info,
					   &param.buf_debug_info_param);
		} else if ((int)buffer->heap->type == ION_HEAP_TYPE_FB) {
			struct ion_fb_buffer_info *buffer_info =
			    buffer->priv_virt;

			buffer_sec = buffer_info->security;
			ion_mm_cp_dbg_info(&buffer_info->dbg_info,
					   &param.buf_debug_info_param);
		} else if ((int)buffer->heap->type ==
			   ION_HEAP_TYPE_MULTIMEDIA_SEC) {
			struct ion_sec_buffer_info *buffer_info =
			    buffer->priv_virt;

			buffer_sec = buffer_info->security;
			ion_mm_cp_dbg_info(&buffer_info->dbg_info,
					   &param.buf_debug_info_param);
		} else {
			IONMSG
			    (" get dbg error: is not from %c heap.\n",
			     buffer->heap->type);
			ret = -EFAULT;
		}
		ion_drv_put_kernel_handle(kernel_handle);

		break;
	case ION_MM_ACQ_CACHE_POOL:
	{
		ion_comm_event_notify(1, param.pool_info_param.len);
		IONMSG("[ion_heap]: ION_MM_ACQ_CACHE_POOL-%d.\n", param.mm_cmd);
	}
	break;
	case ION_MM_QRY_CACHE_POOL:
	{
		int qry_type = ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA;
		struct ion_heap *ion_cam_heap =
					ion_drv_get_heap(g_ion_device,
							 qry_type,
							 1);
		param.pool_info_param.ret =
			ion_mm_heap_pool_size(ion_cam_heap,
					      __GFP_HIGHMEM,
					      true);
		IONMSG("ION_MM_QRY_CACHE_POOL, heap 0x%p, id %d, ret: %d.\n",
		       ion_cam_heap, param.pool_info_param.heap_id_mask,
		       param.pool_info_param.ret);
	}
	break;
	default:
		IONMSG(" Error. Invalid command(%d).\n", param.mm_cmd);
		ret = -EFAULT;
	}

	if (from_kernel)
		*(struct ion_mm_data *)arg = param;
	else
		ret_copy =
		    copy_to_user((void __user *)arg, &param,
				 sizeof(struct ion_mm_data));
	return ret;
}

int ion_mm_heap_cache_allocate(struct ion_heap *heap,
			       struct ion_buffer *buffer,
			       unsigned long size,
			       unsigned long align,
			       unsigned long flags)
{
	struct ion_system_heap
	*sys_heap = container_of(
			heap,
			struct ion_system_heap,
			heap);
	struct sg_table *table = NULL;
	struct scatterlist *sg;
	int ret;
	struct list_head pages;
	struct page_info *info = NULL;
	struct page_info *tmp_info = NULL;
	int i = 0;
	unsigned long size_remaining = PAGE_ALIGN(size);
	unsigned int max_order = orders[0];
	unsigned long long start, end;

	INIT_LIST_HEAD(&pages);
	start = sched_clock();

	/* add time interval to alloc 64k page in low memory status*/
	if ((start - alloc_large_fail_ts) < 500000000)
		max_order = orders[1];

	while (size_remaining > 0) {
		info = alloc_largest_available(sys_heap, buffer,
					       size_remaining,
					       max_order);
		if (!info) {
			IONMSG("%s cache_alloc info failed.\n", __func__);
			break;
		}
		list_add_tail(&info->list, &pages);
		size_remaining -= (1 << info->order) * PAGE_SIZE;
		max_order = info->order;
		i++;
	}
	end = sched_clock();

	if (!info) {
		IONMSG("%s err info, size %ld, remain %ld.\n",
			__func__, size, size_remaining);
		return -ENOMEM;
	}

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		IONMSG("%s cache kzalloc failed table is null.\n", __func__);
		goto err;
	}

	ret = sg_alloc_table(table, i, GFP_KERNEL);
	if (ret) {
		IONMSG("%s sg cache alloc table failed %d.\n", __func__, ret);
		goto err1;
	}

	sg = table->sgl;
	list_for_each_entry_safe(info, tmp_info, &pages, list) {
		struct page *page = info->page;

		sg_set_page(sg, page, (1 << info->order) * PAGE_SIZE, 0);
		sg = sg_next(sg);
		list_del(&info->list);
		kfree(info);
	}

	buffer->sg_table = table;
	if (size != size_remaining)
		IONMSG("%s cache_alloc alloc, size %ld, remain %ld.\n",
		       __func__, size, size_remaining);
	return 0;
err1:
	kfree(table);
	IONMSG("error: cache_alloc for sg_table fail\n");
err:
	list_for_each_entry_safe(info, tmp_info, &pages, list) {
		free_buffer_page(sys_heap, buffer,
				 info->page, info->order);
		kfree(info);
	}
	IONMSG("mm_cache_alloc fail: size=%lu, flag=%lu.\n", size, flags);

	return -ENOMEM;
}

void ion_mm_heap_cache_free(struct ion_buffer *buffer)
{
	struct ion_heap *heap = buffer->heap;
	struct ion_system_heap *sys_heap;
	struct sg_table *table = buffer->sg_table;
	struct scatterlist *sg;
	LIST_HEAD(pages);
	int i;

	sys_heap = container_of(heap, struct ion_system_heap, heap);
	if (!(buffer->private_flags & ION_PRIV_FLAG_SHRINKER_FREE))
		ion_heap_buffer_zero(buffer);

	for_each_sg(table->sgl, sg, table->nents, i)
		free_buffer_page(sys_heap, buffer,
				 sg_page(sg), get_order(sg->length));

	sg_free_table(table);
	kfree(table);
}

int ion_mm_heap_pool_size(struct ion_heap *heap, gfp_t gfp_mask, bool cache)
{
	struct ion_system_heap *sys_heap;
	int nr_total = 0;
	int i;

	sys_heap = container_of(heap, struct ion_system_heap, heap);

	for (i = 0; i < num_orders; i++) {
		struct ion_page_pool *pool = sys_heap->pools[i];

		if (cache)
			pool = sys_heap->cached_pools[i];
		nr_total += (ion_page_pool_shrink(pool, gfp_mask, 0) *
						  PAGE_SIZE);
	}

	return nr_total;
}

#ifdef CONFIG_PM
void shrink_ion_by_scenario(int need_lock)
{
	int nr_to_reclaim, nr_reclaimed;
	int nr_to_try = 3;

	struct ion_heap *cam_heap =
	    ion_drv_get_heap(g_ion_device, ION_HEAP_TYPE_MULTIMEDIA_FOR_CAMERA,
			     need_lock);

	if (!cam_heap)
		return;
	do {
		nr_to_reclaim =
		    ion_mm_heap_shrink(cam_heap,
				       __GFP_HIGHMEM, 0);
		nr_reclaimed =
		    ion_mm_heap_shrink(cam_heap,
				       __GFP_HIGHMEM,
				       nr_to_reclaim);

		if (nr_to_reclaim == nr_reclaimed)
			break;
	} while (--nr_to_try != 0);

	if (nr_to_reclaim != nr_reclaimed)
		IONMSG("%s: remaining (%d)\n", __func__,
		       nr_to_reclaim - nr_reclaimed);
}
#endif
