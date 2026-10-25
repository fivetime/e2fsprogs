/*
 * resize_journal.h --- journal the metadata of an online resize
 *
 * This file may be redistributed under the terms of the GNU Public License.
 */
#ifndef _FUSE4FS_RESIZE_JOURNAL_H
#define _FUSE4FS_RESIZE_JOURNAL_H

#include "ext2fs/ext2fs.h"

struct fuse4fs_journal;

/* Length in blocks and emptiness of the internal journal, read from disk. */
errcode_t fuse4fs_journal_probe(ext2_filsys fs, blk64_t *maxlen, int *empty);

/*
 * Open the (empty) journal for a transaction of nr blocks: EBUSY if it is
 * not empty, ENOSPC if the transaction would not fit.  A metadata_csum file
 * system gets a v3 checksummed journal here, as it does from the kernel.
 */
errcode_t fuse4fs_journal_begin(ext2_filsys fs, size_t nr,
				struct fuse4fs_journal **jh);

/*
 * Write blocks[i] with contents data[i] as one transaction, commit it and
 * point the journal superblock at it.  Once this returns 0 the next mount
 * replays the transaction.
 */
errcode_t fuse4fs_journal_commit(struct fuse4fs_journal *jh, blk64_t *blocks,
				 const char * const *data, size_t nr);

/* The blocks are written home: mark the journal empty and close it. */
errcode_t fuse4fs_journal_finish(struct fuse4fs_journal *jh);

/* Close without committing; the journal stays empty. */
void fuse4fs_journal_abort(struct fuse4fs_journal *jh);

#endif /* _FUSE4FS_RESIZE_JOURNAL_H */
