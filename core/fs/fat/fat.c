#include <stdio.h>
#include <string.h>
#include <sys/dirent.h>
#include <cache.h>
#include <core.h>
#include <disk.h>
#include <fs.h>
#include <klibc/compiler.h>
#include "codepage.h"
#include "fat_fs.h"

static struct inode * new_fat_inode(struct fs_info *fs)
{
    struct inode *inode = alloc_inode(fs, 0, sizeof(struct fat_pvt_inode));
    if (!inode)
	malloc_error("inode structure");

    return inode;
}


/*
 * Check for a particular sector in the FAT cache
 */
static const void *get_fat_sector(struct fs_info *fs, sector_t sector)
{
    return get_cache(fs->fs_dev, FAT_SB(fs)->fat + sector);
}

static uint32_t get_next_cluster(struct fs_info *fs, uint32_t clust_num)
{
    uint32_t next_cluster;
    sector_t fat_sector;
    uint32_t offset;
    int lo, hi;
    uint32_t sector_mask = SECTOR_SIZE(fs) - 1;
    const uint8_t *data;

    switch(FAT_SB(fs)->fat_type) {
    case FAT12:
	offset = clust_num + (clust_num >> 1);
	fat_sector = offset >> SECTOR_SHIFT(fs);
	offset &= sector_mask;
	data = get_fat_sector(fs, fat_sector);
	if (offset == sector_mask) {
	    /*
	     * we got the end of the one fat sector,
	     * but we have just one byte and we need two,
	     * so store the low part, then read the next fat
	     * sector, read the high part, then combine it.
	     */
	    lo = data[offset];
	    data = get_fat_sector(fs, fat_sector + 1);
	    hi = data[0];
	    next_cluster = (hi << 8) + lo;
	} else {
	    next_cluster = *(const uint16_t *)(data + offset);
	}

	if (clust_num & 0x0001)
	    next_cluster >>= 4;         /* cluster number is ODD */
	else
	    next_cluster &= 0x0fff;     /* cluster number is EVEN */
	break;

    case FAT16:
	offset = clust_num << 1;
	fat_sector = offset >> SECTOR_SHIFT(fs);
	offset &= sector_mask;
	data = get_fat_sector(fs, fat_sector);
	next_cluster = *(const uint16_t *)(data + offset);
	break;

    case FAT32:
	offset = clust_num << 2;
	fat_sector = offset >> SECTOR_SHIFT(fs);
	offset &= sector_mask;
	data = get_fat_sector(fs, fat_sector);
	next_cluster = *(const uint32_t *)(data + offset);
	next_cluster &= 0x0fffffff;
	break;
    }

    return next_cluster;
}


static sector_t get_next_sector(struct fs_info* fs, uint32_t sector)
{
    struct fat_sb_info *sbi = FAT_SB(fs);
    sector_t data_area = sbi->data;
    sector_t data_sector;
    uint32_t cluster;
    int clust_shift = sbi->clust_shift;

    if (sector < data_area) {
	/* Root directory sector... */
	sector++;
	if (sector >= data_area)
	    sector = 0; /* Ran out of root directory, return EOF */
	return sector;
    }

    data_sector = sector - data_area;
    if ((data_sector + 1) & sbi->clust_mask)  /* Still in the same cluster */
	return sector + 1;		      /* Next sector inside cluster */

    /* get a new cluster */
    cluster = data_sector >> clust_shift;
    cluster = get_next_cluster(fs, cluster + 2) - 2;

    if (cluster >= sbi->clusters)
	return 0;

    /* return the start of the new cluster */
    sector = (cluster << clust_shift) + data_area;
    return sector;
}

/*
 * Here comes the place I don't like VFAT fs most; if we need seek
 * the file to the right place, we need get the right sector address
 * from begining everytime! Since it's a kind a signle link list, we
 * need to traver from the head-node to find the right node in that list.
 *
 * What a waste of time!
 */
static sector_t get_the_right_sector(struct file *file)
{
    struct inode *inode = file->inode;
    uint32_t sector_pos  = file->offset >> SECTOR_SHIFT(file->fs);
    uint32_t where;
    sector_t sector;

    if (sector_pos < PVT(inode)->offset) {
	/* Reverse seek */
	where = 0;
	sector = PVT(inode)->start;
    } else {
	where = PVT(inode)->offset;
	sector = PVT(inode)->here;
    }

    while (where < sector_pos) {
	sector = get_next_sector(file->fs, sector);
	where++;
    }

    PVT(inode)->offset = sector_pos;
    PVT(inode)->here   = sector;

    return sector;
}

/*
 * Get the next sector in sequence
 */
static sector_t next_sector(struct file *file)
{
    struct inode *inode = file->inode;
    sector_t sector = get_next_sector(file->fs, PVT(inode)->here);
    PVT(inode)->offset++;
    PVT(inode)->here = sector;

    return sector;
}

/**
 * __getfssec:
 *
 * get multiple sectors from a file
 *
 * This routine makes sure the subransfers do not cross a 64K boundary
 * and will correct the situation if it does, UNLESS *sectos* cross
 * 64K boundaries.
 *
 */
static void __getfssec(struct fs_info *fs, char *buf,
                       struct file *file, uint32_t sectors)
{
    sector_t curr_sector = get_the_right_sector(file);
    sector_t frag_start , next_sector;
    uint32_t con_sec_cnt;
    struct disk *disk = fs->fs_dev->disk;

    while (sectors) {
        /* get fragment */
        con_sec_cnt = 0;
        frag_start = curr_sector;

        do {
            /* get consective sector  count */
            con_sec_cnt++;
            sectors--;
            next_sector = get_next_sector(fs, curr_sector);
	    curr_sector++;
        } while (sectors && next_sector == curr_sector);

	PVT(file->inode)->offset += con_sec_cnt;
	PVT(file->inode)->here    = next_sector;

        /* do read */
        disk->rdwr_sectors(disk, buf, frag_start, con_sec_cnt, 0);
        buf += con_sec_cnt << SECTOR_SHIFT(fs);/* adjust buffer pointer */

        curr_sector = next_sector;
    }
}



/**
 * get multiple sectors from a file
 *
 * @param: buf, the buffer to store the read data
 * @param: file, the file structure pointer
 * @param: sectors, number of sectors wanna read
 * @param: have_more, set one if has more
 *
 * @return: number of bytes read
 *
 */
static uint32_t vfat_getfssec(struct file *file, char *buf, int sectors,
			      bool *have_more)
{
    struct fs_info *fs = file->fs;
    uint32_t bytes_left = file->inode->size - file->offset;
    uint32_t bytes_read = sectors << fs->sector_shift;
    int sector_left;

    sector_left = (bytes_left + SECTOR_SIZE(fs) - 1) >> fs->sector_shift;
    if (sectors > sector_left)
        sectors = sector_left;

    __getfssec(fs, buf, file, sectors);

    if (bytes_read >= bytes_left) {
        bytes_read = bytes_left;
        *have_more = 0;
    } else {
        *have_more = 1;
    }
    file->offset += bytes_read;

    return bytes_read;
}

/*
 * Mangle a filename pointed to by src into a buffer pointed to by dst;
 * ends on encountering any whitespace.
 *
 */
static void vfat_mangle_name(char *dst, const char *src)
{
    char *p = dst;
    char c;
    int i = FILENAME_MAX -1;

    /*
     * Copy the filename, converting backslash to slash and
     * collapsing duplicate separators.
     */
    while (not_whitespace(c = *src)) {
        if (c == '\\')
            c = '/';

        if (c == '/') {
            if (src[1] == '/' || src[1] == '\\') {
                src++;
                i--;
                continue;
            }
        }
        i--;
        *dst++ = *src++;
    }

    /* Strip terminal slashes or whitespace */
    while (1) {
        if (dst == p)
            break;
		if (*(dst-1) == '/' && dst-1 == p) /* it's the '/' case */
			break;
        if ((*(dst-1) != '/') && (*(dst-1) != '.'))
            break;

        dst--;
        i++;
    }

    i++;
    for (; i > 0; i --)
        *dst++ = '\0';
}

/*
 * Mangle a normal style string to DOS style string.
 */
static void mangle_dos_name(char *mangle_buf, char *src)
{
    int i;
    unsigned char c;

    i = 0;
    while (i < 11) {
	c = *src++;

	if ((c <= ' ') || (c == '/'))
	    break;

	if (c == '.') {
	    while (i < 8)
		mangle_buf[i++] = ' ';
	    i = 8;
	    continue;
	}

	c = codepage.upper[c];
	if (i == 0 && c == 0xe5)
	    c = 0x05;		/* Special hack for the first byte only! */

	mangle_buf[i++] = c;
    }
    while (i < 11)
	mangle_buf[i++] = ' ';

    mangle_buf[i] = '\0';
}

/*
 * Match a string name against a longname.  "len" is the number of
 * codepoints in the input; including padding.
 *
 * Returns true on match.
 */
static bool vfat_match_longname(const char *str, const uint16_t *match,
				int len)
{
    unsigned char c;
    uint16_t cp;

    while (len) {
	cp = *match++;
	c = *str++;
	if (cp != codepage.uni[0][c] && cp != codepage.uni[1][c])
	    return false;
	if (!c)
	    break;
    }

    if (c)
	return false;

    /* Any padding entries must be FFFF */
    while (len)
	if (*match++ != 0xffff)
	    return false;

    return true;
}

/*
 * Convert an UTF-16 longname to the system codepage; return
 * the length on success or -1 on failure.
 */
static int vfat_cvt_longname(char *entry_name, const uint16_t *long_name)
{
    struct unicache {
	uint16_t utf16;
	uint8_t cp;
    };
    static struct unicache unicache[256];
    struct unicache *uc;
    uint16_t cp;
    unsigned int c;
    char *p = entry_name;

    do {
	cp = *long_name++;
	uc = &unicache[cp % 256];

	if (__likely(uc->utf16 == cp)) {
	    *p++ = uc->cp;
	} else {
	    for (c = 0; c < 512; c++) {
		/* This is a bit hacky... */
		if (codepage.uni[0][c] == cp) {
		    uc->utf16 = cp;
		    *p++ = uc->cp = (uint8_t)c;
		    goto found;
		}
	    }
	    return -1;		/* Impossible character */
	found:
	    ;
	}
    } while (cp);

    return (p-entry_name)-1;
}

static void copy_long_chunk(uint16_t *buf, const struct fat_dir_entry *de)
{
    const struct fat_long_name_entry *le =
	(const struct fat_long_name_entry *)de;

    memcpy(buf,      le->name1, 5 * 2);
    memcpy(buf + 5,  le->name2, 6 * 2);
    memcpy(buf + 11, le->name3, 2 * 2);
}

static uint8_t get_checksum(const char *dir_name)
{
    int  i;
    uint8_t sum = 0;

    for (i = 11; i; i--)
	sum = ((sum & 1) << 7) + (sum >> 1) + (uint8_t)*dir_name++;
    return sum;
}


/* compute the first sector number of one dir where the data stores */
static inline sector_t first_sector(struct fs_info *fs,
				    const struct fat_dir_entry *dir)
{
    const struct fat_sb_info *sbi = FAT_SB(fs);
    sector_t first_clust;
    sector_t sector;

    first_clust = (dir->first_cluster_high << 16) + dir->first_cluster_low;
    sector = ((first_clust - 2) << sbi->clust_shift) + sbi->data;

    return sector;
}

static inline int get_inode_mode(uint8_t attr)
{
    if (attr == FAT_ATTR_DIRECTORY)
	return I_DIR;
    else
	return I_FILE;
}


static struct inode *vfat_find_entry(char *dname, struct inode *dir)
{
    struct fs_info *fs = dir->fs;
    struct inode *inode;
    const struct fat_dir_entry *de;
    struct fat_long_name_entry *long_de;

    char mangled_name[12];
    uint16_t long_name[260];	/* == 20*13 */
    int long_len;

    sector_t dir_sector = PVT(dir)->start;
    uint8_t vfat_init, vfat_next, vfat_csum = 0;
    uint8_t id;
    int slots;
    int entries;
    int checksum;
    int long_match = 0;

    slots = (strlen(dname) + 12) / 13;
    if (slots > 20)
	return NULL;		/* Name too long */

    slots |= 0x40;
    vfat_init = vfat_next = slots;
    long_len = slots*13;

    /* Produce the shortname version, in case we need it. */
    mangle_dos_name(mangled_name, dname);

    while (dir_sector) {
	de = get_cache(fs->fs_dev, dir_sector);
	entries = 1 << (fs->sector_shift - 5);

	while (entries--) {
	    if (de->name[0] == 0)
		return NULL;

	    if (de->attr == 0x0f) {
		/*
		 * It's a long name entry.
		 */
		long_de = (struct fat_long_name_entry *)de;
		id = long_de->id;
		if (id != vfat_next)
		    goto not_match;

		if (id & 0x40) {
		    /* get the initial checksum value */
		    vfat_csum = long_de->checksum;
		    id &= 0x3f;
		    long_len = id * 13;

		    /* ZERO the long_name buffer */
		    memset(long_name, 0, sizeof long_name);
		} else {
		    if (long_de->checksum != vfat_csum)
			goto not_match;
		}

		vfat_next = --id;

		/* got the long entry name */
		copy_long_chunk(long_name + id*13, de);

		/*
		 * If we got the last entry, check it.
		 * Or, go on with the next entry.
		 */
		if (id == 0) {
		    if (!vfat_match_longname(dname, long_name, long_len))
			goto not_match;
		    long_match = 1;
		}
		de++;
		continue;     /* Try the next entry */
	    } else {
		/*
		 * It's a short entry
		 */
		if (de->attr & 0x08) /* ignore volume labels */
		    goto not_match;

		if (long_match) {
		    /*
		     * We already have a VFAT long name match. However, the
		     * match is only valid if the checksum matches.
		     */
		    checksum = get_checksum(de->name);
		    if (checksum == vfat_csum)
			goto found;  /* Got it */
		} else {
		    if (!memcmp(mangled_name, de->name, 11))
			goto found;
		}
	    }

	not_match:
	    vfat_next = vfat_init;
	    long_match = 0;

	    de++;
	}

	/* Try with the next sector */
	dir_sector = get_next_sector(fs, dir_sector);
    }
    return NULL;		/* Nothing found... */

found:
    inode = new_fat_inode(fs);
    inode->size = de->file_size;
    PVT(inode)->start = PVT(inode)->here = first_sector(fs, de);
    inode->mode = get_inode_mode(de->attr);

    return inode;
}

static struct inode *vfat_iget_root(struct fs_info *fs)
{
    struct inode *inode = new_fat_inode(fs);
    int root_size = FAT_SB(fs)->root_size;

    /*
     * For FAT32, the only way to get the root directory size is to
     * follow the entire FAT chain to the end... which seems pointless.
     */
    inode->size = root_size ? root_size << fs->sector_shift : ~0;
    PVT(inode)->start = PVT(inode)->here = FAT_SB(fs)->root;
    inode->mode = I_DIR;

    return inode;
}

static struct inode *vfat_iget(char *dname, struct inode *parent)
{
    return vfat_find_entry(dname, parent);
}

static struct dirent * vfat_readdir(struct file *file)
{
    struct fs_info *fs = file->fs;
    struct dirent *dirent;
    const struct fat_dir_entry *de;
    const char *data;
    const struct fat_long_name_entry *long_de;

    sector_t sector = get_the_right_sector(file);

    uint16_t long_name[261];	/* == 20*13 + 1 (to guarantee null) */
    char filename[261];

    uint8_t vfat_init, vfat_next, vfat_csum;
    uint8_t id;
    int entries_left;
    int checksum;
    int long_entry = 0;
    int sec_off = file->offset & ((1 << fs->sector_shift) - 1);

    data = get_cache(fs->fs_dev, sector);
    de = (const struct fat_dir_entry *)(data + sec_off);
    entries_left = ((1 << fs->sector_shift) - sec_off) >> 5;

    vfat_next = vfat_csum = 0xff;

    while (1) {
	while(entries_left--) {
	    if (de->name[0] == 0)
		return NULL;
	    if ((uint8_t)de->name[0] == 0xe5)
		goto invalid;

	    if (de->attr == 0x0f) {
		/*
		 * It's a long name entry.
		 */
		long_de = (struct fat_long_name_entry *)de;
		id = long_de->id;

		if (id & 0x40) {
		    /* init vfat_csum and vfat_init */
		    vfat_csum = long_de->checksum;
		    id &= 0x3f;
		    if (id >= 20)
			goto invalid; /* Too long! */

		    vfat_init = id;

		    /* ZERO the long_name buffer */
		    memset(long_name, 0, sizeof long_name);
		} else {
		    if (long_de->checksum != vfat_csum ||
			id != vfat_next)
			goto invalid;
		}

		vfat_next = --id;

		/* got the long entry name */
		copy_long_chunk(long_name + id*13, de);

		if (id == 0) {
		    int longlen =
			vfat_cvt_longname(filename, long_name);
		    if (longlen > 0 && longlen < sizeof(dirent->d_name))
			long_entry = 1;
		}

		de++;
		file->offset += sizeof(struct fat_dir_entry);
		continue;     /* Try the next entry */
	    } else {
		/*
		 * It's a short entry
		 */
		if (de->attr & 0x08) /* ignore volume labels */
		    goto invalid;

		if (long_entry == 1) {
		    /* Got a long entry */
		    checksum = get_checksum(de->name);
		    if (checksum == vfat_csum)
			goto got;
		} else {
		    /* Use the shortname */
		    int i;
		    uint8_t c;
		    char *p = filename;

		    for (i = 0; i < 8; i++) {
			c = de->name[i];
			if (c == ' ')
			    break;
			if (de->lcase & LCASE_BASE)
			    c = codepage.lower[c];
			*p++ = c;
		    }
		    if (de->name[8] != ' ') {
			*p++ = '.';
			for (i = 8; i < 11; i++) {
			    c = de->name[i];
			    if (c == ' ')
				break;
			    if (de->lcase & LCASE_EXT)
				c = codepage.lower[c];
			    *p++ = c;
			}
		    }
		    *p = '\0';

		    goto got;
		}
	    }

	invalid:
	    de++;
	    file->offset += sizeof(struct fat_dir_entry);
	}

	/* Try with the next sector */
	sector = next_sector(file);
	if (!sector)
	    return NULL;
	de = get_cache(fs->fs_dev, sector);
	entries_left = 1 << (fs->sector_shift - 5);
    }

got:
    if (!(dirent = malloc(sizeof(*dirent)))) {
	malloc_error("dirent structure in vfat_readdir");
	return NULL;
    }
    dirent->d_ino = de->first_cluster_low | (de->first_cluster_high << 16);
    dirent->d_off = file->offset;
    dirent->d_reclen = 0;
    dirent->d_type = get_inode_mode(de->attr);
    strcpy(dirent->d_name, filename);

    file->offset += sizeof(*de);  /* Update for next reading */

    return dirent;
}

/* Load the config file, return 1 if failed, or 0 */
static int vfat_load_config(void)
{
    const char *search_directories[] = {
	"/boot/syslinux", 
	"/syslinux",
	"/",
	NULL
    };
    com32sys_t regs;
    int i;

    /* If installed by extlinux, try the extlinux filename */
    if (*CurrentDirName && !generic_load_config())
	return 0;

    for (i = 0; search_directories[i]; i++) {
	    memset(&regs, 0, sizeof regs);
	    snprintf(ConfigName, FILENAME_MAX, "%s/syslinux.cfg",
		     search_directories[i]);
	    regs.edi.w[0] = OFFS_WRT(ConfigName, 0);
	    call16(core_open, &regs, &regs);
	    if (!(regs.eflags.l & EFLAGS_ZF))
		break;
    }
    if (!search_directories[i])
	return -1;

    /* Set the current working directory */
    chdir(search_directories[i]);
    return 0;
}

static inline __constfunc uint32_t bsr(uint32_t num)
{
    asm("bsrl %1,%0" : "=r" (num) : "rm" (num));
    return num;
}

/* init. the fs meta data, return the block size in bits */
static int vfat_fs_init(struct fs_info *fs)
{
    struct fat_bpb fat;
    struct fat_sb_info *sbi;
    struct disk *disk = fs->fs_dev->disk;
    int sectors_per_fat;
    uint32_t clusters;
    sector_t total_sectors;

    fs->sector_shift = fs->block_shift = disk->sector_shift;
    fs->sector_size  = 1 << fs->sector_shift;
    fs->block_size   = 1 << fs->block_shift;

    disk->rdwr_sectors(disk, &fat, 0, 1, 0);

    /* XXX: Find better sanity checks... */
    if (!fat.bxResSectors || !fat.bxFATs)
	return -1;
    sbi = malloc(sizeof(*sbi));
    if (!sbi)
	malloc_error("fat_sb_info structure");
    fs->fs_info = sbi;

    sectors_per_fat = fat.bxFATsecs ? : fat.fat32.bxFATsecs_32;
    total_sectors   = fat.bxSectors ? : fat.bsHugeSectors;

    sbi->fat       = fat.bxResSectors;
    sbi->root      = sbi->fat + sectors_per_fat * fat.bxFATs;
    sbi->root_size = root_dir_size(fs, &fat);
    sbi->data      = sbi->root + sbi->root_size;

    sbi->clust_shift      = bsr(fat.bxSecPerClust);
    sbi->clust_byte_shift = sbi->clust_shift + fs->sector_shift;
    sbi->clust_mask       = fat.bxSecPerClust - 1;
    sbi->clust_size       = fat.bxSecPerClust << fs->sector_shift;

    clusters = (total_sectors - sbi->data) >> sbi->clust_shift;
    if (clusters <= 0xff4) {
	sbi->fat_type = FAT12;
    } else if (clusters <= 0xfff4) {
	sbi->fat_type = FAT16;
    } else {
	sbi->fat_type = FAT32;

	if (clusters > 0x0ffffff4)
	    clusters = 0x0ffffff4; /* Maximum possible */

	if (fat.fat32.extended_flags & 0x80) {
	    /* Non-mirrored FATs, we need to read the active one */
	    sbi->fat += (fat.fat32.extended_flags & 0x0f) * sectors_per_fat;
	}

	/* FAT32: root directory is a cluster chain */
	sbi->root = sbi->data
	    + ((fat.fat32.root_cluster-2) << sbi->clust_shift);
    }
    sbi->clusters = clusters;

    /* Initialize the cache */
    cache_init(fs->fs_dev, fs->block_shift);

    return fs->block_shift;
}

const struct fs_ops vfat_fs_ops = {
    .fs_name       = "vfat",
    .fs_flags      = FS_USEMEM | FS_THISIND,
    .fs_init       = vfat_fs_init,
    .searchdir     = NULL,
    .getfssec      = vfat_getfssec,
    .close_file    = generic_close_file,
    .mangle_name   = vfat_mangle_name,
    .unmangle_name = generic_unmangle_name,
    .load_config   = vfat_load_config,
    .readdir       = vfat_readdir,
    .iget_root     = vfat_iget_root,
    .iget          = vfat_iget,
};