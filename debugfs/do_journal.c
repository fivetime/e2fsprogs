/*
 * do_journal.c --- Scribble onto the journal!
 *
 * Copyright (C) 2014 Oracle.  This file may be redistributed
 * under the terms of the GNU Public License.
 */

#include "config.h"
#include <stdio.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
extern int optind;
extern char *optarg;
#endif
#include <ctype.h>
#include <unistd.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "debugfs.h"
#include "ext2fs/kernel-jbd.h"
#include "journal.h"
#include "journal_write.h"

#undef DEBUG

#ifdef DEBUG
# define dbg_printf(f, a...)  do {printf("JFS DEBUG: " f, ## a); \
	fflush(stdout); \
} while (0)
#else
# define dbg_printf(f, a...)
#endif

static journal_t *current_journal = NULL;

/* Feed journal_write the blocks of a transaction from a file. */
struct file_source {
	FILE *fp;
	unsigned int blocksize;
};

static errcode_t read_block_from_file(void *priv,
				      size_t idx EXT2FS_ATTR((unused)),
				      blk64_t blk EXT2FS_ATTR((unused)),
				      void *buf)
{
	struct file_source *src = priv;

	if (fread(buf, src->blocksize, 1, src->fp) != 1)
		return errno ? errno : EXT2_ET_SHORT_READ;
	return 0;
}

void do_journal_write(int argc, ss_argv_t argv, int sci_idx EXT2FS_ATTR((unused)),
		      void *infop EXT2FS_ATTR((unused)))
{
	blk64_t *blist = NULL, *rlist = NULL;
	size_t bn = 0, rn = 0;
	FILE *fp = NULL;
	struct file_source src;
	int opt;
	int flags = 0;
	errcode_t err;

	if (current_journal == NULL) {
		printf("Journal not open.\n");
		return;
	}

	reset_getopt();
	while ((opt = getopt(argc, argv, "b:r:c")) != -1) {
		switch (opt) {
		case 'b':
			err = read_list(optarg, &blist, &bn);
			if (err) {
				com_err(argv[0], err,
					"while reading block list");
				goto out;
			}
			break;
		case 'r':
			err = read_list(optarg, &rlist, &rn);
			if (err) {
				com_err(argv[0], err,
					"while reading revoke list");
				goto out;
			}
			break;
		case 'c':
			flags |= EXT2FS_JOURNAL_WRITE_NO_COMMIT;
			break;
		default:
			printf("%s [-b blocks] [-r revoke] [-c] file\n",
			       argv[0]);
			printf("-b: Write these blocks into transaction.\n");
			printf("-c: Do not commit transaction.\n");
			printf("-r: Revoke these blocks from transaction.\n");

			goto out;
		}
	}

	if (bn > 0 && optind != argc - 1) {
		printf("Need a file to read blocks from.\n");
		return;
	}

	if (bn > 0) {
		fp = fopen(argv[optind], "r");
		if (fp == NULL) {
			com_err(argv[0], errno,
				"while opening journal data file");
			goto out;
		}
	}

	src.fp = fp;
	src.blocksize = current_journal->j_blocksize;
	err = ext2fs_journal_write(current_journal, flags, blist, bn,
				   read_block_from_file, &src, rlist, rn);
	if (err)
		com_err("journal_write", err, "while writing journal");

	if (fp)
		fclose(fp);
out:
	if (blist)
		free(blist);
	if (rlist)
		free(rlist);
}

void do_journal_open(int argc, ss_argv_t argv, int sci_idx EXT2FS_ATTR((unused)),
		     void *infop EXT2FS_ATTR((unused)))
{
	int opt, enable_csum = 0, csum_ver = 3;
	journal_t *journal;
	errcode_t err;

	if (check_fs_open(argv[0]))
		return;
	if (check_fs_read_write(argv[0]))
		return;
	if (check_fs_bitmaps(argv[0]))
		return;
	if (current_journal) {
		printf("Journal is already open.\n");
		return;
	}
	if (!ext2fs_has_feature_journal(current_fs->super)) {
		printf("Journalling is not enabled on this filesystem.\n");
		return;
	}

	reset_getopt();
	while ((opt = getopt(argc, argv, "cv:f:")) != -1) {
		switch (opt) {
		case 'c':
			enable_csum = 1;
			break;
		case 'f':
			if (current_fs->journal_name)
				free(current_fs->journal_name);
			current_fs->journal_name = strdup(optarg);
			break;
		case 'v':
			csum_ver = atoi(optarg);
			if (csum_ver != 2 && csum_ver != 3) {
				printf("Unknown journal csum v%d\n", csum_ver);
				csum_ver = 3;
			}
			break;
		default:
			printf("%s: [-c] [-v ver] [-f ext_jnl]\n", argv[0]);
			printf("-c: Enable journal checksumming.\n");
			printf("-v: Use this version checksum format.\n");
			printf("-f: Load this external journal.\n");
		}
	}

	err = ext2fs_open_journal(current_fs, &current_journal);
	if (err) {
		com_err(argv[0], err, "while opening journal");
		return;
	}
	journal = current_journal;

	dbg_printf("JOURNAL: seq=%u tailseq=%u start=%lu first=%lu "
		   "maxlen=%lu\n", journal->j_tail_sequence,
		   journal->j_transaction_sequence, journal->j_tail,
		   journal->j_first, journal->j_last);

	if (ext2fs_journal_set_uuid(journal) == EBUSY)
		printf("Journal needs recovery, will not set 64bit.\n");
	if (ext2fs_journal_set_64bit(journal) == EBUSY)
		printf("Journal needs recovery, will not set 64bit.\n");
	if (enable_csum) {
		err = ext2fs_journal_set_csum(journal, csum_ver);
		if (err == EBUSY)
			printf("Journal needs recovery, will not add csums.\n");
		else if (!err && journal->j_format_version >= 2 &&
			 ext2fs_has_feature_metadata_csum(current_fs->super))
			printf("Setting csum v%d\n", csum_ver);
	}

	err = ext2fs_journal_find_head(journal);
	if (err)
		com_err(argv[0], err, "while examining journal");
}

void do_journal_close(int argc EXT2FS_ATTR((unused)),
		      ss_argv_t argv EXT2FS_ATTR((unused)),
		      int sci_idx EXT2FS_ATTR((unused)),
		      void *infop EXT2FS_ATTR((unused)))
{
	if (current_journal == NULL) {
		printf("Journal not open.\n");
		return;
	}

	ext2fs_close_journal(current_fs, &current_journal);
}

void do_journal_run(int argc EXT2FS_ATTR((unused)), ss_argv_t argv,
		    int sci_idx EXT2FS_ATTR((unused)),
		    void *infop EXT2FS_ATTR((unused)))
{
	errcode_t err;

	if (check_fs_open(argv[0]))
		return;
	if (check_fs_read_write(argv[0]))
		return;
	if (check_fs_bitmaps(argv[0]))
		return;
	if (current_journal) {
		printf("Please close the journal before recovering it.\n");
		return;
	}

	err = ext2fs_run_ext3_journal(&current_fs);
	if (err)
		com_err("journal_run", err, "while recovering journal");
	else {
		ext2fs_clear_feature_journal_needs_recovery(current_fs->super);
		ext2fs_mark_super_dirty(current_fs);
	}
}
