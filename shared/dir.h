/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_DIR_H
#define NGNFS_SHARED_DIR_H

int ngnfs_dir_create(struct ngnfs_fs_info *nfi, u64 dir_ino, umode_t mode, char *name,
		     size_t name_len);

/*
 * Readddr fills the buffer with entries.  The start of the buffer must
 * be at least as aligned as the entry.  Entries are padded for
 * alignment.  Use next_off to iterate through them instead of trying to
 * skip name length bytes past the end of the struct.
 */
struct ngnfs_readdir_entry {
	u64 pos;
	u64 ino;
	u64 gen;
	u16 next_offset;	/* bytes to add to entry to get next entry */
	u8 dtype;
	u8 name_len;		/* does not include terminating null, like strlen */
	u8 name[0];
};

/*
 * Minimum readdir entry buf size needed to store a single entry with a
 * maximum name size.  The buffer must be this size or an error is
 * returned.
 */
#define NGNFS_READDIR_MIN_BUF_SIZE \
	offsetof(struct ngnfs_readdir_entry , name[NGNFS_NAME_MAX + 1])

/*
 * Returns the number of entries filled into the buffer, not the number
 * of bytes.
 */
int ngnfs_dir_readdir(struct ngnfs_fs_info *nfi, u64 dir_ino, u64 pos,
		      struct ngnfs_readdir_entry *buf, size_t size);

#endif
