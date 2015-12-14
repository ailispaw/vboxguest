/* $Id: regops.c 97541 2015-01-07 10:23:32Z fmehnert $ */
/** @file
 * vboxsf - VBox Linux Shared Folders, Regular file inode and file operations.
 */

/*
 * Copyright (C) 2006-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*
 * Limitations: only COW memory mapping is supported
 */

#include "vfsmod.h"

static void *alloc_bounce_buffer(size_t *tmp_sizep, PRTCCPHYS physp, size_t
                                 xfer_size, const char *caller)
{
    size_t tmp_size;
    void *tmp;

    /* try for big first. */
    tmp_size = RT_ALIGN_Z(xfer_size, PAGE_SIZE);
    if (tmp_size > 128U*_1K)
        tmp_size = 128U*_1K;
    tmp = kmalloc(tmp_size, GFP_KERNEL);
    if (!tmp)
    {
        /* fall back on a page sized buffer. */
        tmp = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (!tmp)
        {
            LogRel(("%s: could not allocate bounce buffer for xfer_size=%zu %s\n", caller, xfer_size));
            return NULL;
        }
        tmp_size = PAGE_SIZE;
    }

    *tmp_sizep = tmp_size;
    *physp = virt_to_phys(tmp);
    return tmp;
}

static void free_bounce_buffer(void *tmp)
{
    kfree (tmp);
}


/* fops */
static int sf_reg_read_aux(const char *caller, struct sf_glob_info *sf_g,
                           struct sf_reg_info *sf_r, void *buf,
                           uint32_t *nread, uint64_t pos)
{
    /** @todo bird: yes, kmap() and kmalloc() input only. Since the buffer is
     *        contiguous in physical memory (kmalloc or single page), we should
     *        use a physical address here to speed things up. */
    int rc = vboxCallRead(&client_handle, &sf_g->map, sf_r->handle,
                          pos, nread, buf, false /* already locked? */);
    if (RT_FAILURE(rc))
    {
        LogFunc(("vboxCallRead failed. caller=%s, rc=%Rrc\n", caller, rc));
        return -EPROTO;
    }
    return 0;
}

static int sf_reg_write_aux(const char *caller, struct sf_glob_info *sf_g,
                            struct sf_reg_info *sf_r, void *buf,
                            uint32_t *nwritten, uint64_t pos)
{
    /** @todo bird: yes, kmap() and kmalloc() input only. Since the buffer is
     *        contiguous in physical memory (kmalloc or single page), we should
     *        use a physical address here to speed things up. */
    int rc = vboxCallWrite(&client_handle, &sf_g->map, sf_r->handle,
                           pos, nwritten, buf, false /* already locked? */);
    if (RT_FAILURE(rc))
    {
        LogFunc(("vboxCallWrite failed. caller=%s, rc=%Rrc\n",
                    caller, rc));
        return -EPROTO;
    }
    return 0;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)

#include <linux/nfs_fs.h>
static ssize_t
sf_file_read(struct kiocb *iocb, struct iov_iter *iov)
{
   int err;
   struct dentry *dentry;

   dentry = iocb->ki_filp->f_path.dentry;
   err = sf_inode_revalidate(dentry);
   if (err)
       return err;
   return generic_file_read_iter(iocb, iov);
}

static int sf_need_sync_write(struct file *file, struct inode *inode)
{
    // >= kernel version 2.6.33
    if (IS_SYNC(inode) || file->f_flags & O_DSYNC) {
       return 1;
    }
    return 0;
}

static ssize_t
sf_file_write(struct kiocb *iocb, struct iov_iter *iov)
{
   int err;
   ssize_t result;
   struct file *file = iocb->ki_filp;
   struct dentry *dentry = file->f_path.dentry;
   struct inode *inode = dentry->d_inode;

   err = sf_inode_revalidate(dentry);
   if (err)
       return err;

   result = generic_file_write_iter(iocb, iov);

   if (result >= 0 && sf_need_sync_write(file, inode)) {
      err = vfs_fsync(file, 0);
      if (err < 0) {
         result = err;
      }
   }
   return result;
}

#else /* KERNEL_VERSION >= 3.16.0 */
/**
 * Read from a regular file.
 *
 * @param file          the file
 * @param buf           the buffer
 * @param size          length of the buffer
 * @param off           offset within the file
 * @returns the number of read bytes on success, Linux error code otherwise
 */
static ssize_t sf_reg_read(struct file *file, char *buf, size_t size, loff_t *off)
{
    int err;
    void *tmp;
    RTCCPHYS tmp_phys;
    size_t tmp_size;
    size_t left = size;
    ssize_t total_bytes_read = 0;
    struct inode *inode = GET_F_DENTRY(file)->d_inode;
    struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
    struct sf_reg_info *sf_r = file->private_data;
    loff_t pos = *off;

    TRACE();
    if (!S_ISREG(inode->i_mode))
    {
        LogFunc(("read from non regular file %d\n", inode->i_mode));
        return -EINVAL;
    }

    /** XXX Check read permission according to inode->i_mode! */

    if (!size)
        return 0;

    tmp = alloc_bounce_buffer(&tmp_size, &tmp_phys, size, __PRETTY_FUNCTION__);
    if (!tmp)
        return -ENOMEM;

    while (left)
    {
        uint32_t to_read, nread;

        to_read = tmp_size;
        if (to_read > left)
            to_read = (uint32_t) left;

        nread = to_read;

        err = sf_reg_read_aux(__func__, sf_g, sf_r, tmp, &nread, pos);
        if (err)
            goto fail;

        if (copy_to_user(buf, tmp, nread))
        {
            err = -EFAULT;
            goto fail;
        }

        pos  += nread;
        left -= nread;
        buf  += nread;
        total_bytes_read += nread;
        if (nread != to_read)
            break;
    }

    *off += total_bytes_read;
    free_bounce_buffer(tmp);
    return total_bytes_read;

fail:
    free_bounce_buffer(tmp);
    return err;
}

/**
 * Write to a regular file.
 *
 * @param file          the file
 * @param buf           the buffer
 * @param size          length of the buffer
 * @param off           offset within the file
 * @returns the number of written bytes on success, Linux error code otherwise
 */
static ssize_t sf_reg_write(struct file *file, const char *buf, size_t size, loff_t *off)
{
    int err;
    void *tmp;
    RTCCPHYS tmp_phys;
    size_t tmp_size;
    size_t left = size;
    ssize_t total_bytes_written = 0;
    struct inode *inode = GET_F_DENTRY(file)->d_inode;
    struct sf_inode_info *sf_i = GET_INODE_INFO(inode);
    struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
    struct sf_reg_info *sf_r = file->private_data;
    loff_t pos;

    TRACE();
    BUG_ON(!sf_i);
    BUG_ON(!sf_g);
    BUG_ON(!sf_r);

    if (!S_ISREG(inode->i_mode))
    {
        LogFunc(("write to non regular file %d\n",  inode->i_mode));
        return -EINVAL;
    }

    pos = *off;
    if (file->f_flags & O_APPEND)
    {
        pos = inode->i_size;
        *off = pos;
    }

    /** XXX Check write permission according to inode->i_mode! */

    if (!size)
        return 0;

    tmp = alloc_bounce_buffer(&tmp_size, &tmp_phys, size, __PRETTY_FUNCTION__);
    if (!tmp)
        return -ENOMEM;

    while (left)
    {
        uint32_t to_write, nwritten;

        to_write = tmp_size;
        if (to_write > left)
            to_write = (uint32_t) left;

        nwritten = to_write;

        if (copy_from_user(tmp, buf, to_write))
        {
            err = -EFAULT;
            goto fail;
        }

#if 1
        if (VbglR0CanUsePhysPageList())
        {
            err = VbglR0SfWritePhysCont(&client_handle, &sf_g->map, sf_r->handle,
                                        pos, &nwritten, tmp_phys);
            err = RT_FAILURE(err) ? -EPROTO : 0;
        }
        else
#endif
            err = sf_reg_write_aux(__func__, sf_g, sf_r, tmp, &nwritten, pos);
        if (err)
            goto fail;

        pos  += nwritten;
        left -= nwritten;
        buf  += nwritten;
        total_bytes_written += nwritten;
        if (nwritten != to_write)
            break;
    }

    *off += total_bytes_written;
    if (*off > inode->i_size)
        inode->i_size = *off;

    sf_i->force_restat = 1;
    free_bounce_buffer(tmp);
    return total_bytes_written;

fail:
    free_bounce_buffer(tmp);
    return err;
}
#endif /* KERNEL_VERSION >= 3.16.0 */

static loff_t
sf_file_llseek(struct file *file, loff_t offset, int origin)
{
   int err;
   struct dentry *dentry;

   dentry = file->f_path.dentry;
   err = sf_inode_revalidate(dentry);
   if (err)
       return err;
   return generic_file_llseek(file, offset, origin);
}

# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 23)
static ssize_t
sf_file_splice_read(struct file *file, loff_t *offset, struct pipe_inode_info *pipe, size_t len, unsigned int flags)
{
   int err;
   struct dentry *dentry;

   dentry = file->f_path.dentry;
   err = sf_inode_revalidate(dentry);
   if (err)
       return err;
   return generic_file_splice_read(file, offset, pipe, len, flags);
}
# endif

/**
 * Open a regular file.
 *
 * @param inode         the inode
 * @param file          the file
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_reg_open(struct inode *inode, struct file *file)
{
    int rc, rc_linux = 0;
    struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
    struct sf_inode_info *sf_i = GET_INODE_INFO(inode);
    struct sf_reg_info *sf_r;
    SHFLCREATEPARMS params;

    TRACE();
    BUG_ON(!sf_g);
    BUG_ON(!sf_i);

    LogFunc(("open %s\n", sf_i->path->String.utf8));

    sf_r = kmalloc(sizeof(*sf_r), GFP_KERNEL);
    if (!sf_r)
    {
        LogRelFunc(("could not allocate reg info\n"));
        return -ENOMEM;
    }

    /* Already open? */
    if (sf_i->handle != SHFL_HANDLE_NIL)
    {
        /*
         * This inode was created with sf_create_aux(). Check the CreateFlags:
         * O_CREAT, O_TRUNC: inherent true (file was just created). Not sure
         * about the access flags (SHFL_CF_ACCESS_*).
         */
        sf_i->force_restat = 1;
        sf_r->handle = sf_i->handle;
        sf_i->handle = SHFL_HANDLE_NIL;
        sf_r->CreateFlags = 0
                           | SHFL_CF_ACT_CREATE_IF_NEW
                           | SHFL_CF_ACT_FAIL_IF_EXISTS
                           | SHFL_CF_ACCESS_READWRITE
                           ;
        list_add_tail(&sf_r->head, &sf_i->regs);
        file->private_data = sf_r;
        return 0;
    }

    RT_ZERO(params);
    params.Handle = SHFL_HANDLE_NIL;
    /* We check the value of params.Handle afterwards to find out if
     * the call succeeded or failed, as the API does not seem to cleanly
     * distinguish error and informational messages.
     *
     * Furthermore, we must set params.Handle to SHFL_HANDLE_NIL to
     * make the shared folders host service use our fMode parameter */

    if (file->f_flags & O_CREAT)
    {
        LogFunc(("O_CREAT set\n"));
        params.CreateFlags |= SHFL_CF_ACT_CREATE_IF_NEW;
        /* We ignore O_EXCL, as the Linux kernel seems to call create
           beforehand itself, so O_EXCL should always fail. */
        if (file->f_flags & O_TRUNC)
        {
            LogFunc(("O_TRUNC set\n"));
            params.CreateFlags |= (  SHFL_CF_ACT_OVERWRITE_IF_EXISTS
                                   | SHFL_CF_ACCESS_WRITE);
        }
        else
            params.CreateFlags |= SHFL_CF_ACT_OPEN_IF_EXISTS;
    }
    else
    {
        params.CreateFlags |= SHFL_CF_ACT_FAIL_IF_NEW;
        if (file->f_flags & O_TRUNC)
        {
            LogFunc(("O_TRUNC set\n"));
            params.CreateFlags |= (  SHFL_CF_ACT_OVERWRITE_IF_EXISTS
                    | SHFL_CF_ACCESS_WRITE);
        }
    }

    if (!(params.CreateFlags & SHFL_CF_ACCESS_READWRITE))
    {
        switch (file->f_flags & O_ACCMODE)
        {
            case O_RDONLY:
                params.CreateFlags |= SHFL_CF_ACCESS_READ;
                break;

            case O_WRONLY:
                params.CreateFlags |= SHFL_CF_ACCESS_WRITE;
                break;

            case O_RDWR:
                params.CreateFlags |= SHFL_CF_ACCESS_READWRITE;
                break;

            default:
                BUG ();
        }
    }

    if (file->f_flags & O_APPEND)
    {
        LogFunc(("O_APPEND set\n"));
        params.CreateFlags |= SHFL_CF_ACCESS_APPEND;
    }

    params.Info.Attr.fMode = inode->i_mode;
    LogFunc(("sf_reg_open: calling vboxCallCreate, file %s, flags=%#x, %#x\n",
              sf_i->path->String.utf8 , file->f_flags, params.CreateFlags));
    rc = vboxCallCreate(&client_handle, &sf_g->map, sf_i->path, &params);
    if (RT_FAILURE(rc))
    {
        LogFunc(("vboxCallCreate failed flags=%d,%#x rc=%Rrc\n",
                  file->f_flags, params.CreateFlags, rc));
        kfree(sf_r);
        return -RTErrConvertToErrno(rc);
    }

    if (SHFL_HANDLE_NIL == params.Handle)
    {
        switch (params.Result)
        {
            case SHFL_PATH_NOT_FOUND:
            case SHFL_FILE_NOT_FOUND:
                rc_linux = -ENOENT;
                break;
            case SHFL_FILE_EXISTS:
                rc_linux = -EEXIST;
                break;
            default:
                break;
        }
    }

    sf_i->force_restat = 1;
    sf_r->handle = params.Handle;
    sf_r->CreateFlags = params.CreateFlags;
    list_add_tail(&sf_r->head, &sf_i->regs);
    file->private_data = sf_r;
    return rc_linux;
}

/**
 * Close a regular file.
 *
 * @param inode         the inode
 * @param file          the file
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_reg_release(struct inode *inode, struct file *file)
{
    int rc;
    struct sf_reg_info *sf_r;
    struct sf_glob_info *sf_g;
    struct sf_inode_info *sf_i = GET_INODE_INFO(inode);

    TRACE();
    sf_g = GET_GLOB_INFO(inode->i_sb);
    sf_r = file->private_data;

    BUG_ON(!sf_g);
    BUG_ON(!sf_r);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 25)
    /* See the smbfs source (file.c). mmap in particular can cause data to be
     * written to the file after it is closed, which we can't cope with.  We
     * copy and paste the body of filemap_write_and_wait() here as it was not
     * defined before 2.6.6 and not exported until quite a bit later. */
    /* filemap_write_and_wait(inode->i_mapping); */
    if (   inode->i_mapping->nrpages
        && filemap_fdatawrite(inode->i_mapping) != -EIO)
        filemap_fdatawait(inode->i_mapping);
#endif
    rc = vboxCallClose(&client_handle, &sf_g->map, sf_r->handle);
    if (RT_FAILURE(rc))
        LogFunc(("vboxCallClose failed rc=%Rrc\n", rc));

    kfree(sf_r);
    list_del_init(&sf_r->head);
    sf_i->handle = SHFL_HANDLE_NIL;
    file->private_data = NULL;
    return 0;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 25)
static int sf_reg_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
static struct page *sf_reg_nopage(struct vm_area_struct *vma, unsigned long vaddr, int *type)
# define SET_TYPE(t) *type = (t)
#else /* LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0) */
static struct page *sf_reg_nopage(struct vm_area_struct *vma, unsigned long vaddr, int unused)
# define SET_TYPE(t)
#endif
{
    struct page *page;
    char *buf;
    loff_t off;
    uint32_t nread = PAGE_SIZE;
    int err;
    struct file *file = vma->vm_file;
    struct inode *inode = GET_F_DENTRY(file)->d_inode;
    struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
    struct sf_reg_info *sf_r = file->private_data;

    TRACE();
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 25)
    if (vmf->pgoff > vma->vm_end)
        return VM_FAULT_SIGBUS;
#else
    if (vaddr > vma->vm_end)
    {
        SET_TYPE(VM_FAULT_SIGBUS);
        return NOPAGE_SIGBUS;
    }
#endif

    /* Don't use GFP_HIGHUSER as long as sf_reg_read_aux() calls vboxCallRead()
     * which works on virtual addresses. On Linux cannot reliably determine the
     * physical address for high memory, see rtR0MemObjNativeLockKernel(). */
    page = alloc_page(GFP_USER);
    if (!page) {
        LogRelFunc(("failed to allocate page\n"));
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 25)
        return VM_FAULT_OOM;
#else
        SET_TYPE(VM_FAULT_OOM);
        return NOPAGE_OOM;
#endif
    }

    buf = kmap(page);
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 25)
    off = (vmf->pgoff << PAGE_SHIFT);
#else
    off = (vaddr - vma->vm_start) + (vma->vm_pgoff << PAGE_SHIFT);
#endif
    err = sf_reg_read_aux(__func__, sf_g, sf_r, buf, &nread, off);
    if (err)
    {
        kunmap(page);
        put_page(page);
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 25)
        return VM_FAULT_SIGBUS;
#else
        SET_TYPE(VM_FAULT_SIGBUS);
        return NOPAGE_SIGBUS;
#endif
    }

    BUG_ON (nread > PAGE_SIZE);
    if (!nread)
    {
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 25)
        clear_user_page(page_address(page), vmf->pgoff, page);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
        clear_user_page(page_address(page), vaddr, page);
#else
        clear_user_page(page_address(page), vaddr);
#endif
    }
    else
        memset(buf + nread, 0, PAGE_SIZE - nread);

    flush_dcache_page(page);
    kunmap(page);
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 25)
    vmf->page = page;
    return 0;
#else
    SET_TYPE(VM_FAULT_MAJOR);
    return page;
#endif
}

static struct vm_operations_struct sf_vma_ops =
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 25)
    .fault = sf_reg_fault
#else
     .nopage = sf_reg_nopage
#endif
};

static int sf_reg_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct dentry *dentry;
    int err;
    TRACE();
    if (vma->vm_flags & VM_SHARED)
    {
        LogFunc(("shared mmapping not available\n"));
        return -EINVAL;
    }

    vma->vm_ops = &sf_vma_ops;

    dentry = file->f_path.dentry;
    err = sf_inode_revalidate(dentry);
    if (err)
        return err;
    return  generic_file_mmap(file, vma);
}

struct file_operations sf_reg_fops =
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
    .read        = new_sync_read,
    .write       = new_sync_write,
    .read_iter   = sf_file_read,
    .write_iter  = sf_file_write,
#else
    .read        = sf_reg_read,
    .write       = sf_reg_write,
    .aio_read    = generic_file_aio_read,
    .aio_write   = generic_file_aio_write,
#endif
    .open        = sf_reg_open,
    .release     = sf_reg_release,
    .mmap        = sf_reg_mmap,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 23)
    .splice_read = sf_file_splice_read,
# else
    .sendfile    = generic_file_sendfile,
# endif
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
    .fsync       = noop_fsync,
# else
    .fsync       = simple_sync_file,
# endif
    .llseek      = sf_file_llseek,
#endif
};


struct inode_operations sf_reg_iops =
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
    .revalidate = sf_inode_revalidate
#else
    .getattr    = sf_getattr,
    .setattr    = sf_setattr
#endif
};


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
static int sf_readpage(struct file *file, struct page *page)
{
    struct inode *inode = GET_F_DENTRY(file)->d_inode;
    struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
    struct sf_reg_info *sf_r = file->private_data;
    uint32_t nread = PAGE_SIZE;
    char *buf;
    loff_t off = ((loff_t)page->index) << PAGE_SHIFT;
    int ret;

    TRACE();

    buf = kmap(page);
    ret = sf_reg_read_aux(__func__, sf_g, sf_r, buf, &nread, off);
    if (ret)
    {
        kunmap(page);
        if (PageLocked(page))
            unlock_page(page);
        return ret;
    }
    BUG_ON(nread > PAGE_SIZE);
    memset(&buf[nread], 0, PAGE_SIZE - nread);
    flush_dcache_page(page);
    kunmap(page);
    SetPageUptodate(page);
    unlock_page(page);
    return 0;
}

static int sf_readpages(struct file *file, struct address_space *mapping,
                        struct list_head *pages, unsigned nr_pages)
{
    RTCCPHYS tmp_phys;
    struct dentry *dentry = file->f_path.dentry;
    struct inode *inode = dentry->d_inode;
    struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
    struct sf_reg_info *sf_r  = file->private_data;
    void *physbuf;
    int bufsize;
    int bufsize2;
    size_t tmp_size;
    pgoff_t buf_startindex = 0;
    pgoff_t pages_in_buf = 0;
    int err = 0;


    /* first try to get everything in one read */
    bufsize2 = PAGE_SIZE * (list_entry(pages->next, struct page, lru)->index
                            - list_entry(pages->prev, struct page, lru)->index);
    bufsize = PAGE_SIZE * nr_pages;
    if (bufsize > 32 * PAGE_SIZE)
        bufsize = 32 * PAGE_SIZE;

    if (!bufsize)
        return 0;

    physbuf = alloc_bounce_buffer(&tmp_size, &tmp_phys, bufsize, __PRETTY_FUNCTION__);
    if (!physbuf)
        return -ENOMEM;


    while (!list_empty(pages))
    {
        struct page *page = list_entry((pages)->prev, struct page, lru);
        loff_t off = (loff_t) page->index << PAGE_SHIFT;
        list_del(&page->lru);
        if (add_to_page_cache_lru(page, mapping, page->index, GFP_KERNEL))
        {
            page_cache_release(page);
            continue;
        }

        /* read the next chunk if needed */
        if (page->index >= buf_startindex + pages_in_buf)
        {
            uint32_t nread = tmp_size;
            err = sf_reg_read_aux(__func__, sf_g, sf_r, physbuf, &nread, off);
            if (err || nread == 0)
                break;

            buf_startindex = page->index;
            pages_in_buf = nread >> PAGE_SHIFT;
            if (nread != PAGE_ALIGN(nread))
            {
                pages_in_buf++;
                memset(physbuf + nread, 0, (pages_in_buf << PAGE_SHIFT) - nread);
            }
        }
        copy_page(page_address(page),
                  physbuf + ((page->index - buf_startindex) << PAGE_SHIFT));

        flush_dcache_page(page);
        SetPageUptodate(page);
        unlock_page(page);
        page_cache_release(page);
    }
    free_bounce_buffer(physbuf);
    return err;
}


static struct sf_reg_info *
sf_gethandle(struct sf_inode_info *sf_i)
{
    struct list_head *cur;
    list_for_each(cur, &sf_i->regs) {
        struct sf_reg_info *sf_r_tmp = list_entry(cur, struct sf_reg_info, head);
        // write可能なやつは一つだけという前提
        if (sf_r_tmp->CreateFlags & SHFL_CF_ACCESS_WRITE) {
            return sf_r_tmp;
        }
    }
    return NULL;
}

static int
sf_writepage(struct page *page, struct writeback_control *wbc)
{
    struct address_space *mapping = page->mapping;
    struct inode *inode = mapping->host;
    struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
    struct sf_inode_info *sf_i = GET_INODE_INFO(inode);
    int err;
    struct sf_reg_info *sf_r = sf_gethandle(sf_i);
    if (!sf_r)
        return -ENOMEM;
    char *buf;
    uint32_t nwritten = PAGE_SIZE;
    int end_index = inode->i_size >> PAGE_SHIFT;
    loff_t off = ((loff_t) page->index) << PAGE_SHIFT;

    TRACE();

    if (page->index >= end_index)
        nwritten = inode->i_size & (PAGE_SIZE-1);

    buf = kmap(page);

    err = sf_reg_write_aux(__func__, sf_g, sf_r, buf, &nwritten, off);
    if (err < 0)
    {
        ClearPageUptodate(page);
        goto out;
    }

    if (off > inode->i_size)
        inode->i_size = off;

    if (PageError(page))
        ClearPageError(page);
    err = 0;

out:
    kunmap(page);

    unlock_page(page);
    return err;
}

#include <linux/pagevec.h>
static int
sf_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
    struct inode *inode = mapping->host;
    struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
    struct sf_inode_info *sf_i = GET_INODE_INFO(inode);

    int ret = 0;
    int done = 0;
    struct pagevec pvec;
    int nr_pages;
    pgoff_t uninitialized_var(writeback_index);
    pgoff_t index;
    pgoff_t end;		/* Inclusive */
    pgoff_t done_index;
    pgoff_t buf_startindex = 0;
    pgoff_t buf_previndex = 0;
    int cycled;
    int range_whole = 0;
    int tag;

    void *physbuf;
    RTCCPHYS tmp_phys;
    size_t tmp_size;
    loff_t off;
    int end_index = inode->i_size >> PAGE_SHIFT;
    int err;

    struct sf_reg_info *sf_r = sf_gethandle(sf_i);
    if (!sf_r)
        return -ENOMEM;

    int bufsize = PAGE_SIZE *  wbc->nr_to_write;
    if (bufsize > 32 * PAGE_SIZE)
        bufsize = 32 * PAGE_SIZE;
    if (!bufsize)
        return 0;

    // tmp_phys: virt_to_phys
    physbuf = alloc_bounce_buffer(&tmp_size, &tmp_phys, bufsize, __PRETTY_FUNCTION__);
    if (!physbuf)
        return -ENOMEM;


    pagevec_init(&pvec, 0);
    //if (wbc->range_cyclic) {
    //    writeback_index = mapping->writeback_index; /* prev offset */
    //    index = writeback_index;
    //    if (index == 0)
    //        cycled = 1;
    //    else
    //        cycled = 0;
    //    end = -1;
    //} else {
        index = wbc->range_start >> PAGE_CACHE_SHIFT;
        end = wbc->range_end >> PAGE_CACHE_SHIFT;
        if (wbc->range_start == 0 && wbc->range_end == LLONG_MAX)
            range_whole = 1;
        cycled = 1; /* ignore range_cyclic tests */
    //}
    if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages){
        tag = PAGECACHE_TAG_TOWRITE;
    }else{
        tag = PAGECACHE_TAG_DIRTY;
    }
retry:
    if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
        tag_pages_for_writeback(mapping, index, end);
    done_index = index;
    while (!done && (index <= end)) {
        int i;
        int to_write = 0;

        nr_pages = pagevec_lookup_tag(&pvec, mapping, &index, tag,
                  min(end - index, (pgoff_t)PAGEVEC_SIZE-1) + 1);

        if (nr_pages == 0)
            break;
        for (i = 0; i < nr_pages; i++) {
            struct page *page = pvec.pages[i];

            /*
             * At this point, the page may be truncated or
             * invalidated (changing page->mapping to NULL), or
             * even swizzled back from swapper_space to tmpfs file
             * mapping. However, page->index will not change
             * because we have a reference on the page.
             */
            if (page->index > end) {
                /*
                 * can't be range_cyclic (1st pass) because
                 * end == -1 in that case.
                 */
                done = 1;
                break;
            }
            done_index = page->index;
            lock_page(page);

            /*
             * Page truncated or invalidated. We can freely skip it
             * then, even for data integrity operations: the page
             * has disappeared concurrently, so there could be no
             * real expectation of this data interity operation
             * even if there is now a new, dirty page at the same
             * pagecache address.
             */
            if (unlikely(page->mapping != mapping)) {
continue_unlock:
                unlock_page(page);
                continue;
            }

            if (!PageDirty(page)) {
                /* someone wrote it for us */
                goto continue_unlock;
            }

            if (PageWriteback(page)) {
                if (wbc->sync_mode != WB_SYNC_NONE)
                    wait_on_page_writeback(page);
                else
                    goto continue_unlock;
            }

            BUG_ON(PageWriteback(page));
            if (!clear_page_dirty_for_io(page))
                goto continue_unlock;

            if (to_write != 0){
                if ( buf_previndex + 1 != page->index || to_write + PAGE_SIZE > tmp_size){
                   // try to write
                    off = (loff_t) buf_startindex << PAGE_SHIFT;
                    err = sf_reg_write_aux(__func__, sf_g, sf_r, physbuf, &to_write, off);
                    if (err < 0)
                    {
                        ret = err;
                        break;
                    }
                    buf_startindex = page->index;
                    to_write = 0;
                }
            }else{
                buf_startindex = page->index;
            }

            // copy to buffer
            copy_page(physbuf + ((page->index - buf_startindex) << PAGE_SHIFT),
                        page_address(page));
            if (page->index >= end_index)
                to_write += inode->i_size & (PAGE_SIZE-1);
            else
                to_write += PAGE_SIZE;

            buf_previndex = page->index;
            loff_t tmp_off = ((loff_t) page->index) << PAGE_SHIFT;
            if (tmp_off > inode->i_size)
                inode->i_size = tmp_off;
            unlock_page(page);

            if (unlikely(ret)) {
                if (ret == AOP_WRITEPAGE_ACTIVATE) {
                    unlock_page(page);
                    ret = 0;
                } else {
                    /*
                     * done_index is set past this page,
                     * so media errors will not choke
                     * background writeout for the entire
                     * file. This has consequences for
                     * range_cyclic semantics (ie. it may
                     * not be suitable for data integrity
                     * writeout).
                     */
                    done_index = page->index + 1;
                    done = 1;
                    break;
                }
            }

            /*
             * We stop writing back only if we are not doing
             * integrity sync. In case of integrity sync we have to
             * keep going until we have written all the pages
             * we tagged for writeback prior to entering this loop.
             */
            if (--wbc->nr_to_write <= 0 &&
                wbc->sync_mode == WB_SYNC_NONE) {
                done = 1;
                break;
            }
        }

        // try to write last data
        if (to_write != 0){
            off = ((loff_t) buf_startindex) << PAGE_SHIFT;
            err = sf_reg_write_aux(__func__, sf_g, sf_r, physbuf, &to_write, off);
            if (err < 0)
                ret = err;
        }

        pagevec_release(&pvec);
        cond_resched();
    }
    //if (!cycled && !done) {
    //    /*
    //     * range_cyclic:
    //     * We hit the last page and there is more work to be done: wrap
    //     * back to the start of the file
    //     */
    //    cycled = 1;
    //    index = 0;
    //    end = writeback_index - 1;
    //    goto retry;
    //}
    //if (wbc->range_cyclic || (range_whole && wbc->nr_to_write > 0))
    //    mapping->writeback_index = done_index;

    free_bounce_buffer(physbuf);
    return ret;
}
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24)

/*
 * Determine the number of bytes of data the page contains
 */
static inline
unsigned int sf_page_length(struct page *page)
{
    //loff_t i_size = i_size_read(page_file_mapping(page)->host);
    loff_t i_size = page->mapping->host->i_size;

    if (i_size > 0) {
        pgoff_t page_index = page_file_index(page);
        pgoff_t end_index = (i_size - 1) >> PAGE_CACHE_SHIFT;
        if (page_index < end_index)
            return PAGE_CACHE_SIZE;
        if (page_index == end_index)
            return ((i_size - 1) & ~PAGE_CACHE_MASK) + 1;
    }
    return 0;
}

static int sf_want_read_modify_write(struct file *file, struct page *page,
            loff_t pos, unsigned len)
{
    unsigned int pglen = sf_page_length(page);
    unsigned int offset = pos & (PAGE_CACHE_SIZE - 1);
    unsigned int end = offset + len;

    if ((file->f_mode & FMODE_READ) &&  /* open for read? */
        !PageUptodate(page) &&          /* Uptodate? */
        !PageDirty(page) &&             /* page dirty */
        pglen &&                        /* valid bytes of file? */
        (end < pglen || offset))        /* replace all valid bytes? */
        return 1;
    return 0;
}

int sf_write_begin(struct file *file, struct address_space *mapping, loff_t pos,
                   unsigned len, unsigned flags, struct page **pagep, void **fsdata)
{
    int ret;
    pgoff_t index = pos >> PAGE_CACHE_SHIFT;
    struct page *page;
    int once_thru = 0;

start:
    page = grab_cache_page_write_begin(mapping, index, flags);
    if (!page)
        return -ENOMEM;
    *pagep = page;

    if (!once_thru && sf_want_read_modify_write(file, page, pos, len)) {
        once_thru = 1;
        ret = sf_readpage(file, page);
        page_cache_release(page);
        if (!ret)
            goto start;
    }
    return ret;
}

void update_file_size(struct page *page, unsigned to)
{
    struct inode *inode = page_file_mapping(page)->host; 
    loff_t end, i_size;
    pgoff_t end_index;

    spin_lock(&inode->i_lock);

    i_size = i_size_read(inode);
    end_index = (i_size - 1) >> PAGE_CACHE_SHIFT;
    if (i_size == 0 || page_file_index(page) >= end_index){
        end = page_file_offset(page) + to;
        if (i_size < end)
            i_size_write(inode, end);
    }
    spin_unlock(&inode->i_lock);
}

int sf_write_end(struct file *file, struct address_space *mapping, loff_t pos,
                 unsigned len, unsigned copied, struct page *page, void *fsdata)
{
    struct inode *inode = mapping->host;
    struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
    struct sf_reg_info *sf_r = file->private_data;
    void *buf;
    unsigned from = pos & (PAGE_SIZE - 1);
    unsigned to = from + len;
    uint32_t nwritten = len;
    struct timespec ct = CURRENT_TIME;
    int err = 0;

    TRACE();

    /* buf = kmap(page); */
    /* err = sf_reg_write_aux(__func__, sf_g, sf_r, buf+from, &nwritten, pos); */
    /* kunmap(page); */


    if (!PageUptodate(page)) {
        unsigned pglen = sf_page_length(page);

        if (pglen == 0) {
            zero_user_segments(page, 0, from, to, PAGE_CACHE_SIZE);
            SetPageUptodate(page);
        } else if (to >= pglen) {
            zero_user_segment(page, to, PAGE_CACHE_SIZE);
            if (from == 0)
                SetPageUptodate(page);
        } else
            zero_user_segment(page, pglen, PAGE_CACHE_SIZE);
    }

    if (!PageUptodate(page)){
        buf = kmap(page);
        err = sf_reg_write_aux(__func__, sf_g, sf_r, buf+from, &nwritten, pos);
        kunmap(page);
    }else{
        __set_page_dirty_nobuffers(page);
    }
    if (err >= 0) {
        update_file_size(page, to);
    }

    unlock_page(page);
    page_cache_release(page);

    return nwritten;
}

# endif /* KERNEL_VERSION >= 2.6.24 */

struct address_space_operations sf_reg_aops =
{
    .readpage      = sf_readpage,
    .readpages     = sf_readpages,
    .writepage     = sf_writepage,
    .writepages    = sf_writepages,
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24)
    .write_begin   = sf_write_begin,
    .write_end     = sf_write_end,
# else
    .prepare_write = simple_prepare_write,
    .commit_write  = simple_commit_write,
# endif
};
#endif
