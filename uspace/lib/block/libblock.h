/*
 * Copyright (c) 2008 Jakub Jermar
 * Copyright (c) 2008 Martin Decky 
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup libblock 
 * @{
 */ 
/**
 * @file
 */

#ifndef LIBBLOCK_LIBBLOCK_H_
#define LIBBLOCK_LIBBLOCK_H_

#include <stdint.h>
#include "../../srv/vfs/vfs.h"
#include <fibril_synch.h>
#include <adt/hash_table.h>
#include <adt/list.h>

/*
 * Flags that can be used with block_get().
 */

/** 
 * This macro is a symbolic value for situations where no special flags are
 * needed.
 */
#define BLOCK_FLAGS_NONE	0

/**
 * When the client of block_get() intends to overwrite the current contents of
 * the block, this flag is used to avoid the unnecessary read.
 */
#define BLOCK_FLAGS_NOREAD	1

typedef struct block {
	/** Mutex protecting the reference count. */
	fibril_mutex_t lock;
	/** Number of references to the block_t structure. */
	unsigned refcnt;
	/** If true, the block needs to be written back to the block device. */
	bool dirty;
	/** If true, the blcok does not contain valid data. */
	bool toxic;
	/** Readers / Writer lock protecting the contents of the block. */
	fibril_rwlock_t contents_lock;
	/** Handle of the device where the block resides. */
	devmap_handle_t devmap_handle;
	/** Logical block address */
	aoff64_t lba;
	/** Physical block address */
	aoff64_t pba;
	/** Size of the block. */
	size_t size;
	/** Link for placing the block into the free block list. */
	link_t free_link;
	/** Link for placing the block into the block hash table. */ 
	link_t hash_link;
	/** Buffer with the block data. */
	void *data;
} block_t;

/** Caching mode */
enum cache_mode {
	/** Write-Through */
	CACHE_MODE_WT,
	/** Write-Back */
	CACHE_MODE_WB
};

extern int block_init(devmap_handle_t, size_t);
extern void block_fini(devmap_handle_t);

extern int block_bb_read(devmap_handle_t, aoff64_t);
extern void *block_bb_get(devmap_handle_t);

extern int block_cache_init(devmap_handle_t, size_t, unsigned, enum cache_mode);
extern int block_cache_fini(devmap_handle_t);

extern int block_get(block_t **, devmap_handle_t, aoff64_t, int);
extern int block_put(block_t *);

extern int block_seqread(devmap_handle_t, size_t *, size_t *, aoff64_t *, void *,
    size_t);

extern int block_get_bsize(devmap_handle_t, size_t *);
extern int block_get_nblocks(devmap_handle_t, aoff64_t *);
extern int block_read_direct(devmap_handle_t, aoff64_t, size_t, void *);
extern int block_write_direct(devmap_handle_t, aoff64_t, size_t, const void *);

#endif

/** @}
 */

