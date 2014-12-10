#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"


#define CLUSTER_ZEROMASK (0)
#define CLUSTER_ALLMASK (255)
#define CLUSTER_USED (1)
#define CLUSTER_POINTED (1 << 1)
#define CLUSTER_BAD (1 << 2)
#define CLUSTER_MULTIPOINTED (1 << 3)



void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

// Prof Sommers Code
//
//
void print_indent(int indent)
{
    int i;
    for (i = 0; i < indent*4; i++)
	printf(" ");
}


uint16_t print_dirent(struct direntry *dirent, int indent)
{
    uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY) {
	return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED) {
	return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E) {
	// dot entry ("." or "..")
	// skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--)  {
	if (name[i] == ' ') 
	    name[i] = '\0';
	else 
	    break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--)  {
	if (extension[i] == ' ') 
	    extension[i] = '\0';
	else 
	    break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN) {
	// ignore any long file name extension entries
	//
	// printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    } else if ((dirent->deAttributes & ATTR_VOLUME) != 0)  {
	printf("Volume: %s\n", name);
    } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
	if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN) {
	    print_indent(indent);
    	    printf("%s/ (directory)\n", name);
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
        }
    } else {
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
	int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
	int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
	int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
	int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;

	size = getulong(dirent->deFileSize);
	print_indent(indent);
	printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n", 
	       name, extension, size, getushort(dirent->deStartCluster),
	       ro?'r':' ', 
               hidden?'h':' ', 
               sys?'s':' ', 
               arch?'a':' ');
    }

    return followclust;
}

void follow_dir(uint16_t cluster, int indent,
		uint8_t *image_buf, struct bpb33* bpb) {
    while (is_valid_cluster(cluster, bpb)) {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	for ( ; i < numDirEntries; i++)	{
        uint16_t followclust = print_dirent(dirent, indent);
        if (followclust) {
            follow_dir(followclust, indent+1, image_buf, bpb);
        }
        dirent++;
	}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}

// End of Prof Sommers code

void dos_ls(uint8_t *image_buf, struct bpb33 *bpb) {
    struct direntry *dirent = (struct direntry *)  root_dir_addr(image_buf, bpb);
    for (int i = 0; i < bpb -> bpbRootDirEnts; i++) {
        uint16_t followclust = print_dirent(dirent, 0);
        if (is_valid_cluster(followclust, bpb)) {
            follow_dir(followclust, 1, image_buf, bpb);
        }
        dirent++;
    }
}

void test_FAT(uint8_t *image_buf, struct bpb33 *bpb) {

}

// Return the value of the FAT at a cluster number, masked for FAT12
uint16_t get_12masked_fat_entry(uint16_t cluster, uint8_t *image_buf, struct bpb33 *bpb) {
    uint16_t value = get_fat_entry(cluster, image_buf, bpb); 
    return value & FAT12_MASK;
}

int cluster_trace(uint16_t start_cluster, uint8_t *image_buf, struct bpb33 *bpb) {
    int cluster_count = 0;
    uint16_t cluster= get_12masked_fat_entry(start_cluster, image_buf, bpb);
    while (!is_end_of_file(cluster)) {
        cluster_count ++;
        cluster = get_12masked_fat_entry(cluster, image_buf, bpb);
    } 
    cluster_count ++;
    printf("Total cluster occupied is: %d\n", cluster_count);
    return cluster_count;
}

void check_free_cluster(uint8_t **cluster_info, uint8_t *image_buf, struct bpb33 *bpb) {
    // Assumes cluster_info is clean and pristine
    uint16_t cluster = 0;
    for (int i = 2; i < bpb -> bpbSectors + 2; i++) {
        cluster = get_12masked_fat_entry(i, image_buf, bpb);
        if (cluster != CLUST_FREE) {
            *(cluster_info[i]) &= CLUSTER_USED;
        }
        // don't forget to check bad cluster!
    }

}

int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
	usage(argv[0]);
    }

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

    // your code should start here...
    dos_ls(image_buf, bpb);

    int num_cluster = bpb -> bpbSectors;

    // Array to keep track of cluster info
    uint8_t cluster_info[num_cluster + 2];
    for (int i = 0; i < num_cluster + 2; i++) {
        cluster_info[i] = CLUSTER_ZEROMASK;
    }

    int free_count = 1;
    for (int i = 2; i < num_cluster + 2; i++) {
        uint16_t cluster = get_fat_entry(i, image_buf, bpb);
        cluster = cluster & FAT12_MASK;
        if (cluster == CLUST_FREE) {
            //printf("We have a free cluster at %d \n", i);
            free_count ++;
        }
    }
    printf("We have this many free sectors: %d\n", free_count);
    cluster_trace(34,image_buf,bpb);   
    //uint16_t cluster = get_fat_entry(0, image_buf, bpb);
    //cluster = cluster & FAT12_MASK;
    //printf("Value of cluster 0 is %#08X \n", cluster);

    unmmap_file(image_buf, &fd);
    return 0;
}
