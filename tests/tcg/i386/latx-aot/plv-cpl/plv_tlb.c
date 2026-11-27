#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <asm/pgtable.h>

#define PLV_TEST_MAGIC 0x504c5633U
#define PLV_TEST_TOUCH _IO('P', 1)

static struct page *plv_page;

static int make_pte_supervisor(struct vm_area_struct *vma, unsigned long addr)
{
    struct mm_struct *mm = vma->vm_mm;
    pgd_t *pgd = pgd_offset(mm, addr);
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    spinlock_t *ptl;

    if (pgd_none(*pgd) || pgd_bad(*pgd)) {
        return -EFAULT;
    }
    pud = pud_offset(pgd, addr);
    if (pud_none(*pud) || pud_bad(*pud)) {
        return -EFAULT;
    }
    pmd = pmd_offset(pud, addr);
    if (pmd_none(*pmd) || pmd_bad(*pmd)) {
        return -EFAULT;
    }

    pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
    if (!pte_present(*pte)) {
        pte_unmap_unlock(pte, ptl);
        return -EFAULT;
    }
    pr_info("plv_tlb: PTE before supervisor=%llx\n",
            (unsigned long long)pte_val(*pte));
    set_pte_at(mm, addr, pte, __pte(pte_val(*pte) & ~_PAGE_USER));
    pr_info("plv_tlb: PTE after supervisor=%llx\n",
            (unsigned long long)pte_val(*pte));
    pte_unmap_unlock(pte, ptl);
    asm volatile("invlpg (%0)" : : "r" (addr) : "memory");
    return 0;
}

static int plv_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    int ret;

    if (vma->vm_pgoff != 0 || size != PAGE_SIZE) {
        return -EINVAL;
    }

    vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot) &
                                 ~_PAGE_USER);
    ret = remap_pfn_range(vma, vma->vm_start, page_to_pfn(plv_page),
                          PAGE_SIZE, vma->vm_page_prot);
    if (ret) {
        return ret;
    }
    return make_pte_supervisor(vma, vma->vm_start);
}

static long plv_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    uint32_t value;

    if (cmd != PLV_TEST_TOUCH) {
        return -ENOTTY;
    }
    if (!access_ok(VERIFY_READ, (void __user *)arg, sizeof(value))) {
        return -EFAULT;
    }
    if (get_user(value, (uint32_t __user *)arg)) {
        return -EFAULT;
    }
    return value == PLV_TEST_MAGIC ? 0 : -EIO;
}

static const struct file_operations plv_fops = {
    .owner = THIS_MODULE,
    .mmap = plv_mmap,
    .unlocked_ioctl = plv_ioctl,
};

static struct miscdevice plv_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "plv_tlb",
    .fops = &plv_fops,
    .mode = 0600,
};

static int __init plv_tlb_init(void)
{
    void *addr;
    int ret;

    plv_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!plv_page) {
        return -ENOMEM;
    }
    SetPageReserved(plv_page);

    addr = kmap(plv_page);
    *(uint32_t *)addr = PLV_TEST_MAGIC;
    kunmap(plv_page);

    ret = misc_register(&plv_misc);
    if (ret) {
        ClearPageReserved(plv_page);
        __free_page(plv_page);
        return ret;
    }
    pr_info("plv_tlb: loaded, /dev/plv_tlb ready\n");
    return 0;
}

static void __exit plv_tlb_exit(void)
{
    misc_deregister(&plv_misc);
    ClearPageReserved(plv_page);
    __free_page(plv_page);
    pr_info("plv_tlb: unloaded\n");
}

module_init(plv_tlb_init);
module_exit(plv_tlb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal CPL0/CPL3 HAMT PLV test helper");
