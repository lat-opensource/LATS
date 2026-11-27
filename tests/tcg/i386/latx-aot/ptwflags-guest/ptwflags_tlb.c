#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <asm/pgtable.h>

#define PTWFLAGS_TLB_TOUCH _IO('P', 2)

#define PTWFLAGS_MAP_RAM        0
#define PTWFLAGS_MAP_VGA        1
#define PTWFLAGS_MAP_MMIO       2
#define PTWFLAGS_MAP_SUPERVISOR 3
#define PTWFLAGS_MAP_READONLY   4

#define VGA_PFN  0xa0UL
#define MMIO_PFN 0xc0000UL

static struct page *ram_page;

static int clear_pte_flags(struct vm_area_struct *vma, unsigned long addr,
			   unsigned long clear)
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
	set_pte_at(mm, addr, pte, __pte(pte_val(*pte) & ~clear));
	pte_unmap_unlock(pte, ptl);
	asm volatile("invlpg (%0)" : : "r" (addr) : "memory");
	return 0;
}

static int ptwflags_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pfn;
	int ret;

	if (size != PAGE_SIZE) {
		return -EINVAL;
	}

	switch (vma->vm_pgoff) {
	case PTWFLAGS_MAP_RAM:
	case PTWFLAGS_MAP_SUPERVISOR:
	case PTWFLAGS_MAP_READONLY:
		pfn = page_to_pfn(ram_page);
		vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
		break;
	case PTWFLAGS_MAP_VGA:
		pfn = VGA_PFN;
		vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		break;
	case PTWFLAGS_MAP_MMIO:
		pfn = MMIO_PFN;
		vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		break;
	default:
		return -EINVAL;
	}

	if (vma->vm_pgoff == PTWFLAGS_MAP_SUPERVISOR) {
		vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot) &
					    ~_PAGE_USER);
	}

	ret = remap_pfn_range(vma, vma->vm_start, pfn, PAGE_SIZE,
			      vma->vm_page_prot);
	if (ret) {
		return ret;
	}

	if (vma->vm_pgoff == PTWFLAGS_MAP_SUPERVISOR) {
		ret = clear_pte_flags(vma, vma->vm_start, _PAGE_USER);
	} else if (vma->vm_pgoff == PTWFLAGS_MAP_READONLY) {
		ret = clear_pte_flags(vma, vma->vm_start, _PAGE_RW);
	}
	return ret;
}

static long ptwflags_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	uint32_t value;

	if (cmd != PTWFLAGS_TLB_TOUCH) {
		return -ENOTTY;
	}
	if (!access_ok(VERIFY_READ, (void __user *)arg, sizeof(value))) {
		return -EFAULT;
	}
	if (get_user(value, (uint32_t __user *)arg)) {
		return -EFAULT;
	}
	return value ? 0 : -EIO;
}

static const struct file_operations ptwflags_fops = {
	.owner = THIS_MODULE,
	.mmap = ptwflags_mmap,
	.unlocked_ioctl = ptwflags_ioctl,
};

static struct miscdevice ptwflags_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ptwflags_tlb",
	.fops = &ptwflags_fops,
	.mode = 0600,
};

static int __init ptwflags_tlb_init(void)
{
	void *addr;
	int ret;

	ram_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!ram_page) {
		return -ENOMEM;
	}
	SetPageReserved(ram_page);

	addr = kmap(ram_page);
	/* mov $0x13572468, %eax; ret */
	((uint8_t *)addr)[0] = 0xb8;
	((uint8_t *)addr)[1] = 0x68;
	((uint8_t *)addr)[2] = 0x24;
	((uint8_t *)addr)[3] = 0x57;
	((uint8_t *)addr)[4] = 0x13;
	((uint8_t *)addr)[5] = 0xc3;
	kunmap(ram_page);

	ret = misc_register(&ptwflags_misc);
	if (ret) {
		ClearPageReserved(ram_page);
		__free_page(ram_page);
		return ret;
	}
	pr_info("ptwflags_tlb: /dev/ptwflags_tlb ready\n");
	return 0;
}

static void __exit ptwflags_tlb_exit(void)
{
	misc_deregister(&ptwflags_misc);
	ClearPageReserved(ram_page);
	__free_page(ram_page);
}

module_init(ptwflags_tlb_init);
module_exit(ptwflags_tlb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Physical mappings for HAMT ptwflags guest tests");
