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


// cluster flags
#define CLUSTER_ZEROMASK (0)      // initial cluster flag
#define CLUSTER_ALLMASK (255)     // full flag
#define CLUSTER_USED (1)          // cluster is used
#define CLUSTER_POINTED (1 << 1)  // cluster is pointed by another cluster
#define CLUSTER_BAD (1 << 2)      // cluster is invalid
#define CLUSTER_DUPE (1 << 3)     // cluster points to already pointed cluster
#define CLUSTER_DEAD (1 << 4)     // cluster points to an invalid cluster #
#define CLUSTER_NULL (1 << 5)     // the file is empty
#define CLUSTER_LESS (1 << 6)     // less cluster than expected
#define CLUSTER_MORE (1 << 7)     // more cluster than expected

struct corruption_info {
    struct direntry *file;
    uint8_t anomaly_flag;
    struct corruption_info *next;
};

struct disk_info {
    uint8_t *image_buf;
    struct bpb33 *bpb;
    uint8_t *cluster_info;
    struct corruption_info *corr_info;
};

struct corruption_info *cluster_trace(struct direntry *, struct disk_info *,
                  int, uint32_t); 

void add_corr_entry(struct disk_info *disk_info, struct corruption_info *info) {
    struct corruption_info *last = disk_info -> corr_info;
    if (last == NULL) {
        disk_info -> corr_info = info; 
    } else {
        while (last -> next != NULL) {
            last = last -> next;
        }
        last -> next = info;
    }
}

// Get the file name from a dirent. Assuming this is a valid dirent
void get_file_name(struct direntry *dirent, char *fullname) {
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);

   /* names are space padded - remove the spaces */
    for (int i = 8; i > 0; i--)  {
	if (name[i] == ' ') 
	    name[i] = '\0';
	else 
	    break;
    }

    /* remove the spaces from extensions */
    for (int i = 3; i > 0; i--)  {
	if (extension[i] == ' ') 
	    extension[i] = '\0';
	else 
	    break;
    }

    fullname[0] = '\0';
    strcat(fullname,name);
    strcat(fullname,".");
    strcat(fullname,extension);

}

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


uint16_t print_dirent(struct direntry *dirent, int indent,
                      uint16_t cluster, struct disk_info *disk_info) {
    uint8_t *cluster_info = disk_info -> cluster_info; 
    uint8_t *image_buf = disk_info -> image_buf; 
    struct bpb33 *bpb = disk_info -> bpb;

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

            // Change cluster_info to mark file_cluster as being pointed to
            // We assume the directory only takes one cluster
            cluster_info[file_cluster] |= CLUSTER_POINTED;
        }
    } else {
        /*
         * a "regular" file entry
         * print size, starting cluster, etc.
         */

        size = getulong(dirent->deFileSize);
        print_indent(indent);
        printf("%s.%s (%u bytes) (starting cluster %d)\n", 
           name, extension, size, getushort(dirent->deStartCluster));
       
        uint16_t sectorSize= bpb -> bpbBytesPerSec;
        uint32_t expected_cluster_num = (size + sectorSize - 1) / sectorSize;
        print_indent(indent+1);
        printf("Expected sectors occupied based on size: %d \n", expected_cluster_num);
   
        // Go through the FAT chain of the file and mark cluster as being
        // pointer to. Done through the cluster_trace function
        cluster_trace(dirent, disk_info, indent+1, expected_cluster_num);

    }

    return followclust;
}

void follow_dir(uint16_t cluster, int indent, struct disk_info *disk_info) {
    uint8_t *image_buf = disk_info -> image_buf; 
    struct bpb33* bpb = disk_info -> bpb;
    uint8_t *cluster_info = disk_info -> cluster_info;

    while (is_valid_cluster(cluster, bpb)) {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
        
        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        printf("Number of dir entries are: %d \n", numDirEntries);
        for (int i = 0 ; i < numDirEntries; i++) {
            uint16_t followclust = print_dirent(dirent, indent, cluster, disk_info);
            if (followclust) {
                follow_dir(followclust, indent+1, disk_info);
            }
            dirent++;
        }

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}

// End of Prof Sommers code

void dos_ls(struct disk_info *disk_info) {
    uint8_t *image_buf = disk_info -> image_buf; 
    struct bpb33 *bpb = disk_info -> bpb; 
    uint8_t *cluster_info = disk_info -> cluster_info;

    struct direntry *dirent = (struct direntry *)  root_dir_addr(image_buf, bpb);
    for (int i = 0; i < bpb -> bpbRootDirEnts; i++) {
        // 19 is the cluster number of the root dir
        uint16_t followclust = print_dirent(dirent, 0, 19, disk_info);
        if (is_valid_cluster(followclust, bpb)) {
            follow_dir(followclust, 1, disk_info);
        }
        dirent++;
    }
}

// Trace the FAT chain starting from start_cluster
// Mark the corresponding index in cluster_info as being pointed to
// Return the total number of cluster in the chain
struct corruption_info *cluster_trace(struct direntry *dirent, struct disk_info *disk_info, int indent, uint32_t num_of_cluster) {
    uint8_t *image_buf = disk_info -> image_buf;
    uint8_t *cluster_info = disk_info -> cluster_info;
    struct bpb33 *bpb = disk_info -> bpb;
    uint16_t cluster = getushort(dirent->deStartCluster);

    // Temporary fix. Will remove
    uint8_t anomaly_flag = CLUSTER_ZEROMASK;
    
    uint32_t cluster_count = 0;  // Has at least one cluster for beginning


    if (cluster == 0) {
        // The file is empty
        anomaly_flag |= CLUSTER_NULL;
    } else do {
        cluster_count ++;

        cluster_info[cluster] |= CLUSTER_POINTED;
        uint16_t next_cluster = get_fat_entry(cluster, image_buf, bpb);

        // Check and mark pointer flag
        if (num_of_cluster > cluster_count && is_end_of_file(next_cluster)) {
            // The file shouldn't end here
            cluster_info[cluster] |= CLUSTER_LESS;
            anomaly_flag |= CLUSTER_LESS;
            break;
        }
        if ( (!is_end_of_file(next_cluster)) && (!is_valid_cluster(next_cluster, bpb)) ) {
            // Points to invalid cluster
            cluster_info[cluster] |= CLUSTER_DEAD;
            anomaly_flag |= CLUSTER_DEAD;
            break;
        }
        if ( cluster_info[next_cluster] & CLUSTER_POINTED ) {
            // Points to a previously pointed cluster
            cluster_info[cluster] |= CLUSTER_DUPE;
            anomaly_flag |= CLUSTER_DUPE;
            break;
        }
        cluster = next_cluster;
    } while (!is_end_of_file(cluster));

    if (num_of_cluster < cluster_count) {
        anomaly_flag |= CLUSTER_MORE;
    }

    print_indent(indent);
    printf("Actual number of clusters occupied is: %d\n", cluster_count);

    if ( anomaly_flag & CLUSTER_NULL ) {
        print_indent(indent);
        printf("** Warning: The file is empty **\n");
    }
    if ( anomaly_flag & CLUSTER_LESS ) {
        print_indent(indent);
        printf("** Warning: Less data exists than expected **\n");
    }
    if ( anomaly_flag & CLUSTER_MORE ) {
        print_indent(indent);
        printf("** Warning: More data exists than expected **\n");
    }
    if ( anomaly_flag & CLUSTER_DEAD ) {
        print_indent(indent);
        printf("** Invalid cluster end found: pointing to nonexistent cluster **\n");
    }
    if ( anomaly_flag & CLUSTER_DUPE ) {
        print_indent(indent);
        printf("** Invalid cluster end found: duplicated pointing to cluster **\n");
    } 

    struct corruption_info *new_info = NULL;
    /*
    if ((anomaly_flag & (CLUSTER_ALLMASK ^ CLUSTER_NULL)) != CLUSTER_ZEROMASK) {
        new_info = malloc(sizeof(struct corruption_info));
        new_info -> file = dirent;
        new_info -> next = NULL;
        new_info -> anomaly_flag = anomaly_flag;

        add_corr_entry(disk_info, new_info);
    }
*/
    return new_info;
}

void check_free_cluster(struct disk_info *disk_info) {
    uint8_t *cluster_info = disk_info -> cluster_info;
    uint8_t *image_buf = disk_info -> image_buf;
    struct bpb33 *bpb = disk_info -> bpb;
    // Assumes cluster_info is clean and pristine
    uint16_t cluster = 0;
    for (int i = 2; i < bpb -> bpbSectors; i++) {
        cluster = get_fat_entry(i, image_buf, bpb);
        if (cluster != CLUST_FREE) {
        // Check for free cluster            
            (cluster_info[i]) |= CLUSTER_USED;
        }
    }

}

void validify_cluster_info(uint8_t *cluster_info, int size) {
    // Check if file is pointed to but free
    // or not pointed to but used
    for (int i = 2; i < size; i++) {
        uint8_t value = cluster_info[i];
        /*
        if (value & CLUSTER_BAD) {
            printf("Cluster %d is marked as a bad cluster\n", i);
            if (value & CLUSTER_USED) {
                printf("Cluster %d is pointed to but is a bad cluster\n", i);
            }
        }*/
        if (value & CLUSTER_POINTED) {
            if (!(value & CLUSTER_USED)) {
                printf("Cluster %d is free but pointed to.\n", i);
            }
        }
        if (value & CLUSTER_USED) {
            if (!(value & CLUSTER_POINTED)) {
                printf("Cluster %d is used but not pointed to.\n", i);
            }
        }
    }
}

void fix_corruption(struct disk_info *disk_info) {
    // Print list of FAT entries error 
    validify_cluster_info(disk_info -> cluster_info, disk_info -> bpb -> bpbSectors);

    // Print files error
    struct corruption_info *info = disk_info -> corr_info;
    while (info != NULL) {
        char fullname[12];
        get_file_name(info -> file, fullname);
        printf("File inconsistency: %s \n", fullname);

        info = info -> next;
    }
    printf("Printing of error completes\n");

    // Freeing the malloc
    info = disk_info -> corr_info;
    struct corruption_info *next = NULL;
    while (info != NULL) {
        next = info -> next;
        free(info);
        info = next;
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

    int num_cluster = bpb -> bpbSectors;

    // Array to keep track of cluster info
    uint8_t cluster_info[num_cluster];
    for (int i = 0; i < num_cluster; i++) {
        cluster_info[i] = CLUSTER_ZEROMASK;
    }
    
    // Putting the general info together in one struct
    struct disk_info disk_info;
    disk_info.image_buf = image_buf;
    disk_info.bpb = bpb;
    disk_info.cluster_info = cluster_info;
    disk_info.corr_info = NULL;

    check_free_cluster(&disk_info); 
    dos_ls(&disk_info);
    

    printf("==================\n");
    printf("Scanning complete\n");

    fix_corruption(&disk_info);
    unmmap_file(image_buf, &fd);
    return 0;
}
