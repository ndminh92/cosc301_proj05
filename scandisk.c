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

/*
 * COSC 301 Project 5
 * Sak Lee and Dang Minh Nguyen
 * We pair program most of the code. Sak designed the bit masking.
 */
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
/* write the values into a directory entry */
void write_dirent(struct direntry *dirent, char *filename, 
		  uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++) 
    {
	if (p2[i] == '/' || p2[i] == '\\') 
	{
	    uppername = p2+i+1;
	}
    }

    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++) 
    {
	uppername[i] = toupper(uppername[i]);
    }

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL) 
    {
	fprintf(stderr, "No filename extension given - defaulting to .___\n");
    }
    else 
    {
	*p = '\0';
	p++;
	len = strlen(p);
	if (len > 3) len = 3;
	memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8) 
    {
	uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);

    /* could also set time and date here if we really
       cared... */
}


/* create_dirent finds a free slot in the directory, and write the
   directory entry */

void create_dirent(struct direntry *dirent, char *filename, 
		   uint16_t start_cluster, uint32_t size,
		   uint8_t *image_buf, struct bpb33* bpb)
{
    while (1) 
    {
	if (dirent->deName[0] == SLOT_EMPTY) 
	{
	    /* we found an empty slot at the end of the directory */
	    write_dirent(dirent, filename, start_cluster, size);
	    dirent++;

	    /* make sure the next dirent is set to be empty, just in
	       case it wasn't before */
	    memset((uint8_t*)dirent, 0, sizeof(struct direntry));
	    dirent->deName[0] = SLOT_EMPTY;
	    return;
	}

	if (dirent->deName[0] == SLOT_DELETED) 
	{
	    /* we found a deleted entry - we can just overwrite it */
	    write_dirent(dirent, filename, start_cluster, size);
	    return;
	}
	dirent++;
    }
}

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
            printf("Cluster %d is a directory\n", file_cluster);
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
       
        uint16_t sectorSize = bpb -> bpbBytesPerSec;
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
    uint16_t bug = 1504;

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

        // Debug
        if (get_fat_entry(cluster, image_buf, bpb) == bug) {
            printf("Cluster %d points to 1504\n", cluster);
        }
        //

        // Check and mark pointer flag
        if (num_of_cluster > cluster_count && is_end_of_file(next_cluster)) {
            // The file shouldn't end here
            cluster_info[cluster] |= CLUSTER_LESS;
            anomaly_flag |= CLUSTER_LESS;
            break;
        }
        if (!is_end_of_file(next_cluster)) {
            if (!is_valid_cluster(next_cluster, bpb))  {
                // Points to invalid cluster
                cluster_info[cluster] |= CLUSTER_DEAD;
                anomaly_flag |= CLUSTER_DEAD;
                break;
            }
            /* The previous logic suddenly stopped working because
             * it did not stop at the end of the file, and thus
             * the end of the file cluster (filled with garbage)
             * resulted in GIGO situation with anomaly_flag
             */
            if ( (cluster_info[next_cluster] & CLUSTER_POINTED ) ) {
                // Points to a previously pointed cluster
                cluster_info[cluster] |= CLUSTER_DUPE;
                anomaly_flag |= CLUSTER_DUPE;
                break;
            }
        }
        cluster = next_cluster;
    } while ((!is_end_of_file(cluster)));

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
    if ((anomaly_flag & (CLUSTER_ALLMASK ^ CLUSTER_NULL)) != CLUSTER_ZEROMASK) {
        new_info = malloc(sizeof(struct corruption_info));
        new_info -> file = dirent;
        new_info -> next = NULL;
        new_info -> anomaly_flag = anomaly_flag;
        add_corr_entry(disk_info, new_info);
    }
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
        if (cluster == (FAT12_MASK & CLUST_BAD)) {
            cluster_info[i] |= CLUSTER_BAD;
        }

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
        if (value & CLUSTER_BAD) {
            if (value & CLUSTER_POINTED) {
                printf("Cluster %d is pointed to but is a bad cluster\n", i);
            } else {
                //printf("Cluster %d is an orphaned bad cluster\n", i);
            }
        } else {
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
}

void fix_corruption(struct disk_info *disk_info) {
    // Print list of FAT entries error 
    validify_cluster_info(disk_info -> cluster_info, disk_info -> bpb -> bpbSectors);
    
    uint8_t *image_buf = disk_info -> image_buf;
    struct bpb33 *bpb = disk_info -> bpb;
    uint8_t *cluster_info = disk_info -> cluster_info;
    uint16_t clusterSize = bpb -> bpbBytesPerSec * bpb -> bpbSecPerClust ;
    
    char fullname[15];
    // Print files error
    struct corruption_info *info = disk_info -> corr_info;
    while (info != NULL) {
        get_file_name(info -> file, fullname);
        printf("File inconsistency: %s \n", fullname);
        info = info -> next;
    }
    printf("==========\n");
    printf("End of error messages\n");

    // Fixing the errors
    info = disk_info -> corr_info;
    while (info != NULL) {
        struct direntry *dirent = info -> file;
        uint32_t size = getulong(dirent->deFileSize);
        
        uint32_t expected_cluster_num = (size + clusterSize - 1) / clusterSize;
        uint16_t start_cluster = getushort(dirent->deStartCluster);
        get_file_name(info -> file, fullname);


        // More cluster in FAT chain than file size
        if ((info -> anomaly_flag) & CLUSTER_MORE) {
            printf("Fixing %s : more cluster in FAT chain than file size indicate. Trimming\n", fullname);
            uint16_t cluster = start_cluster;
            uint32_t cluster_count = 1;
            while (cluster_count < expected_cluster_num) {
                cluster = get_fat_entry(cluster, image_buf, bpb);
                cluster_count++;
            }
            uint16_t next_cluster = get_fat_entry(cluster, image_buf, bpb);
            set_fat_entry(cluster, CLUST_EOFS & FAT12_MASK, disk_info -> image_buf, disk_info -> bpb);
            cluster = next_cluster;
            while (!is_end_of_file(cluster)) {
                if (cluster == (CLUST_BAD & FAT12_MASK)) {
                    break;
                }
                next_cluster = get_fat_entry(cluster, image_buf, bpb);
                cluster_info[cluster] &= CLUSTER_ALLMASK ^ CLUSTER_POINTED;
                cluster_info[cluster] &= CLUSTER_ALLMASK ^ CLUSTER_USED;
                set_fat_entry(cluster, CLUST_FREE & FAT12_MASK, disk_info -> image_buf, disk_info -> bpb);
                cluster = next_cluster;
            } 
            if (cluster != (CLUST_BAD & FAT12_MASK)) {
                cluster_info[cluster] &= CLUSTER_ALLMASK ^ CLUSTER_POINTED;
                cluster_info[cluster] &= CLUSTER_ALLMASK ^ CLUSTER_USED;
                set_fat_entry(cluster, CLUST_FREE & FAT12_MASK, disk_info -> image_buf, disk_info -> bpb);
            }
        }

        // Less cluster in FAT chain than file size
        // We change the file size to match the FAT chain
        if ((info -> anomaly_flag) & CLUSTER_LESS) {
            printf("Fixing %s : less cluster in FAT chain than file size indicate. Trimming\n", fullname);
            uint16_t cluster = start_cluster;
            uint32_t cluster_count = 0;
            while (!is_end_of_file(cluster)) {
                cluster_count++;
                cluster = get_fat_entry(cluster, image_buf, bpb);
            }
            size = cluster_count * clusterSize;
            printf("Cluster count is :%d\n", cluster_count);
            putulong(dirent -> deFileSize, size);
        }

        // We detect a bad cluster in the middle of a FAT chain
        // We assume things are linear, and check the cluster after the bad cluster
        // If it is pointed to (by some other chain), we will trim the file size
        // If it is marked as used but not pointed to, we assume it is from the 
        // current FAT chain, and try to follow it to the end. Adjust file size at the
        // end
        if ((info -> anomaly_flag) & CLUSTER_DEAD) {
            printf("Fixing %s : bad cluster detected. Trying to recover\n", fullname);
            uint16_t cluster = start_cluster;
            uint16_t next_cluster = get_fat_entry(cluster, image_buf, bpb);
            uint32_t cluster_count = 0;
            while (get_fat_entry(next_cluster, image_buf, bpb) != (CLUST_BAD & FAT12_MASK)) {
                cluster_count++;
                cluster = next_cluster;
                next_cluster = get_fat_entry(cluster, image_buf, bpb);
            } 
            // We know next_cluster is a bad cluster. 
            // So we try get_fat_entry(cluster) + 1
            next_cluster++;
            //printf("Current cluster is now %d\n", cluster);
            while (get_fat_entry(next_cluster, image_buf, bpb) == (CLUST_BAD & FAT12_MASK)) {
                next_cluster ++;     
            }
            //printf("Next cluster here is %d\n", next_cluster);
            if (!(cluster_info[next_cluster] & CLUSTER_POINTED)) {
                cluster_count++;
                set_fat_entry(cluster, next_cluster, image_buf, bpb);
                //printf("After this, cluster %d points to %d\n", cluster, get_fat_entry(cluster, image_buf, bpb));
                while (!is_end_of_file(next_cluster)) {
                    cluster_info[next_cluster] |= CLUSTER_POINTED;
                    cluster_count++;
                    next_cluster = get_fat_entry(next_cluster, image_buf, bpb);
                }

            } else {
                printf("Could not recover. Trimming file\n");
                set_fat_entry(cluster, CLUST_EOFS & FAT12_MASK, image_buf, bpb);
            }


            size = cluster_count * clusterSize;
            printf("Cluster count is :%d\n", cluster_count);
            putulong(dirent -> deFileSize, size);
        }


        // We detect a loop in the FAT chain
        // We go until the duplicate starts, and then make it EOF
        // and update the file size
        if ((info -> anomaly_flag) & CLUSTER_DUPE) {
            printf("Fixing %s : loop in chain detected. Cutting loop\n", fullname);
            uint16_t cluster = start_cluster;
            uint16_t next_cluster = get_fat_entry(cluster, image_buf, bpb);
            uint32_t cluster_count = 1;
            while (!(cluster_info[cluster] & CLUSTER_DUPE)) {
                cluster = get_fat_entry(cluster, image_buf, bpb);
                cluster_count ++;
            }
            set_fat_entry(cluster, CLUST_EOFS & FAT12_MASK, image_buf, bpb);


            size = cluster_count * clusterSize;
            printf("Cluster count is :%d\n", cluster_count);
            putulong(dirent -> deFileSize, size);
        }    
        info = info -> next;
    }

    // After fixing the files, we put each orphaned cluster into a new
    // file under the root directory
    int orphan_count = 0;
    for (int i = 2; i < bpb -> bpbSectors; i++) {
        uint16_t cluster = get_fat_entry(i, image_buf, bpb);
        if (cluster != (FAT12_MASK & CLUST_BAD)) {
            if ((cluster_info[i] & CLUSTER_USED) && (!(cluster_info[i] & CLUSTER_POINTED))) {
                set_fat_entry(i, CLUST_EOFS & FAT12_MASK, image_buf, bpb);
                orphan_count ++;
                fullname[0] = '\0';
                sprintf(fullname, "found%d.dat\0", orphan_count);
                printf("File name is: %s\n", fullname);
                struct direntry *root = (struct direntry *) root_dir_addr(image_buf, bpb);

                create_dirent(root, fullname, i, clusterSize, image_buf, bpb);


            }
        }
    }

    // We now fix all the pointed to but free sector
    for (int i = 2; i < bpb -> bpbSectors; i++) {
        uint16_t cluster = get_fat_entry(i, image_buf, bpb);
        if ((cluster == (CLUST_FREE)) && (cluster_info[i] & CLUSTER_POINTED)) {
            set_fat_entry(i, CLUST_EOFS & FAT12_MASK, image_buf, bpb);
        }
    }

    // Freeing memory used for corruption info
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
   
    uint16_t bug = 1504;
    for (uint16_t i = 2; i < num_cluster; i++) {
        if (get_fat_entry(i, image_buf, bpb) == bug) {
            printf("Entry %d is pointing to 1504\n", i);
        }
    }


    fix_corruption(&disk_info);
    
    for (uint16_t i = 2; i < num_cluster; i++) {
        if (get_fat_entry(i, image_buf, bpb) == bug) {
            printf("Entry %d is pointing to 1504\n", i);
        }
    }

    unmmap_file(image_buf, &fd);
    free(bpb);
    return 0;
}
