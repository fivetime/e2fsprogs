/*
 * journal_write.h --- write transactions into an ext4 journal
 *
 * This file may be redistributed under the terms of the GNU Public License.
 */
#ifndef _JOURNAL_WRITE_H
#define _JOURNAL_WRITE_H

#include "journal.h"

struct journal_transaction_s {
	unsigned int magic;
	ext2_filsys fs;
	journal_t *journal;
	blk64_t block;
	blk64_t start, end;
	tid_t tid;
	int flags;
};

typedef struct journal_transaction_s journal_transaction_t;

/*
 * Supplies the contents of the idx-th block of a transaction's block list,
 * which is file system block blk, in buf (one file system block).
 */
typedef errcode_t (*ext2fs_journal_block_reader)(void *priv, size_t idx,
						 blk64_t blk, void *buf);

#define EXT2FS_JOURNAL_WRITE_NO_COMMIT	1

/* Position the head of an open journal after its last transaction. */
errcode_t ext2fs_journal_find_head(journal_t *journal);

/* Feature changes; each returns EBUSY if the journal is not empty. */
errcode_t ext2fs_journal_set_csum(journal_t *journal, int ver);
errcode_t ext2fs_journal_set_uuid(journal_t *journal);
errcode_t ext2fs_journal_set_64bit(journal_t *journal);

/* Journal blocks a transaction with this many data and revoke blocks needs. */
blk64_t ext2fs_journal_trans_blocks(journal_t *journal, blk64_t data_blocks,
				    blk64_t revoke_blocks);

errcode_t ext2fs_journal_open_trans(journal_t *journal,
				    journal_transaction_t *trans,
				    blk64_t blocks);
errcode_t ext2fs_journal_add_blocks_to_trans(journal_transaction_t *trans,
					    blk64_t *block_list,
					    size_t block_len,
					    ext2fs_journal_block_reader read_block,
					    void *priv);
errcode_t ext2fs_journal_add_revoke_to_trans(journal_transaction_t *trans,
					    blk64_t *revoke_list,
					    size_t revoke_len);
errcode_t ext2fs_journal_commit_trans(journal_transaction_t *trans);
errcode_t ext2fs_journal_close_trans(journal_transaction_t *trans);

/* Open, fill, commit (unless EXT2FS_JOURNAL_WRITE_NO_COMMIT) and close. */
errcode_t ext2fs_journal_write(journal_t *journal, int flags,
			       blk64_t *block_list, size_t block_len,
			       ext2fs_journal_block_reader read_block,
			       void *priv, blk64_t *revoke_list,
			       size_t revoke_len);

#endif /* _JOURNAL_WRITE_H */
