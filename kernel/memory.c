#include "memory.h"
#include "bitmap.h"
#include "stdint.h"
#include "global.h"
#include "print.h"
#include "string.h"
#include "debug.h"
#include "sync.h"
#include "thread.h"
#include "interrupt.h"

/******* 位图地址 ******
 * 因为0xc009f000是内核主线程栈顶，0xc009e000是内核主线程的pcd。
 * 一个页框大小的位图可表示128MB内存，位图位置安排在地址0xc009a000，
 * 这样本系统最大支持4个页框的位图，即512MB
*/
#define MEM_BITMAP_BASE 0xc009a000

/*0xc0000000是内核从虚拟地址3G起。0x100000意指跨过低端1MB内存， 使虚拟地址在逻辑上连续*/
#define K_HEAP_START 0xc0100000
#define PDE_IDX(addr) ((addr & 0xffc00000) >> 22)
#define PTE_IDX(addr) ((addr & 0x003ff000) >> 12)

/*内存池结构，生成两个实例用于管理内核内存池和用户内存池*/
struct pool
{
    struct bitmap pool_bitmap;   //本内存池用到的位图结构，用于管理物理内存
    uint32_t phy_addr_start;   //本内存池所管理物理内存的起始地址
    uint32_t pool_size;   //本内存池字节容量
    struct lock lock;   //申请内存时互斥
};

/*内存仓库*/
struct arena
{
    struct mem_block_desc* desc;   //此arena关联的mem_block_desc
    uint32_t cnt;
    bool large;   //为true时，cnt表示的是页框数，否则表示空闲mem_block数量，将来释放内存时要用到此项
};

struct mem_block_desc k_block_descs[DESC_CNT];   //内核内存块描述符数组

struct pool kernel_pool, user_pool;  //生成内核内存池和用户内存池
struct virtual_addr kernel_vaddr;   //此结构用来给内核分配虚拟地址

/*在pf表示的虚拟内存池中申请pg_cnt个虚拟页，成功返回虚拟页的起始地址，失败返回NULL*/
static void* vaddr_get(enum pool_flags pf, uint32_t pg_cnt)
{
    int vaddr_start = 0, bit_idx_start = -1;
    uint32_t cnt = 0;
    if(pf == PF_KERNEL) {   //内核内存池
        bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap, pg_cnt);
        if(bit_idx_start == -1) {
            return NULL;
        }
        while(cnt < pg_cnt) {
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);
        }
        vaddr_start = kernel_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
    } else {
        //用户内存池，将来实现用户进程补充
        struct task_struct* cur = running_thread();
        bit_idx_start = bitmap_scan(&cur->userprog_vaddr.vaddr_bitmap, pg_cnt);
        if(bit_idx_start == -1) {
            return NULL;
        }

        while(cnt < pg_cnt) {
            bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);
        }
        vaddr_start = cur->userprog_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
        //0xc0000000 - PG_SIZE作为用户3级栈已经在start_process被分配
        ASSERT((uint32_t)vaddr_start < (0xc0000000 - PG_SIZE));
    }
    return (void*)vaddr_start;
}

/*得到虚拟地址vaddr对应的pte指针*/
uint32_t* pte_ptr(uint32_t vaddr)
{
    //先放到页表自己 + 再用页目录项pde（页目录内页表的索引作为pte的索引访问到页表 + pte的索引作为业内偏移）
    uint32_t* pte = (uint32_t*)(0xffc00000 + ((vaddr & 0xffc00000) >> 10) + PTE_IDX(vaddr) * 4);
    return pte;
}

/*得到虚拟地址vaddr对应的pde的指针*/
uint32_t* pde_ptr(uint32_t vaddr)
{
    //0xfffff用来访问页表本身所在的地址
    uint32_t* pde = (uint32_t*)((0xfffff000) + PDE_IDX(vaddr) * 4);
    return pde;
}

/*在m_pool指向的物理内存池中分配1个物理页，成功则返回页框的物理地址，失败则返回NULL*/
static void* palloc(struct pool* m_pool)
{
    //扫描或设置要保证原子操作
    int bit_idx = bitmap_scan(&m_pool->pool_bitmap, 1);   //找到一个物理页面
    if(bit_idx == -1) {
        return NULL;
    }
    bitmap_set(&m_pool->pool_bitmap, bit_idx, 1);   //将此位置1
    uint32_t page_phyaddr = ((bit_idx * PG_SIZE) + m_pool->phy_addr_start);
    return (void*)page_phyaddr;
}

/*页表中添加虚拟地址_vaddr与物理地址_page_phyaddr的映射*/
static void page_table_add(void* _vaddr, void* _page_phyaddr)
{
    uint32_t vaddr = (uint32_t)_vaddr, page_phyaddr = (uint32_t)_page_phyaddr;
    uint32_t* pde = pde_ptr(vaddr);
    uint32_t* pte = pte_ptr(vaddr);

    //********************************************************************************
    //执行*pte，会访问到空的pde，所以确保pde创建完成后才能执行*pte
    //否则会引发page_fault。因此在*pde为0时，*pte只能出现在下面else语句块中的*pde后面
    //********************************************************************************
    //先在页目录内判断目录项的P位，若为1，则表示该表已存在
    if(*pde & 0x00000001) {
        ASSERT(!(*pte & 0x00000001));

        if(!(*pte & 0x00000001)) {   //只要是创建页表，pte就应该存在，此处多判断一下放心
            *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        } else {   //目前不会执行到这，因为上面的ASSERT会先执行
            PANIC("pte repeat");
            *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        }
    } else {   //页目录项不存在，所以要先创建页目录项在创建页表项
        //页表中用到的页框一律从内核空间分配
        uint32_t pde_phyaddr = (uint32_t)palloc(&kernel_pool);
        
        *pde = (pde_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        
        //分配道德物理页地址pde_phyaddr对应的物理内存清0，避免里面的陈旧数据编程页表项，让页表混乱
        //访问到pde对应的物理地址，用pte取高20位便可。因为pte基于该pde对应的物理地址内在寻址，第12位置0便是对应的物理页的起始地址
        memset((void*)((int)pte & 0xfffff000), 0, PG_SIZE);

        ASSERT(!(*pte & 0x00000001));
        *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
    }
}

/*分配pg_cnt个页空间，成功则返回虚拟地址，失败时返回NULL*/
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt)
{
    ASSERT(pg_cnt > 0 && pg_cnt < 3840);
    //1.通过vaddr_get在虚拟内存池中申请虚拟地址
    //2.通过palloc在物理内存池中申请物理页
    //3.通过page_table_add将以上得到的虚拟地址和物理地址在页表中完成映射
    void* vaddr_start = vaddr_get(pf, pg_cnt);
    if(vaddr_start == NULL) {
        return NULL;
    }

    uint32_t vaddr = (uint32_t)vaddr_start, cnt = pg_cnt;
    struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;

    //因为虚拟地址是连续的，但物理地址可以使不连续的，所以逐个映射
    while(cnt-- > 0) {
        void* page_phyaddr = palloc(mem_pool);
        if(page_phyaddr == NULL) {
            //失败时要将已申请的虚拟地址和物理页全部回滚，在将在完成内存回收时再补充
            return NULL;
        }
        page_table_add((void*)vaddr, page_phyaddr);   //在页表中做映射
        vaddr += PG_SIZE;   //下一个虚拟页
    }
    return vaddr_start;
}

/*从内核物理内存池中申请1页内存，成功返回虚拟地址，失败返回NULL*/
void* get_kernel_pages(uint32_t pg_cnt)
{
    lock_acquire(&kernel_pool.lock);
    void* vaddr = malloc_page(PF_KERNEL, pg_cnt);
    if(vaddr != NULL) {   //若分配的地址不为空，将页框清0后返回
        memset(vaddr, 0, pg_cnt * PG_SIZE);
    }
    lock_release(&kernel_pool.lock);
    return vaddr;
}

/*在用户空间申请4K内存，并返回其虚拟地址*/
void* get_user_pages(uint32_t pg_cnt)
{
    lock_acquire(&user_pool.lock);
    void* vaddr = malloc_page(PF_USER, pg_cnt);
    if(vaddr != NULL) {
        memset(vaddr, 0, pg_cnt * PG_SIZE);
    }
    lock_release(&user_pool.lock);
    return vaddr;
}

/*将地址vaddr与pf池的物理地址关联，仅支持一页空间分配*/
void* get_a_page(enum pool_flags pf, uint32_t vaddr)
{
    struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
    lock_acquire(&mem_pool->lock);

    //先将虚拟地址对应的位图置1
    struct task_struct* cur = running_thread();
    int32_t bit_idx = -1;

    //若当前是用户进程申请用户内存，就修改用户进程自己的虚拟地址位图
    if(cur->pgdir != NULL && pf == PF_USER) {
        bit_idx = (vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx, 1);
    } else if(cur->pgdir == NULL && pf == PF_KERNEL) {   //如果是内核内核线程申请内核内存，就修改kernel_vaddr
        bit_idx = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx, 1);
    } else {
        PANIC("get_a_page: not allow kernel alloc userapace or user alloc kernelspace by get_a_page");
    }

    void* page_phyaddr = palloc(mem_pool);
    if(page_phyaddr == NULL) {
        lock_release(&mem_pool->lock);
        return NULL;
    }
    page_table_add((void*)vaddr, page_phyaddr);
    lock_release(&mem_pool->lock);
    return (void*)vaddr;
}

/*得到虚拟地址映射到的物理地址*/
uint32_t addr_v2p(uint32_t vaddr)
{
    uint32_t* pte = pte_ptr(vaddr);   //(*pte)的值是页表所在的物理页框地址，去掉其低12位的页表属性+虚拟地址vaddr的低12位
    return ((*pte & 0xfffff000) + (vaddr & 0x00000fff));
}

/*init mem_block_descs，为malloc做准备*/
void block_desc_init(struct mem_block_desc* desc_array)
{
    uint16_t desc_idx, block_size = 16;

    //初始化每个mem_block_desc描述符
    for(desc_idx = 0; desc_idx < DESC_CNT; desc_idx++) {
        desc_array[desc_idx].block_size = block_size;

        //初始化arena中内存块数量
        desc_array[desc_idx].blocks_per_arena = (PG_SIZE - sizeof(struct arena)) / block_size;

        list_init(&desc_array[desc_idx].free_list);

        block_size *= 2;   //更新为下一个规格内存块
    }
}

/*返回arena中第idx个内存块的地址*/
static struct mem_block* arena2block(struct arena* a, uint32_t idx)
{
    return (struct mem_block*)((uint32_t)a + sizeof(struct arena) + idx * a->desc->block_size);
}

/*返回内存块b所在的arena地址*/
static struct arena* block2arena(struct mem_block* b)
{
    return (struct arena*)((uint32_t)b & 0xfffff000);   //arena占据一个页框
}

/*在堆中申请size字节内存*/
void* sys_malloc(uint32_t size)
{
    enum pool_flags PF;
    struct pool* mem_pool;
    uint32_t pool_size;
    struct mem_block_desc* descs;
    struct task_struct* cur_thread = running_thread();

    //判断在那个内存池
    if(cur_thread->pgdir == NULL) {
        PF = PF_KERNEL;   //为内核线程
        pool_size = kernel_pool.pool_size;
        mem_pool = &kernel_pool;
        descs = k_block_descs;
    } else {
        PF = PF_USER;   //为用户线程，用户进程pcb中的pgdir会在为其分配页目录表时创建
        pool_size = user_pool.pool_size;
        mem_pool = &user_pool;
        descs = cur_thread->u_block_desc;
    }

    //若申请的内存不在内存池容量范围内，则直接返回NULL
    if(!(size > 0 && size < pool_size)) {
        return NULL;
    }

    struct arena* a;
    struct mem_block* b;

    lock_acquire(&mem_pool->lock);

    //超过最大内存块1024，就分配页框
    if(size > 1024) {
        uint32_t page_cnt = DIV_ROUND_UP(size + sizeof(struct arena), PG_SIZE);   //向上取整获得需要的页框数
        a = malloc_page(PF, page_cnt);   //在PF内存池中分配page_cnt个页框

        if(a != NULL) {
            memset(a, 0, page_cnt * PG_SIZE);   //将分配的内存清零
            //对于分配的大块页框，将desc置为NULL，cnt置为页框数，large置为true
            a->desc = NULL;
            a->cnt = page_cnt;
            a->large = true;

            lock_release(&mem_pool->lock);
            return (void*)(a + 1);   //跨过arena大小，把剩下的内存返回
        } else {
            lock_release(&mem_pool->lock);
            return NULL;
        }
    } else {
        uint8_t desc_idx;

        //从内存块描述符中匹配合适的内存块规格
        for(desc_idx = 0; desc_idx < DESC_CNT; desc_idx++) {
            if(size <= descs[desc_idx].block_size) {
                break;
            }
        }

        //若mem_block_desc的free_list中已经没有可用的mem_block，就创建新的arena提供mem_block
        if(list_empty(&descs[desc_idx].free_list)) {
            a = malloc_page(PF, 1);   //分配一页作为arena
            if(a == NULL) {
                lock_release(&mem_pool->lock);
                return NULL;
            }
            memset(a, 0, PG_SIZE);   //分配的页框内容清零

            //对于分配的小内存块，将desc置为相应内存块描述符，cnt置为arena可用的内存块数量，large置为false
            a->desc = &descs[desc_idx];
            a->large = false;
            a->cnt = descs[desc_idx].blocks_per_arena;
            
            uint32_t block_idx;
            enum intr_status old_status = intr_disable();

            //开始将arena拆分成内存块，并添加到内存块描述符的free_lsit中
            for(block_idx = 0; block_idx < descs[desc_idx].blocks_per_arena; block_idx++) {
                b = arena2block(a, block_idx);
                ASSERT(!elem_find(&a->desc->free_list, &b->free_elem));
                list_append(&a->desc->free_list, &b->free_elem);
            }

            intr_set_status(old_status);
        }

        //开始分配内存块
        b = elem2entry(struct mem_block, free_elem, list_pop(&(descs[desc_idx].free_list)));
        memset(b, 0, descs[desc_idx].block_size);

        a = block2arena(b);   //获取内存块b所在的arena
        a->cnt--;   //将此arena中空闲的内存块数-1
        lock_release(&mem_pool->lock);
        return (void*)b;
    }
}

/*将物理地址pg_phy_addr回收到物理内存池。将物理页对应的位图中的那一位值0*/
void pfree(uint32_t pg_phy_addr)
{
    struct pool* mem_pool;
    uint32_t bit_idx = 0;
    if(pg_phy_addr >= user_pool.phy_addr_start) {   //用户物理内存池
        mem_pool = &user_pool;
        bit_idx = (pg_phy_addr - user_pool.phy_addr_start) / PG_SIZE;
    } else {   //内核物理内存池
        mem_pool = &kernel_pool;
        bit_idx = (pg_phy_addr - kernel_pool.phy_addr_start) / PG_SIZE;
    }
    bitmap_set(&mem_pool->pool_bitmap, bit_idx, 0);   //将位图中该位清0
}

/*去掉页表中虚拟地址vaddr的映射，只去掉vaddr的pte。将vaddr对应的页表项的p位置0*/
static void page_table_pte_remove(uint32_t vaddr)
{
    uint32_t *pte = pte_ptr(vaddr);   //获得虚拟地址的pte地址
    *pte &= ~PG_P_1;   //将pte的p位置0
    asm volatile ("invlpg %0" : : "m"(vaddr) : "memory");   //更新块表tlb
}

/*在虚拟地址池中释放以_vaddr起始的连续pg_cnt个虚拟地址页。将虚拟页对应的位图中的那一位置0*/
static void vaddr_remove(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt)
{
    uint32_t bit_idx_start = 0, vaddr = (uint32_t)_vaddr, cnt = 0;

    if(pf == PF_KERNEL) {
        bit_idx_start = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        while(cnt < pg_cnt) {
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 0);
        }
    } else {
        struct task_struct* cur_thread = running_thread();
        bit_idx_start = (vaddr - cur_thread->userprog_vaddr.vaddr_start) / PG_SIZE;
        while(cnt < pg_cnt) {
            bitmap_set(&cur_thread->userprog_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 0);
        }
    }
}

/*释放以虚拟地址vaddr起始的cnt个物理页框*/
void mfree_page(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt)
{
    uint32_t pg_phy_addr;
    uint32_t vaddr = (int32_t)_vaddr, page_cnt = 0;
    ASSERT(pg_cnt >= 1 && vaddr % PG_SIZE == 0);
    pg_phy_addr = addr_v2p(vaddr);   //获取虚拟地址vaddr对应的物理地址

    //确保待释放的物理内存在低端1MB+1KB的页目录+1KB的页表地址范围外
    ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= 0x102000);

    //判断pg_phy_addr属于用户物理内存池还是内核物理内存池
    if(pg_phy_addr >= user_pool.phy_addr_start) {
        vaddr -= PG_SIZE;
        while(page_cnt < pg_cnt) {
            vaddr += PG_SIZE;
            pg_phy_addr = addr_v2p(vaddr);
            //确保物理地址属于用户物理内存池
            ASSERT((pg_phy_addr & PG_SIZE) == 0 && pg_phy_addr >= user_pool.phy_addr_start);
            //1. 先将对应的物理页框归还到内存池
            pfree(pg_phy_addr);
            //2. 从也表中清楚此虚拟地址所在的页表项pte
            page_table_pte_remove(vaddr);
            page_cnt++;
        }
        //3. 清空虚拟地址中的位图中的相应位
        vaddr_remove(pf, _vaddr, pg_cnt);
    } else {
        vaddr -= PG_SIZE;
        while(page_cnt < pg_cnt) {
            vaddr += PG_SIZE;
            pg_phy_addr = addr_v2p(vaddr);
            //确保待释放的物理内存只属于内核物理内存池
            ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= kernel_pool.phy_addr_start && pg_phy_addr < user_pool.phy_addr_start);
            //1. 将对应的物理页框归还到内存池
            pfree(pg_phy_addr);
            //2. 从页表中清除此虚拟地址所在的页表项pte
            page_table_pte_remove(vaddr);
            page_cnt++;
        }
        //3. 清空虚拟地址中位图中的相应位
        vaddr_remove(pf, _vaddr, pg_cnt);
    }
}

/*回收内存ptr*/
void sys_free(void* ptr)
{
    ASSERT(ptr != NULL);
    if(ptr != NULL) {
        enum pool_flags PF;
        struct pool* mem_pool;

        //判断是线程，还是进程
        if(running_thread()->pgdir == NULL) {   //kernel-thread
            ASSERT((uint32_t)ptr >= K_HEAP_START);
            PF = PF_KERNEL;
            mem_pool = &kernel_pool;
        } else {   //user-program
            PF = PF_USER;
            mem_pool = &user_pool;
        }

        lock_acquire(&mem_pool->lock);
        struct mem_block* b = ptr;   //将ptr赋值给内存块指针b
        struct arena* a = block2arena(b);   //把mem_block转换成arena，获取元信息，内存块b的元信息为struct arena* a
        ASSERT(a->large == 0 || a->large == 1);
        if(a->desc == NULL && a->large == true) {
            mfree_page(PF, a, a->cnt);
        } else {
            //先将内存块回收到free_list
            list_append(&a->desc->free_list, &b->free_elem);
            //再判断此arena中的内存块是否都是空闲，如果是就释放arena
            if(++a->cnt == a->desc->blocks_per_arena) {
                uint32_t block_idx;
                for(block_idx = 0; block_idx < a->desc->blocks_per_arena; block_idx++) {
                    struct mem_block* b = arena2block(a, block_idx);
                    ASSERT(elem_find(&a->desc->free_list, &b->free_elem));
                    list_remove(&b->free_elem);
                }
                mfree_page(PF, a, 1);
            }
        }
        lock_release(&mem_pool->lock);
    }
}

/*初始化内存池*/
static void mem_pool_init(uint32_t all_mem)
{
    put_str("   mem_pool_init start\n");
    uint32_t page_table_size = PG_SIZE * 256;   //页表大小 = 1页的页目录表 + 第0和第768个页目录指向同一个页表 + 
                                                //第769~1022个页目录项共指向254个页表，共256个页框
    uint32_t used_mem = page_table_size + 0x100000;   //0x100000为低端1MB内存
    uint32_t free_mem = all_mem - used_mem;
    uint16_t all_free_pages = free_mem / PG_SIZE;   //1页为4KB，不管总内存是不是4K的倍数
                                                    //对于以页为单位的内存分配策略，不足1页的内存不用考虑
    uint16_t kernel_free_pages = all_free_pages / 2;   //可给内核分配的内存页数
    uint16_t user_free_pages = all_free_pages - kernel_free_pages;   //可给用户程序分配的内存页数

    //为简化位图操作，余数不处理，坏处是这样做会丢内存。
    //好处是不用做内存的越界检查，因为位图表示的内存少于实际物理内存
    uint32_t kbm_length = kernel_free_pages / 8;   //Kernel Bitmap的长度，位图中的一位表示一页，以字节为单位
    uint32_t ubm_length = user_free_pages / 8;   //User Bitmap的长度，以字节为单位

    uint32_t kp_start = used_mem;   //kernel pool start，内核物理内存池的起始地址
    uint32_t up_start = kp_start + kernel_free_pages * PG_SIZE;   //user pool start，用户物理内存池的起始地址

    //初始化内存池结构体
    kernel_pool.phy_addr_start = kp_start;
    user_pool.phy_addr_start = up_start;

    kernel_pool.pool_size = kernel_free_pages * PG_SIZE;
    user_pool.pool_size = user_free_pages * PG_SIZE;

    kernel_pool.pool_bitmap.btmp_bytes_len = kbm_length;
    user_pool.pool_bitmap.btmp_bytes_len = ubm_length;

    /*
     * ****** 内核内存池和用户内存池位图 ******
     * 位图是全局的数据，长度不固定。
     * 全局或静态的数组需要在编译时知道其长度，
     * 而我们需要根据总内存大小算出需要多少字节，
     * 所以改为指定一块内存来生成位图
     * ***************************************
    */
    //内核使用的最高地址是0xc009f000，这是主线程的栈地址（内核大小预计为70KB左右）
    //32MB内存占用的位图是2KB，内核内存池的位图先定在MEM_BITMAP_BASE(0xc009a000)处
    kernel_pool.pool_bitmap.bits = (void*)MEM_BITMAP_BASE;
    /*用户内存池的位图紧跟在内核内存池位图之后*/
    user_pool.pool_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length);

    //输出内存池信息
    put_str("   kernel_pool_bitmap_start:");
    put_int((int)kernel_pool.pool_bitmap.bits);
    put_str("\n");
    put_str("  kerenl_pool_phy_addr_start:");
    put_int(kernel_pool.phy_addr_start);
    put_str("\n");
    put_str("user_pool_bitmap_start:");
    put_int((int)user_pool.pool_bitmap.bits);
    put_str("\n");
    put_str("user_pool_phy_addr_start:");
    put_int(user_pool.phy_addr_start);
    put_str("\n");

    //将位图置0
    bitmap_init(&kernel_pool.pool_bitmap);
    bitmap_init(&user_pool.pool_bitmap);

    //下面初始化内核虚拟地址的位图，按实际物理内存大小生成数组
    kernel_vaddr.vaddr_bitmap.btmp_bytes_len = kbm_length;   //用于维护内核堆栈的虚拟地址，所以要和内核内存池大小一致
    kernel_vaddr.vaddr_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length + ubm_length);   //位图的数组指向一块未使用的内存，目前定位在内核内存池和用户内存池之外
    kernel_vaddr.vaddr_start = K_HEAP_START;
    
    bitmap_init(&kernel_vaddr.vaddr_bitmap);

    lock_init(&kernel_pool.lock);
    lock_init(&user_pool.lock);

    put_str("mem_init done\n");
}

/*内存管理部分初始化入口*/
void mem_init()
{
    put_str("mem_init start\n");
    uint32_t mem_bytes_total = *((uint32_t*)(0xb00));
    mem_pool_init(mem_bytes_total);   //初始化内存池
    block_desc_init(k_block_descs);   //初始化mem_block_descs数组descs，为malloc做准备
    put_str("mem_init done\n");
}
