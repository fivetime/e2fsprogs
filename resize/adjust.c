/*
 * adjust.c --- lay out a resized file system in memory
 *
 * adjust_fs_info() and its helpers, split out of resize2fs.c so that
 * fuse4fs can grow a mounted file system with the same code.
 *
 * Copyright (C) 1997, 1998 by Theodore Ts'o and
 * 	PowerQuest, Inc.
 *
 * Copyright (C) 1999, 2000 by Theodore Ts'o
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#include "config.h"
#include "resize2fs.h"
#include <time.h>

#define B2C(x)	EXT2FS_B2C(fs, (x))
#define C2B(x)	EXT2FS_C2B(fs, (x))

/* Leave the inode tables of new block groups to the kernel's lazy init */
int lazy_itable_init;

/* Keep the size of the group descriptor region constant */
void adjust_reserved_gdt_blocks(ext2_filsys old_fs, ext2_filsys fs)
{
	if (ext2fs_has_feature_resize_inode(fs->super) &&
	    (old_fs->desc_blocks != fs->desc_blocks)) {
		int new;

		new = ((int) fs->super->s_reserved_gdt_blocks) +
			(old_fs->desc_blocks - fs->desc_blocks);
		if (new < 0)
			new = 0;
		if (new > (int) fs->blocksize/4)
			new = fs->blocksize/4;
		fs->super->s_reserved_gdt_blocks = new;
	}
}

/*
 * If the group descriptor's bitmap and inode table blocks are valid,
 * release them in the new filesystem data structure, and mark them as
 * reserved so the old inode table blocks don't get overwritten.
 */
static errcode_t free_gdp_blocks(ext2_filsys fs,
				 ext2fs_block_bitmap reserve_blocks,
				 ext2_filsys old_fs,
				 dgrp_t group)
{
	blk64_t		blk;
	unsigned int	j;
	dgrp_t		i;
	ext2fs_block_bitmap bg_map = NULL;
	errcode_t	retval = 0;
	dgrp_t		count = old_fs->group_desc_count - fs->group_desc_count;

	/* If bigalloc, don't free metadata living in the same cluster */
	if (EXT2FS_CLUSTER_RATIO(fs) > 1) {
		retval = ext2fs_allocate_block_bitmap(fs, "bgdata", &bg_map);
		if (retval)
			goto out;

		retval = mark_table_blocks(fs, bg_map);
		if (retval)
			goto out;
	}

	for (i = group; i < group + count; i++) {
		blk = ext2fs_block_bitmap_loc(old_fs, i);
		if (blk &&
		    (blk < ext2fs_blocks_count(fs->super)) &&
		    !(bg_map && ext2fs_test_block_bitmap2(bg_map, blk))) {
			ext2fs_block_alloc_stats2(fs, blk, -1);
			ext2fs_mark_block_bitmap2(reserve_blocks, blk);
		}

		blk = ext2fs_inode_bitmap_loc(old_fs, i);
		if (blk &&
		    (blk < ext2fs_blocks_count(fs->super)) &&
		    !(bg_map && ext2fs_test_block_bitmap2(bg_map, blk))) {
			ext2fs_block_alloc_stats2(fs, blk, -1);
			ext2fs_mark_block_bitmap2(reserve_blocks, blk);
		}

		blk = ext2fs_inode_table_loc(old_fs, i);
		for (j = 0;
		     j < fs->inode_blocks_per_group; j++, blk++) {
			if (blk >= ext2fs_blocks_count(fs->super) ||
			    (bg_map && ext2fs_test_block_bitmap2(bg_map, blk)))
				continue;
			ext2fs_block_alloc_stats2(fs, blk, -1);
			ext2fs_mark_block_bitmap2(reserve_blocks, blk);
		}
	}

out:
	if (bg_map)
		ext2fs_free_block_bitmap(bg_map);
	return retval;
}

/*
 * This helper function creates a block bitmap with all of the
 * filesystem meta-data blocks.
 */
errcode_t mark_table_blocks(ext2_filsys fs,
				   ext2fs_block_bitmap bmap)
{
	dgrp_t			i;
	blk64_t			blk;

	for (i = 0; i < fs->group_desc_count; i++) {
		ext2fs_reserve_super_and_bgd(fs, i, bmap);

		/*
		 * Mark the blocks used for the inode table
		 */
		blk = ext2fs_inode_table_loc(fs, i);
		if (blk)
			ext2fs_mark_block_bitmap_range2(bmap, blk,
						fs->inode_blocks_per_group);

		/*
		 * Mark block used for the block bitmap
		 */
		blk = ext2fs_block_bitmap_loc(fs, i);
		if (blk)
			ext2fs_mark_block_bitmap2(bmap, blk);

		/*
		 * Mark block used for the inode bitmap
		 */
		blk = ext2fs_inode_bitmap_loc(fs, i);
		if (blk)
			ext2fs_mark_block_bitmap2(bmap, blk);
	}
	/* Reserve the MMP block */
	if (ext2fs_has_feature_mmp(fs->super) &&
	    fs->super->s_mmp_block > fs->super->s_first_data_block &&
	    fs->super->s_mmp_block < ext2fs_blocks_count(fs->super))
		ext2fs_mark_block_bitmap2(bmap, fs->super->s_mmp_block);
	return 0;
}

/*
 * This routine is shared by the online and offline resize routines.
 * All of the information which is adjusted in memory is done here.
 */
errcode_t adjust_fs_info(ext2_filsys fs, ext2_filsys old_fs,
			 ext2fs_block_bitmap reserve_blocks, blk64_t new_size)
{
	errcode_t	retval;
	blk64_t		overhead = 0;
	blk64_t		rem;
	blk64_t		blk, group_block;
	blk64_t		real_end;
	blk64_t		old_numblocks, numblocks, adjblocks;
	unsigned long	i, j, old_desc_blocks;
	unsigned int	meta_bg, meta_bg_size;
	int		has_super, csum_flag, has_bg;
	unsigned long long new_inodes;	/* u64 to check for overflow */
	double		percent;

	ext2fs_blocks_count_set(fs->super, new_size);
	fs->super->s_overhead_clusters = 0;

retry:
	fs->group_desc_count = ext2fs_div64_ceil(ext2fs_blocks_count(fs->super) -
				       fs->super->s_first_data_block,
				       EXT2_BLOCKS_PER_GROUP(fs->super));
	if (fs->group_desc_count == 0)
		return EXT2_ET_TOOSMALL;
	fs->desc_blocks = ext2fs_div_ceil(fs->group_desc_count,
					  EXT2_DESC_PER_BLOCK(fs->super));

	/*
	 * Overhead is the number of bookkeeping blocks per group.  It
	 * includes the superblock backup, the group descriptor
	 * backups, the inode bitmap, the block bitmap, and the inode
	 * table.
	 */
	overhead = (int) (2 + fs->inode_blocks_per_group);

	has_bg = 0;
	if (ext2fs_has_feature_sparse_super2(fs->super)) {
		/*
		 * We have to do this manually since
		 * super->s_backup_bgs hasn't been set up yet.
		 */
		if (fs->group_desc_count == 2)
			has_bg = fs->super->s_backup_bgs[0] != 0;
		else
			has_bg = fs->super->s_backup_bgs[1] != 0;
	} else
		has_bg = ext2fs_bg_has_super(fs, fs->group_desc_count - 1);
	if (has_bg)
		overhead += 1 + fs->desc_blocks +
			fs->super->s_reserved_gdt_blocks;

	/*
	 * See if the last group is big enough to support the
	 * necessary data structures.  If not, we need to get rid of
	 * it.
	 */
	rem = (ext2fs_blocks_count(fs->super) - fs->super->s_first_data_block) %
		fs->super->s_blocks_per_group;
	if ((fs->group_desc_count == 1) && rem && (rem < overhead))
		return EXT2_ET_TOOSMALL;
	if ((fs->group_desc_count > 1) && rem && (rem < overhead+50)) {
		ext2fs_blocks_count_set(fs->super,
					ext2fs_blocks_count(fs->super) - rem);
		goto retry;
	}
	/*
	 * Adjust the number of inodes
	 */
	new_inodes =(unsigned long long) fs->super->s_inodes_per_group * fs->group_desc_count;
	if (new_inodes > ~0U) {
		new_inodes = (unsigned long long) fs->super->s_inodes_per_group * (fs->group_desc_count - 1);
		if (new_inodes <= ~0U) {
			unsigned long long new_blocks =
		((unsigned long long) fs->super->s_blocks_per_group *
		 (fs->group_desc_count - 1)) + fs->super->s_first_data_block;

			ext2fs_blocks_count_set(fs->super, new_blocks);
			goto retry;
		}
		fprintf(stderr, _("inodes (%llu) must be less than %u\n"),
			(unsigned long long) new_inodes, ~0U);
		return EXT2_ET_TOO_MANY_INODES;
	}
	fs->super->s_inodes_count = fs->super->s_inodes_per_group *
		fs->group_desc_count;

	/*
	 * Adjust the number of free blocks
	 */
	blk = ext2fs_blocks_count(old_fs->super);
	if (blk > ext2fs_blocks_count(fs->super))
		ext2fs_free_blocks_count_set(fs->super, 
			ext2fs_free_blocks_count(fs->super) -
			(blk - ext2fs_blocks_count(fs->super)));
	else
		ext2fs_free_blocks_count_set(fs->super, 
			ext2fs_free_blocks_count(fs->super) +
			(ext2fs_blocks_count(fs->super) - blk));

	/*
	 * Adjust the number of reserved blocks
	 */
	percent = (ext2fs_r_blocks_count(old_fs->super) * 100.0) /
		ext2fs_blocks_count(old_fs->super);
	ext2fs_r_blocks_count_set(fs->super,
				  (percent * ext2fs_blocks_count(fs->super) /
				   100.0));

	/*
	 * Adjust the bitmaps for size
	 */
	retval = ext2fs_resize_inode_bitmap2(fs->super->s_inodes_count,
					    fs->super->s_inodes_count,
					    fs->inode_map);
	if (retval) goto errout;

	real_end = EXT2_GROUPS_TO_CLUSTERS(fs->super, fs->group_desc_count) -
		1 + EXT2FS_B2C(fs, fs->super->s_first_data_block);
	retval = ext2fs_resize_block_bitmap2(new_size - 1,
					     real_end, fs->block_map);
	if (retval) goto errout;

	/*
	 * If we are growing the file system, also grow the size of
	 * the reserve_blocks bitmap
	 */
	if (reserve_blocks && new_size > ext2fs_blocks_count(old_fs->super)) {
		retval = ext2fs_resize_block_bitmap2(new_size - 1,
						     real_end, reserve_blocks);
		if (retval) goto errout;
	}

	/*
	 * Reallocate the group descriptors as necessary.
	 */
	if (EXT2_DESC_SIZE(old_fs->super) == EXT2_DESC_SIZE(fs->super) &&
	    old_fs->desc_blocks != fs->desc_blocks) {
		retval = ext2fs_resize_mem(old_fs->desc_blocks *
					   fs->blocksize,
					   fs->desc_blocks * fs->blocksize,
					   &fs->group_desc);
		if (retval)
			goto errout;
		if (fs->desc_blocks > old_fs->desc_blocks)
			memset((char *) fs->group_desc +
			       (old_fs->desc_blocks * fs->blocksize), 0,
			       (fs->desc_blocks - old_fs->desc_blocks) *
			       fs->blocksize);
	}

	/*
	 * If the resize_inode feature is set, and we are changing the
	 * number of descriptor blocks, then adjust
	 * s_reserved_gdt_blocks if possible to avoid needing to move
	 * the inode table either now or in the future.
	 *
	 * Note: If we're converting to 64bit mode, we did this earlier.
	 */
	if (EXT2_DESC_SIZE(old_fs->super) == EXT2_DESC_SIZE(fs->super))
		adjust_reserved_gdt_blocks(old_fs, fs);

	if (ext2fs_has_feature_meta_bg(fs->super) &&
	    (fs->super->s_first_meta_bg > fs->desc_blocks)) {
		ext2fs_clear_feature_meta_bg(fs->super);
		fs->super->s_first_meta_bg = 0;
	}

	/*
	 * Update the location of the backup superblocks if the
	 * sparse_super2 feature is enabled.
	 */
	if (ext2fs_has_feature_sparse_super2(fs->super)) {
		dgrp_t last_bg = fs->group_desc_count - 1;
		dgrp_t old_last_bg = old_fs->group_desc_count - 1;

		if (last_bg > old_last_bg) {
			if (old_fs->group_desc_count == 1)
				fs->super->s_backup_bgs[0] = 1;
			if ((old_fs->group_desc_count < 3 &&
			     fs->group_desc_count > 2) ||
			    fs->super->s_backup_bgs[1])
				fs->super->s_backup_bgs[1] = last_bg;
		} else if (last_bg < old_last_bg) {
			if (fs->super->s_backup_bgs[0] > last_bg)
				fs->super->s_backup_bgs[0] = 0;
			if (fs->super->s_backup_bgs[1] > last_bg)
				fs->super->s_backup_bgs[1] = 0;
			if (last_bg > 1 &&
			    old_fs->super->s_backup_bgs[1] == old_last_bg)
				fs->super->s_backup_bgs[1] = last_bg;
		}
	}

	/*
	 * If we are shrinking the number of block groups, we're done
	 * and can exit now.
	 */
	if (old_fs->group_desc_count > fs->group_desc_count) {
		/*
		 * Check the block groups that we are chopping off
		 * and free any blocks associated with their metadata
		 */
		retval = free_gdp_blocks(fs, reserve_blocks, old_fs,
					 fs->group_desc_count);
		goto errout;
	}

	/*
	 * Fix the count of the last (old) block group
	 */
	old_numblocks = (ext2fs_blocks_count(old_fs->super) -
			 old_fs->super->s_first_data_block) %
				 old_fs->super->s_blocks_per_group;
	if (!old_numblocks)
		old_numblocks = old_fs->super->s_blocks_per_group;
	if (old_fs->group_desc_count == fs->group_desc_count) {
		numblocks = (ext2fs_blocks_count(fs->super) -
			     fs->super->s_first_data_block) %
			fs->super->s_blocks_per_group;
		if (!numblocks)
			numblocks = fs->super->s_blocks_per_group;
	} else
		numblocks = fs->super->s_blocks_per_group;
	i = old_fs->group_desc_count - 1;
	ext2fs_bg_free_blocks_count_set(fs, i, ext2fs_bg_free_blocks_count(fs, i) + (numblocks - old_numblocks));
	ext2fs_group_desc_csum_set(fs, i);

	/*
	 * If the number of block groups is staying the same, we're
	 * done and can exit now.  (If the number block groups is
	 * shrinking, we had exited earlier.)
	 */
	if (old_fs->group_desc_count >= fs->group_desc_count) {
		retval = 0;
		goto errout;
	}

	/*
	 * Initialize the new block group descriptors
	 */
	group_block = ext2fs_group_first_block2(fs,
						old_fs->group_desc_count);
	csum_flag = ext2fs_has_group_desc_csum(fs);
	if (getenv("RESIZE2FS_FORCE_LAZY_ITABLE_INIT") ||
	    (!getenv("RESIZE2FS_FORCE_ITABLE_INIT") &&
	     access("/sys/fs/ext4/features/lazy_itable_init", F_OK) == 0))
		lazy_itable_init = 1;
	if (ext2fs_has_feature_meta_bg(fs->super))
		old_desc_blocks = fs->super->s_first_meta_bg;
	else
		old_desc_blocks = fs->desc_blocks +
			fs->super->s_reserved_gdt_blocks;

	/*
	 * If we changed the number of block_group descriptor blocks,
	 * we need to make sure they are all marked as reserved in the
	 * filesystem's block allocation map.
	 */
	for (i = 0; i < old_fs->group_desc_count; i++)
		ext2fs_reserve_super_and_bgd(fs, i, fs->block_map);

	for (i = old_fs->group_desc_count;
	     i < fs->group_desc_count; i++) {
		memset(ext2fs_group_desc(fs, fs->group_desc, i), 0,
		       sizeof(struct ext2_group_desc));
		adjblocks = 0;

		ext2fs_bg_flags_zap(fs, i);
		if (csum_flag) {
			ext2fs_bg_flags_set(fs, i, EXT2_BG_INODE_UNINIT);
			if (!lazy_itable_init)
				ext2fs_bg_flags_set(fs, i,
						    EXT2_BG_INODE_ZEROED);
			ext2fs_bg_itable_unused_set(fs, i,
					fs->super->s_inodes_per_group);
		}

		numblocks = ext2fs_group_blocks_count(fs, i);
		if ((i < fs->group_desc_count - 1) && csum_flag)
			ext2fs_bg_flags_set(fs, i, EXT2_BG_BLOCK_UNINIT);

		has_super = ext2fs_bg_has_super(fs, i);
		if (has_super) {
			ext2fs_block_alloc_stats2(fs, group_block, +1);
			adjblocks++;
		}
		meta_bg_size = EXT2_DESC_PER_BLOCK(fs->super);
		meta_bg = i / meta_bg_size;
		if (!ext2fs_has_feature_meta_bg(fs->super) ||
		    (meta_bg < fs->super->s_first_meta_bg)) {
			if (has_super) {
				for (j=0; j < old_desc_blocks; j++)
					ext2fs_block_alloc_stats2(fs,
						 group_block + 1 + j, +1);
				adjblocks += old_desc_blocks;
			}
		} else {
			if (has_super)
				has_super = 1;
			if (((i % meta_bg_size) == 0) ||
			    ((i % meta_bg_size) == 1) ||
			    ((i % meta_bg_size) == (meta_bg_size-1)))
				ext2fs_block_alloc_stats2(fs,
						 group_block + has_super, +1);
		}

		adjblocks += 2 + fs->inode_blocks_per_group;

		numblocks -= adjblocks;
		ext2fs_free_blocks_count_set(fs->super,
			     ext2fs_free_blocks_count(fs->super) - adjblocks);
		fs->super->s_free_inodes_count +=
			fs->super->s_inodes_per_group;
		ext2fs_bg_free_blocks_count_set(fs, i, numblocks);
		ext2fs_bg_free_inodes_count_set(fs, i,
						fs->super->s_inodes_per_group);
		ext2fs_bg_used_dirs_count_set(fs, i, 0);
		ext2fs_group_desc_csum_set(fs, i);

		retval = ext2fs_allocate_group_table(fs, i, 0);
		if (retval) goto errout;

		group_block += fs->super->s_blocks_per_group;
	}
	retval = 0;

	/*
	 * Mark all of the metadata blocks as reserved so they won't
	 * get allocated by the call to ext2fs_allocate_group_table()
	 * in blocks_to_move(), where we allocate new blocks to
	 * replace those allocation bitmap and inode table blocks
	 * which have to get relocated to make space for an increased
	 * number of the block group descriptors.
	 */
	if (reserve_blocks)
		mark_table_blocks(fs, reserve_blocks);

errout:
	return (retval);
}

/*
 * Rebuild the resize inode for the new number of descriptor blocks and
 * block groups.
 */
errcode_t fix_resize_inode(ext2_filsys fs)
{
	struct ext2_inode	inode;
	errcode_t		retval;

	if (!ext2fs_has_feature_resize_inode(fs->super))
		return 0;

	retval = ext2fs_read_inode(fs, EXT2_RESIZE_INO, &inode);
	if (retval) goto errout;

	ext2fs_iblk_set(fs, &inode, 1);

	retval = ext2fs_write_inode(fs, EXT2_RESIZE_INO, &inode);
	if (retval) goto errout;

	if (!inode.i_block[EXT2_DIND_BLOCK]) {
		/*
		 * Avoid zeroing out block #0; that's rude.  This
		 * should never happen anyway since the filesystem
		 * should be fsck'ed and we assume it is consistent.
		 */
		fprintf(stderr, "%s",
			_("Should never happen: resize inode corrupt!\n"));
		return EXT2_ET_RESIZE_INODE_CORRUPT;
	}

	retval = ext2fs_zero_blocks2(fs, inode.i_block[EXT2_DIND_BLOCK], 1,
				     NULL, NULL);
	if (retval)
		goto errout;

	retval = ext2fs_create_resize_inode(fs);
	if (retval)
		goto errout;

errout:
	return retval;
}

/*
 * Recount the free blocks and inodes of every group and of the file system
 * from the bitmaps.
 */
errcode_t resize2fs_calculate_summary_stats(ext2_filsys fs)
{
	errcode_t	retval;
	blk64_t		b, blk = fs->super->s_first_data_block;
	ext2_ino_t	ino;
	unsigned int	n, max, group, count;
	blk64_t		total_clusters_free = 0;
	int		total_inodes_free = 0;
	int		group_free = 0;
	int		uninit = 0;
	char		*bitmap_buf;

	/*
	 * First calculate the block statistics
	 */
	bitmap_buf = malloc(fs->blocksize);
	if (!bitmap_buf)
		return ENOMEM;
	for (group = 0; group < fs->group_desc_count; group++) {
		retval = ext2fs_get_block_bitmap_range2(fs->block_map,
			B2C(blk), fs->super->s_clusters_per_group, bitmap_buf);
		if (retval) {
			free(bitmap_buf);
			return retval;
		}
		max = ext2fs_group_blocks_count(fs, group) >>
		       fs->cluster_ratio_bits;
		if ((group == fs->group_desc_count - 1) && (max & 7)) {
			n = 0;
			for (b = (fs->super->s_first_data_block +
				  ((blk64_t) fs->super->s_blocks_per_group *
				   group));
			     b < ext2fs_blocks_count(fs->super);
			     b += EXT2FS_CLUSTER_RATIO(fs)) {
				if (ext2fs_test_block_bitmap2(fs->block_map, b))
					n++;
			}
		} else {
			n = ext2fs_bitcount(bitmap_buf, (max + 7) / 8);
		}
		group_free = max - n;
		total_clusters_free += group_free;
		ext2fs_bg_free_blocks_count_set(fs, group, group_free);
		ext2fs_group_desc_csum_set(fs, group);
		blk += fs->super->s_blocks_per_group;
	}
	free(bitmap_buf);
	ext2fs_free_blocks_count_set(fs->super, C2B(total_clusters_free));

	/*
	 * Next, calculate the inode statistics
	 */
	group_free = 0;
	count = 0;
	group = 0;

	/* Protect loop from wrap-around if s_inodes_count maxed */
	uninit = ext2fs_bg_flags_test(fs, group, EXT2_BG_INODE_UNINIT);
	for (ino = 1; ino <= fs->super->s_inodes_count && ino > 0; ino++) {
		if (uninit ||
		    !ext2fs_fast_test_inode_bitmap2(fs->inode_map, ino)) {
			group_free++;
			total_inodes_free++;
		}
		count++;
		if ((count == fs->super->s_inodes_per_group) ||
		    (ino == fs->super->s_inodes_count)) {
			ext2fs_bg_free_inodes_count_set(fs, group, group_free);
			ext2fs_group_desc_csum_set(fs, group);
			group++;
			if (group >= fs->group_desc_count)
				break;
			count = 0;
			group_free = 0;
			uninit = ext2fs_bg_flags_test(fs, group, EXT2_BG_INODE_UNINIT);
		}
	}
	fs->super->s_free_inodes_count = total_inodes_free;
	ext2fs_mark_super_dirty(fs);
	return 0;
}
