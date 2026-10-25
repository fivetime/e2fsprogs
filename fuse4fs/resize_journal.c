/*
 * resize_journal.c --- journal the metadata of an online resize
 *
 * fuse4fs does not use the journal while it runs, so an online resize can
 * have it to itself: the metadata blocks inside the old file system that
 * the resize changes are written as one committed transaction, then written
 * home, then the journal is marked empty again.  A crash before the commit
 * leaves the old file system, a crash after it leaves a transaction that
 * the next mount replays.  This file is compiled with the debugfs journal
 * code (-DDEBUGFS), which fuse4fs.c itself does not include.
 *
 * This file may be redistributed under the terms of the GNU Public License.
 */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "ext2fs/ext2fs.h"
#include "ext2fs/kernel-jbd.h"
#include "../debugfs/journal.h"
#include "../debugfs/journal_write.h"
#include "resize_journal.h"

struct fuse4fs_journal {
	ext2_filsys fs;
	journal_t *journal;
	int committed;
};

errcode_t fuse4fs_journal_probe(ext2_filsys fs, blk64_t *maxlen, int *empty)
{
	struct ext2_inode inode;
	journal_superblock_t *jsb;
	char *buf = NULL;
	blk64_t pblk;
	__u32 blocktype;
	errcode_t err;

	if (!ext2fs_has_feature_journal(fs->super) ||
	    ext2fs_has_feature_journal_dev(fs->super) ||
	    !fs->super->s_journal_inum)
		return EXT2_ET_NO_JOURNAL;

	err = ext2fs_read_inode(fs, fs->super->s_journal_inum, &inode);
	if (err)
		return err;

	err = ext2fs_bmap2(fs, fs->super->s_journal_inum, &inode, NULL, 0, 0,
			   NULL, &pblk);
	if (err)
		return err;

	err = ext2fs_get_mem(fs->blocksize, &buf);
	if (err)
		return err;

	err = io_channel_read_blk64(fs->io, pblk, 1, buf);
	if (err)
		goto out;

	jsb = (journal_superblock_t *)buf;
	blocktype = ext2fs_be32_to_cpu(jsb->s_header.h_blocktype);
	if (jsb->s_header.h_magic != ext2fs_cpu_to_be32(JBD2_MAGIC_NUMBER) ||
	    (blocktype != JBD2_SUPERBLOCK_V1 &&
	     blocktype != JBD2_SUPERBLOCK_V2)) {
		err = EXT2_ET_CORRUPT_JOURNAL_SB;
		goto out;
	}

	*maxlen = ext2fs_be32_to_cpu(jsb->s_maxlen);
	*empty = (jsb->s_start == 0);
out:
	ext2fs_free_mem(&buf);
	return err;
}

/* Write the journal superblock out and make sure it is on disk. */
static errcode_t write_journal_sb(journal_t *journal)
{
	journal_superblock_t *jsb = journal->j_superblock;
	errcode_t err;

	jsb->s_sequence = ext2fs_cpu_to_be32(journal->j_tail_sequence);
	err = ext2fs_journal_sb_csum_set(journal, jsb);
	if (err)
		return err;

	mark_buffer_dirty(journal->j_sb_buffer);
	ll_rw_block(REQ_OP_WRITE, 0, 1, &journal->j_sb_buffer);
	if (journal->j_sb_buffer->b_err)
		return journal->j_sb_buffer->b_err;

	return io_channel_flush(journal->j_fs_dev->k_fs->io);
}

errcode_t fuse4fs_journal_begin(ext2_filsys fs, size_t nr,
				struct fuse4fs_journal **jhp)
{
	struct fuse4fs_journal *jh;
	journal_t *journal;
	blk64_t needed;
	errcode_t err;

	err = ext2fs_get_memzero(sizeof(*jh), &jh);
	if (err)
		return err;

	err = ext2fs_open_journal(fs, &journal);
	if (err) {
		ext2fs_free_mem(&jh);
		return err;
	}

	jh->fs = fs;
	jh->journal = journal;

	if (journal->j_tail != 0 ||
	    ext2fs_has_feature_journal_needs_recovery(fs->super)) {
		err = EBUSY;
		goto fail;
	}

	err = ext2fs_journal_find_head(journal);
	if (err)
		goto fail;

	/*
	 * The kernel checksums the journal of a metadata_csum file system
	 * when it mounts it; a journal that was never mounted by the kernel
	 * has no checksums yet.  Switching is a journal superblock write of
	 * its own, before anything else changes.
	 */
	if (ext2fs_has_feature_metadata_csum(fs->super) &&
	    !jbd2_journal_has_csum_v2or3(journal)) {
		err = ext2fs_journal_set_csum(journal, 3);
		if (err)
			goto fail;
		err = write_journal_sb(journal);
		if (err)
			goto fail;
	}

	/* The descriptor and data blocks plus the commit block. */
	needed = ext2fs_journal_trans_blocks(journal, nr, 0);
	if (journal->j_first + needed > journal->j_last) {
		err = ENOSPC;
		goto fail;
	}

	*jhp = jh;
	return 0;
fail:
	fuse4fs_journal_abort(jh);
	return err;
}

struct block_source {
	const char * const *data;
	unsigned int blocksize;
};

/* Test hook, as in fuse4fs.c: die here if FUSE4FS_RESIZE_FAULT names it. */
static void fault(const char *point)
{
	const char *want = getenv("FUSE4FS_RESIZE_FAULT");

	if (want && strcmp(want, point) == 0) {
		fprintf(stderr, "FUSE4FS: resize fault injected at %s\n", point);
		fflush(stderr);
		_exit(1);
	}
}

static errcode_t read_block_from_memory(void *priv, size_t idx,
					blk64_t blk EXT2FS_ATTR((unused)),
					void *buf)
{
	struct block_source *src = priv;

	memcpy(buf, src->data[idx], src->blocksize);
	return 0;
}

errcode_t fuse4fs_journal_commit(struct fuse4fs_journal *jh, blk64_t *blocks,
				 const char * const *data, size_t nr)
{
	struct block_source src;
	journal_transaction_t trans;
	errcode_t err;

	src.data = data;
	src.blocksize = jh->journal->j_blocksize;
	err = ext2fs_journal_open_trans(jh->journal, &trans,
					ext2fs_journal_trans_blocks(jh->journal,
								    nr, 0));
	if (err)
		return err;

	err = ext2fs_journal_add_blocks_to_trans(&trans, blocks, nr,
						 read_block_from_memory, &src);
	if (err)
		return err;

	/* Descriptor and data blocks written, no commit block yet */
	fault("mid-commit");

	err = ext2fs_journal_commit_trans(&trans);
	if (err)
		return err;

	err = ext2fs_journal_close_trans(&trans);
	if (err)
		return err;

	/*
	 * The transaction's blocks and commit block are in the journal, but
	 * recovery only looks where the journal superblock points.  This
	 * write is the commit point.
	 */
	err = write_journal_sb(jh->journal);
	if (err)
		return err;

	jh->committed = 1;
	return 0;
}

errcode_t fuse4fs_journal_finish(struct fuse4fs_journal *jh)
{
	journal_t *journal = jh->journal;
	errcode_t err;

	/* An empty journal: no start, and the next transaction id. */
	journal->j_tail = 0;
	journal->j_tail_sequence = journal->j_transaction_sequence;
	journal->j_superblock->s_start = 0;
	err = write_journal_sb(journal);

	ext2fs_close_journal(jh->fs, &jh->journal);
	ext2fs_free_mem(&jh);
	return err;
}

void fuse4fs_journal_abort(struct fuse4fs_journal *jh)
{
	journal_t *journal = jh->journal;

	if (journal) {
		/*
		 * Nothing was committed, so closing must not point the
		 * journal superblock at whatever was written.
		 */
		if (!jh->committed) {
			journal->j_tail = 0;
			journal->j_superblock->s_start = 0;
		}
		ext2fs_close_journal(jh->fs, &jh->journal);
	}
	ext2fs_free_mem(&jh);
}
