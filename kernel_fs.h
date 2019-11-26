#ifndef __KERNEL_FS_H
#define __KERNEL_FS_H

#include <stdint.h>

/* Determines the API */
typedef enum {
	FSE_FS,
	FSE_DIR,
	FSE_FILE,
	FSE_DEV
} Fse_type;

#define MAX_NAME_LENGTH 31

struct dir_entry;
struct fs_element;

typedef uintptr_t fse_id;
typedef struct fs_element Inode;
typedef struct dir_entry dir_entry;

typedef struct {
	struct dir_entry* (*Lookup)(const char* name);
	int (*Link)(const char* name, fse_id id);
	int (*Unlink)(const char* name);
	int (*ReadDir)(dir_entry* dlist, unsigned int n);
} DIR_ops;


typedef struct {
	int (*Truncate)(void* this, intptr_t size);
	intptr_t (*Seek)(void* this, intptr_t offset, int whence);
    int (*Read)(void* this, char *buf, unsigned int size);
    int (*Write)(void* this, const char* buf, unsigned int size);
    int (*Close)(void* this);
} FILE_ops;



typedef struct {
	Inode* (*Open)(fse_id id);
	void (*Sync)();

} FS_ops;


typedef struct dir_entry
{
	char name[MAX_NAME_LENGTH+1];
	fse_id id;
} dir_entry;


/* File system element */
typedef struct fs_element
{
	Fse_type type;
} Inode;





#endif


