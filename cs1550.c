/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

// Index at which to start allocating new directories
// and files in the bitmap
#define START_ALLOC_INDEX 2

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;


typedef struct cs1550_directory_entry cs1550_directory_entry;

#define MAX_MAP_ENTRIES (BLOCK_SIZE/sizeof(short))


// Struct to store blocks from the fille allocation table
struct cs1550_bitmap_block {
	short table[MAX_MAP_ENTRIES];
};

typedef struct cs1550_bitmap_block cs1550_bitmap;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct cs1550_disk_block
{
	//All of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;

// Function to open up .disk and read in the
// bitmap from it
static cs1550_bitmap get_bitmap() {
	// Open the .disk file
	FILE* disk = fopen(".disk", "r+b");
	// Seek to position block_size in disk
	fseek(disk, BLOCK_SIZE, SEEK_SET);
	// Initialize bitmap block and read it in
	cs1550_bitmap bitmap;
	fread(&bitmap, BLOCK_SIZE, 1, disk);
	return bitmap;
}

// Write new bitmap data to .disk
static void write_new_bitmap(cs1550_bitmap* bitmap) {
	// Open disk
	FILE* disk = fopen(".disk", "r+b");
	// Seek to position BLOCK_SIZE
	fseek(disk, BLOCK_SIZE, SEEK_SET);
	// Write the bitmap to .disk
	// and close it
	fwrite(bitmap, BLOCK_SIZE, 1, disk);
	fclose(disk);
}

// Get root from the .disk file
static cs1550_root_directory get_root_dir() {
	// Open up the .disk file
	FILE* disk_file = fopen(".disk", "r+b");
	// Set the file position at 0
	fseek(disk_file, 0, SEEK_SET);
	// Create a root_directory and read the .disk
	// file into it
	cs1550_root_directory root_dir;
	fread(&root_dir, BLOCK_SIZE, 1, disk_file);
	// Return the root_directory
	return root_dir;
}

//Write the root data from a given pointer to disk at block 0
// Write new root directory information to .disk
// a position BLOCK_SIZE
static void write_new_root(cs1550_root_directory* root_on_disk) {
	// Open disk
	FILE* disk = fopen(".disk", "r+b");
	// Write root to disk
	fwrite(root_on_disk, BLOCK_SIZE, 1, disk);
	fclose(disk);
}


/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not. 
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;
		
	// Variables to store path
	char dir[MAX_FILENAME + 1];
	char file_name[MAX_FILENAME + 1];
	char ext[MAX_EXTENSION + 1];

	//Check if path is root
	if(strcmp(path, "/") != 0) {
		// If it is not, then get the current path
		sscanf(path, "/%[^/]/%[^.].%s", dir, file_name, ext);
	} else {
		// If it is set the directory mode to 
		// directory
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		return res;
	}
	
	// Check if the directory is empty
	if(strcmp(dir, "") == 0){ 
		// If the directory is empty then 
		// the file cannot be found
		res = -ENOENT;
		return res;
	} 

	// Clear stat buffer
	memset(stbuf, 0, sizeof(struct stat));

	// Since we're not in root, we need to find the correct 
	// directory
	int i = 0;
	struct cs1550_directory placeholder;
	// Initialize  dname
	strcpy(placeholder.dname, "");
	placeholder.nStartBlock = -1;
	// Initialize the root directory
	cs1550_root_directory root_directory = get_root_dir();

	// Need to iterate through the root's directories 
	// to find the one we want
	for(i = 0; i < root_directory.nDirectories; i++) { 
		// Check if the current directory matches the search directory
		if(strcmp(root_directory.directories[i].dname, dir) == 0) {
			// If this matches the one we want then set it as the current directory
			placeholder = root_directory.directories[i];
			break;
		}
	}

	// Check if the directory was found
	if(strcmp(placeholder.dname, "") == 0) {
		// It was not so return ENOENT error
		res = -ENOENT;
		return res;
	}

	// No where left to go
	if(strcmp(file_name, "") == 0) {
		// Return a success and
		// the appropriate permissions
		res = 0;
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		return res; 
	}

	// Open .disk and get the file location
	FILE* disk = fopen(".disk", "r+b");
	int disk_location = BLOCK_SIZE * placeholder.nStartBlock;
	// Set file position using the location
	fseek(disk, disk_location, SEEK_SET);
	cs1550_directory_entry entry;
	entry.nFiles = 0;
	memset(entry.files, 0, MAX_FILES_IN_DIR*sizeof(struct cs1550_file_directory));

	// Read in the directory and all of its data including
	// any files inside 
	int items_read = fread(&entry, BLOCK_SIZE, 1, disk);
	fclose(disk);
	
	// Check if a single block was read
	if(items_read == 1) {
		// If it was we need to find the file in the directory
		struct cs1550_file_directory filedir;
		strcpy(filedir.fext, "");
		strcpy(filedir.fname, "");
		filedir.fsize = 0;
		filedir.nStartBlock = -1;
				
		int i = 0;
		// Need to iterate over the filed in the directory to
		// find the one we want
		for(i = 0; i < MAX_FILES_IN_DIR; i++) {
			// Check if the current file name and extension match
			// the one we want.
			if(strcmp(entry.files[i].fname, file_name) == 0 && strcmp(entry.files[i].fext, ext) == 0) { 
				// It does, so set it to the file struct
				filedir = entry.files[i];
				break;
			}
		}

		// Check if a file was found by checking the starting block
		if((filedir.nStartBlock) == -1) { 
			// It was not found so return ENOENT error
			res = -ENOENT;
			return res;
		} else {
			// File was found, so return success
			res = 0;
			stbuf->st_mode = S_IFREG | 0666;
			stbuf->st_nlink = 1;
			stbuf->st_size = filedir.fsize;
			return res;
		}
	}
	return res;
}

/* 
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;

	//This line assumes we have no subdirectories, need to change
	if (strcmp(path, "/") != 0) {
		return -ENOENT;
	}

	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	// Variable to store the destination
	char* dest = strtok(path, "/");

	// Check if the current path is root
	if(strcmp(path, "/") == 0) {
		int i = 0;
		
		cs1550_root_directory root_dir = get_root_dir();

		// Check all directories in root
		for(i = 0; i < MAX_DIRS_IN_ROOT; i++){ 
			// Check if the current directory is empty
			if(strcmp(root_dir.directories[i].dname, "") != 0) {
				// If it does, print it
				filler(buf, root_dir.directories[i].dname, NULL, 0);
			}
		}
		return 0;
	} else {
		// Varaible to store directory for
		// checking files
		struct cs1550_directory directory;
		strcpy(directory.dname, "");
		directory.nStartBlock = -1;

		cs1550_root_directory root_dir = get_root_dir();

		// Check all directories in root 
		int i = 0;
		for(i = 0; i < MAX_DIRS_IN_ROOT; i++) {
			// Check if the current directory is the one we want
			if(strcmp(dest, root_dir.directories[i].dname) == 0) {
				// It is, so set dir to the current directory and break
				directory = root_dir.directories[i];
				break;
			}
		}

		// Check if no directory was found
		if(strcmp(directory.dname, "") == 0) {
			// It was not, so send an error back
			return -ENOENT;
		} else {
			// The proper directory has been found
			// Load disk file to read over directory 
			FILE* disk = fopen(".disk", "rb+");
			int disk_location = directory.nStartBlock * BLOCK_SIZE;
			fseek(disk, disk_location, SEEK_SET);

			cs1550_directory_entry entry;
			entry.nFiles = 0;
			memset(entry.files, 0, MAX_FILES_IN_DIR*sizeof(struct cs1550_file_directory));

			// Read in the data from .disk to
			// entry for use later
			fread(&entry, BLOCK_SIZE, 1, disk); 
			fclose(disk);

			i = 0;
			// Loop over the files in the directory entry and print them out 
			for(i = 0; i < MAX_FILES_IN_DIR; i++) {
				// Variable to store the current  
				struct cs1550_file_directory curr_file_dir = entry.files[i];
				// Variable to store the file name
				char full_file_name[MAX_FILENAME+1];
				strcpy(full_file_name, curr_file_dir.fname);
				// Check if the file has an extension
				if(strcmp(curr_file_dir.fext, "") != 0) {
					// If so, append it
					strcat(full_file_name, ".");
					strcat(full_file_name, curr_file_dir.fext);
				}
				// Check if the file is empty
				if(strcmp(curr_file_dir.fname, "") != 0){
					// If it isn't, print it
					filler(buf, full_file_name, NULL, 0);
				}
			}
		}
	}
	return 0;
}

/* 
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{
	(void) path;
	(void) mode;
	// Variables to store the directory
	// and its subdirectory
	char* dir;
	char* sub;	

	// Derived from the path
	dir = strtok(path, "/");
	sub = strtok(NULL, "/");

	// Check if the directory name is too big
	if(strlen(dir) > MAX_FILENAME) {
		// If it is, return an error
		return -ENAMETOOLONG;
	} else if(sub) {
		// The user cannot pass in a subdirectory
		// If they do, deny permission
		return -EPERM;
	}

	// Variables to store the root directory and
	// the bitmap
	cs1550_root_directory root_directory = get_root_dir();
	cs1550_bitmap bitmap = get_bitmap();

	int i = 0;
	// Go through every directory in the root
	for(i = 0; i < MAX_DIRS_IN_ROOT; i++) {
		// Check if the directory the user wants to
		// create already exists
		if(strcmp(root_directory.directories[i].dname, dir) == 0) {
			// If it does, return an error
			return -EEXIST;
		}
	}

	i = 0;
	// Go through every directory in root
	for(i = 0; i < MAX_DIRS_IN_ROOT; i++) {
		// Check if the current directory is nameless
		if(strcmp(root_directory.directories[i].dname, "") == 0) {
			// If the directory has no name then we can user it
			// to store the new directory
			struct cs1550_directory new_dir;
			// Add the users directory name to the 
			// new directory's name
			strcpy(new_dir.dname, dir);
			
			int j = 0;
			// Go through every member of the bitmap
			for(j = START_ALLOC_INDEX; j < MAX_MAP_ENTRIES; j++){ //Iterate over the bitmap to find a new block to store the directory in
				// Check if the current index has anything allocated
				// to it
				if(bitmap.table[j] == 0) {
					// It doesn't so add to it
					bitmap.table[j] = EOF;
					// Record index at which file is contained
					new_dir.nStartBlock = j;
					break;
				}
			}
			
			// Open up .disk to write the new directory to it
			FILE* disk = fopen(".disk", "r+b");
			int disk_location = BLOCK_SIZE * new_dir.nStartBlock;
			fseek(disk, disk_location, SEEK_SET);
			cs1550_directory_entry dir;
			dir.nFiles = 0;
			
			// Read the directory and all of its data
			int items_read = fread(&dir, BLOCK_SIZE, 1, disk);
			
			// Check if a single block was read
			if(items_read == 1) {
				memset(&dir, 0, sizeof(struct cs1550_directory_entry));
				 // Write the new directory to the disk file
				fwrite(&dir, BLOCK_SIZE, 1, disk);
				fclose(disk);

				// Update root with an new directory
				root_directory.nDirectories++;
				root_directory.directories[i] = new_dir;				

				// Write the new root directory and the new
				// bitmap to .disk
				write_new_root(&root_directory);
				write_new_bitmap(&bitmap);
			} else {
				// Error with the disk, close the file 
				fclose(disk);
			}
			return 0;
		}
	}
	return 0;
}

/* 
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
    return 0;
}

/* 
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;
	
	//path will be in the format of /directory/sub_directory
	// Variables to store the directory, file name
	// and file extension
	char* dir; //The first directory in the 2-level file system
	char* file_name; //The directory within the root's directory	
	char* ext;

	dir = strtok(path, "/");
	file_name = strtok(NULL, ".");
	ext = strtok(NULL, ".");

	// Check if directory is not empty
	if((dir && dir[0]) && strcmp(dir, "") != 0) {
		// Check if file name is file name is empty
		if(file_name && file_name[0] && strcmp(file_name, "") == 0) {
				// If it is, return error
				return -EPERM;
		}

		// Check if file extension is null
		if(ext && ext[0]) {
			// If neither file extension and the file name are 
			// null, then check if they are too long
			if(strlen(file_name) > MAX_FILENAME || strlen(ext) > MAX_EXTENSION) {
				// If they are return an error
				return -ENAMETOOLONG;
			} else {
				// If file name is not empty but file
				// extension is empty, then check if
				// the file name is too long
				if(strlen(file_name) > MAX_FILENAME) {
					return -ENAMETOOLONG;
				} else { 
					// File name is null or empty, so return
					// an error
					return -EPERM;
				}
			}
		}

		// Get data for both root directory and bitmap 
		// and store it
		cs1550_root_directory root_dir = get_root_dir();
		cs1550_bitmap bitmap = get_bitmap();

		// Variable store the desired directory
		struct cs1550_directory directory_to_add_to;

		int i = 0;
		// Go through the directories in root
		for(i = 0; i < MAX_DIRS_IN_ROOT; i++) {
			// Check if the directory we are looking for
			// is the same as teh current directory
			if(strcmp(dir, root_dir.directories[i].dname) == 0) {
				// If it is, grab it and store it
				directory_to_add_to = root_dir.directories[i];
				break;
			}
		}

		// Check if the directory was found
		if(strcmp(directory_to_add_to.dname, "") != 0) {
			// bitmap the location of the file to add to
			// find it in the .disk file
			long disk_location = BLOCK_SIZE * directory_to_add_to.nStartBlock;
			// Open .disk file and seek to 
			// location disk_location
			FILE* disk = fopen(".disk", "r+b");
			fseek(disk, disk_location, SEEK_SET);
			
			cs1550_directory_entry entry;
			int items_read = fread(&entry, BLOCK_SIZE, 1, disk);

			// Check if any items were read
			if(items_read) {
				int file_exists = 0;
				int first_free_index = -1;

				int j = 0;
				// Check over every file in the directory
				for(j = 0; j < MAX_FILES_IN_DIR; j++) {
					// Check if the current file is empty, and store that index to use later
					if(strcmp(entry.files[j].fname, "") == 0 && strcmp(entry.files[j].fext, "") == 0 && first_free_index == -1) {
						first_free_index = j;
					}

					// Check if the file with the same name and extension already exists in the directory
					if(strcmp(entry.files[j].fname, file_name) == 0 && strcmp(entry.files[j].fext, ext) == 0) {
						// Set out boolean since it does.
						file_exists = 1;
						break;
					}			
				}

				// Check if the file exists
				if(!file_exists) {
					// It does not, so we're good to create it
					// Varaible to store the start index for the bitmap
					short bitmap_start_index = -1;

					// Go through and check every member of the bitmap
					int k = 0;
					for(k = 2; k < MAX_MAP_ENTRIES; k++) {
						// If the current index is empty
						// then store the start index
						// and mark the index
						if(bitmap.table[k] == 0) {
							bitmap_start_index = k;
							bitmap.table[k] = EOF;
							break;
						}
					}


					// Variable to store the new file
					struct cs1550_file_directory new_file;
					// Copy over the file name that we want to create
					strcpy(new_file.fname, file_name);
					// Check if there is a file extension
					if(ext && ext[0]) {
						// If there is a file extension, add it to the file
						strcpy(new_file.fext, ext);
					} else {
						// If there is no file extension, fill
						// the fext field with an empty string
						strcpy(new_file.fext, "");
					}
					// Initialize new file fields
					new_file.fsize = 0;
					new_file.nStartBlock = bitmap_start_index;

					// Use saved index to store teh new file
					entry.files[first_free_index] = new_file;
					// Increase number files in the directory
					entry.nFiles++; 

					// Seek to the index we need to store
					// the new file at
					fseek(disk, disk_location, SEEK_SET);
					// Write the changed directory to .disk
					fwrite(&entry, BLOCK_SIZE, 1, disk);
					fclose(disk);
					
					//Write the root and bitmap back to disk
					write_new_root(&root_dir);
					write_new_bitmap(&bitmap);
				} else { 
					// File already exists, so return
					// a permissions error
					fclose(disk);
					return -EEXIST;
				}
			} else {
				// Directory does not have a name so no file can be added
				fclose(disk);
				return -EPERM;
			}
		} else {
			// Directory was null or empty, so
			// just return
			return 0;
		}		
	}
	return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
}

/* 
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//set size and return, or error

	//size = 0;
	// Variables to store the path, file, and extension
	char* dir;
	char* file_name;	
	char* ext;	


	// Parse path using strtok
	dir = strtok(path, "/");
	file_name = strtok(NULL, ".");
	ext = strtok(NULL, ".");

	// Check if we were given an empty directory
	if((dir && dir[0]) && strcmp(dir, "") != 0) {
		//Read in the root and bitmap
		// Variables to store root directory and file
		// allocation table
		cs1550_root_directory root_dir = get_root_dir();
		cs1550_bitmap bitmap = get_bitmap();

		struct cs1550_directory directory;

		int i = 0;
		// Go through every directory in root
		for(i = 0; i < MAX_DIRS_IN_ROOT; i++) {
			
			// Check if the current directory is the one
			// we want
			if(strcmp(dir, root_dir.directories[i].dname) == 0) {
				// If it is, then store it to be used later
				directory = root_dir.directories[i];
				break;
			}
		}

		// Check if the directory that was found has a name
		if(strcmp(directory.dname, "") != 0) {
			// Read in disk file and caluclate
			// offset at whcih to fine the block
			// we need
			long disk_location = BLOCK_SIZE * directory.nStartBlock;
			FILE* disk = fopen(".disk", "r+b");
			fseek(disk, disk_location, SEEK_SET);
			
			// Variable to store the directory we
			// want from the .disk file
			cs1550_directory_entry entry;
			int items_read = fread(&entry, BLOCK_SIZE, 1, disk);
			fclose(disk);

			// Check if anything was read in
			if(items_read) {
				// If it was it's time to find the file we want
				struct cs1550_file_directory file_directory;

				int j = 0;
				// Search the files int eh directory until we
				// find thee one we want
				for(j = 0; j < MAX_FILES_IN_DIR; j++) {

					// Check if the current file 
					// matches the name of the one we 
					// want to read
					if(strcmp(entry.files[j].fname, file_name) == 0) {
						// If it is, then we need to
						// check the extension on the current
						// file to see if that also matches
						if(ext && ext[0]) {
							// If the extension for the 
							// file we want is not empty
							// then check if the current file
							// matches it
							if(strcmp(entry.files[j].fext, ext) == 0) {
								// The extension matches, so this is
								// the file that we want to read
								file_directory = entry.files[j];
								break;
							}
						} else {
							// The extension we want is empty, so
							// we need to check if the current file
							// extension is also empty
							if(strcmp(entry.files[j].fext, "") == 0) {
								// It matches, so store the file
								// to be read
								file_directory = entry.files[j];
								break;
							}
						}
					}
				}

				// Check if the file we found has
				// no name
				if(strcmp(file_directory.fname, "") != 0) {
					// We need to find the right offset to read from
					int block_num = 0;
					if(offset != 0) {
						block_num = offset/BLOCK_SIZE;
					} 
					int block_offset = 0;
					if(offset != 0) {
						block_offset = offset - (block_num * BLOCK_SIZE);
					} 

					// Get the current block we're using
					// and iterate through the bitmap
					// until we get the one we want
					int block = file_directory.nStartBlock;
					if(block_num != 0) {
						while(block_num > 0){
							block = bitmap.table[block];
							block_num--;
						}
					}

					// Open .disk to read the data from the blocks of the file
					FILE* disk = fopen(".disk", "r+b");
					fseek(disk, (BLOCK_SIZE * block) + block_offset, SEEK_SET);
					cs1550_disk_block read_block;
					fread(&read_block.data, BLOCK_SIZE - block_offset, 1, disk);
					int buf_size = 0;

					// Add the new block's data to the buffer
					if(file_directory.fsize >= BLOCK_SIZE) {
						memcpy(buf, &read_block.data, BLOCK_SIZE - block_offset);
					} else {
						memcpy(buf, &read_block.data, file_directory.fsize);
					}

					// Change size of the buffer
					buf_size = BLOCK_SIZE - block_offset;

					//While this file hasn't ended yet and there are still more block sto read in, repeat the above procedure and keep iterating through the blocks of the file until EOF is reached
					// Go through every block in the allocation table
					// until we reach the end of the file
					while(bitmap.table[block] != EOF){
						// Get the current block
						block = bitmap.table[block];
						// Variable to store current block
						// in buffer
						cs1550_disk_block block_data;
						// Seek to the space reserved for this block 
						// in the .disk file
						fseek(disk, (BLOCK_SIZE * block), SEEK_SET);
						// Read in the block's data
						fread(&block_data.data, BLOCK_SIZE, 1, disk);
						memcpy(buf + buf_size, &block_data, strlen(block_data.data));
						// Increase buffer size
						buf_size += strlen(block_data.data);
					}
					fclose(disk);

					// Write new root directory and new
					// bitmap back to .disk
					write_new_root(&root_dir);
					write_new_bitmap(&bitmap);
					size = buf_size;
				} else { 
					// No filename provided and we
					// can't read from a directory
					return -EISDIR;
				}
			}
		} else {
			// Directory name was empty or null,
			// so just return
			return 0;
		}	
	}
	return size;
}

/* 
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size, 
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//path will be in the format of /directory/sub_directory
	char* dir; //The first directory in the 2-level file system
	char* file_name; //The directory within the root's directory	
	char* ext;	

	dir = strtok(path, "/");
	file_name = strtok(NULL, ".");
	ext = strtok(NULL, ".");

	// Check if we were given an empty directory
	if((dir && dir[0]) && strcmp(dir, "") != 0) { 
		// Variables to store the root directory
		// and to store the bitmap
		cs1550_root_directory root_dir = get_root_dir();
		cs1550_bitmap bitmap = get_bitmap();

		struct cs1550_directory directory;

		int i = 0;
		// Go through all the entries in root directory 
		for(i = 0; i < MAX_DIRS_IN_ROOT; i++) {
			// Check if the current directory is
			// the one we want
			if(strcmp(dir, root_dir.directories[i].dname) == 0) {
				// If it is, store it to be used
				directory = root_dir.directories[i];
				break;
			}
		}

		// Check if a directory was found
		if(strcmp(directory.dname, "") != 0) {
			// Get directory location to find it in .disk
			long disk_location = BLOCK_SIZE * directory.nStartBlock;

			// Open up disk file and set our file position
			// based on the calculated disk location
			FILE* disk = fopen(".disk", "r+b");
			fseek(disk, disk_location, SEEK_SET);	
			// Variable to store our directory
			// that we got from .disk
			cs1550_directory_entry entry;
			int num_items = fread(&entry, BLOCK_SIZE, 1, disk);
			fclose(disk);

			// Check if a block was read
			if(num_items) {
				// If it was we can begin the long and arudous
				// project of writing to it
				struct cs1550_file_directory file_directory;
				int file_index = -1;

				int j = 0;
				// Need to go through every file in the directory
				for(j = 0; j < MAX_FILES_IN_DIR; j++) {

					// Check if the current file matches the one we want
					if(strcmp(entry.files[j].fname, file_name) == 0) {
						// Check if the file extension is empty
						if(ext && ext[0]){ //Check whether the file we're looking for has a file extension or not; if true, it does
							// If it is not empty then check if the 
							// extension matches the one we're looking for
							if(strcmp(entry.files[j].fext, ext) == 0) {
								// Store the file directory and it's location 
								// because we've found a match
								file_directory = entry.files[j];
								file_index = j;
								break;
							}
						} else {
							// The file we're looking for does not have an extension
							// So now check if the current file has a blank extension
							if(strcmp(entry.files[j].fext, "") == 0) {
								// Store the file directory and it's location 
								// because we've found a match
								file_directory = entry.files[j];
								file_index = j;
								break;
							}
						}
					}
				}

				// Check whether the file name is empty
				if(strcmp(file_directory.fname, "") != 0) {
					// Check if the offset is bigger than our file size
					if(offset > file_directory.fsize) {
						// It is, so return an error
						return -EFBIG;
					}
					
					// Get the buffer size and then get
					// the number of bytes we need until we can 
					// append to the file
					int buf_size = strlen(buf);
					int bytes_to_append = file_directory.fsize - offset;
					
					// We need to figure out what blocks we have to skip 
					// in order to write to the desired location
					int block_num = 0;
					if(offset != 0) {
						block_num = offset/BLOCK_SIZE;
					} 
					int block_offset = 0;
					if(offset != 0) {
						block_offset = offset - (block_num * BLOCK_SIZE);
					} 

					// Need to move through the map until
					// we find the correct starting block
					int block = file_directory.nStartBlock;
					if(block_num != 0){
						while(block_num > 0){
							block = bitmap.table[block];
							block_num--;
						}
					}

					// Bytes needed to write to disk
					int bytes_left = buf_size;

					//Open the disk and write the correct about of buffer data in the first block; this basically gets rid of the offset so we can then start writing entire Blocks at a time later
					// Open .disk and seek to the correct location
					FILE* disk = fopen(".disk", "r+b");
					fseek(disk, (BLOCK_SIZE * block) + block_offset, SEEK_SET);
					// Check if the buffer has more in it
					// than a block will allow 
					if(buf_size >= BLOCK_SIZE) { //This means there will be left over stuff in the buffer that we have to write after we finish writing to this block
						// Write the data in buffer to disk
						fwrite(buf, BLOCK_SIZE - block_offset, 1, disk);
						// Subtract from our btes left now
						// that we've written a few things
						bytes_left -= (BLOCK_SIZE - block_offset);
						if(offset == size) {
							// Expand file size based on the offset
							file_directory.fsize = offset+1;
						}
					} else { 
						// We aren't overflowing, so write this 
						// to .disk, lets get this bread!
						fwrite(buf, buf_size, 1, disk);

						// Need to pad to our remaining spaces						
						char pad[BLOCK_SIZE-buf_size];
						int h = 0;
						// Fill a pad array with  null terminators
						for(h = 0; h < BLOCK_SIZE-buf_size; h++){
							pad[h] = '\0';
						}
						// Write our pad to the file
						fwrite(pad, BLOCK_SIZE-buf_size, 1, disk);
						bytes_left -= buf_size;

						// Check if fsize is greater than
						// size
						if(file_directory.fsize > size) {
							if(offset == size){
								// Expand our file size based on 
								// our offset
								file_directory.fsize = offset+1;
							} else {
								// Set fsize = size
								file_directory.fsize = size;
							}
						}
					}		

					// If there are any bytes left
					// then we need to continue writing
					while(bytes_left > 0) { 
						
						// Check if we have room in the file
						if(bitmap.table[block] == EOF) {
							int k = 2;
							// Search through our bitmap
							for(k = 2; k < MAX_MAP_ENTRIES; k++) { //We need to allocate another block for this file to write more bytes; find a new block in the bitmap
								// Need to allocate another block for this file
								// so we need to find a block to write too
								// Check if the current block is allocated
								if(bitmap.table[k] == 0) {
									// If our current block is unallocated,
									// then we need to allocate it
									bitmap.table[block] = k;
									bitmap.table[k] = EOF;
									block = k;
									break;
								}
							}
						} else { 
							// We still have room, so continue writing into the next block
							block = bitmap.table[block];
						}

						// Seek to a new position so we 
						// can write the new file stuff into
						// .disk
						fseek(disk, BLOCK_SIZE * block, SEEK_SET);
						// Check if there still is more data to write
						if(bytes_left >= BLOCK_SIZE) { 
							// Get address of new block
							char* new_addr = buf + (buf_size - bytes_left);
							// Write to that address
							fwrite(new_addr, BLOCK_SIZE, 1, disk);
							// Update our bytes left
							bytes_left -= BLOCK_SIZE;
						} else { //This is our final write because we don't have anything left in the buffer to write; so just write it and finish
							// Nothing is left in the buffer that 
							// we need to write, so finish this
							// file out
							char* new_addr = buf + (buf_size - bytes_left);
							fwrite(new_addr, bytes_left, 1, disk);

							// All done!
							bytes_left = 0;
						}
					}
			
					// Figure out how many bytes have been appended out of the 
					// ones we need

					int appended_bytes = bytes_to_append - buf_size - bytes_left;
					if(appended_bytes > 0) {
						// if there still is stuff to append, 
						// alter file size accordingly
						file_directory.fsize += appended_bytes;
					}

					// Add our file to the list of 
					// files in the directory
					entry.files[file_index] = file_directory;
					// Seek to the appropriate
					// spot in .disk
					fseek(disk, directory.nStartBlock * BLOCK_SIZE, SEEK_SET);
					// Write our appended file directory
					// to .disk
					fwrite(&entry, BLOCK_SIZE, 1, disk);
					fclose(disk);

					write_new_root(&root_dir);
					write_new_bitmap(&bitmap);
					size = buf_size;
				}
			} 
		}
	}
	return size;
}

/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or 
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/* 
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but 
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file 
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
	.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
	.mknod	= cs1550_mknod,
	.unlink = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush = cs1550_flush,
	.open	= cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
