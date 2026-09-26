
#include "common.h"

#include <sbv_patches.h>
#include <loadfile.h>
#include <kernel.h>
#include <stdlib.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <libhdd.h>
#include <libpwroff.h>
#include <sifrpc.h>
#include <sys/fcntl.h>
#include <string.h>

//#define DPRINTF(x...) sio_printf(x)
#define DPRINTF(x...)
//#define DEBUG

static char padBuf_t[2][256] __attribute__((aligned(64)));

extern u8 iomanX_irx[];
extern int size_iomanX_irx;

extern u8 usbhdfsd_irx[];
extern int size_usbhdfsd_irx;

extern u8 usbd_irx[];
extern int size_usbd_irx;

extern u8 freesd_irx[];
extern int size_freesd_irx;

extern u8 audsrv_irx[];
extern int size_audsrv_irx;

extern u8 fileXio_irx[];
extern int size_fileXio_irx;

extern u8 ps2atad_irx[];
extern int size_ps2atad_irx;

extern u8 ps2fs_irx[];
extern int size_ps2fs_irx;

extern u8 ps2hdd_irx[];
extern int size_ps2hdd_irx;

extern u8 ps2dev9_irx[];
extern int size_ps2dev9_irx;

extern u8 poweroff_irx[];
extern int size_poweroff_irx;

extern u8 smscdvd_irx[];
extern int size_smscdvd_irx;

#ifndef HOST
extern u8 bdm_irx[];
extern int size_bdm_irx;

extern u8 bdmfs_vfat_irx[];
extern int size_bdmfs_vfat_irx;

extern u8 sio2man_irx[];
extern int size_sio2man_irx;

extern u8 mmceman_irx[];
extern int size_mmceman_irx;

extern u8 mx4sio_bd_irx[];
extern int size_mx4sio_bd_irx;
#endif

static int initdirs();
static int get_part_list();
static void load_hddmodules();
static int hddinit();
static int mountParty(char *party);
static void unmountAllParts();
static void unmountParty(int i);
static int fix_hddpath(char *name);
static int ensure_hdd(void);
static int is_pfs_dev(const char *s);
static int resolve_pfs_dir(char *path, int is_main);
static int parse_hdd0_pfs_argv(const char *argv, char *out);
#ifndef HOST
static int load_mmce_modules(void);
static int load_mx4sio_modules(void);
static int path_has_dev(const char *path, const char *dev);
#endif

static void load_modules()
{
#ifdef HOST
	/* PCSX2: keep the old rom0-then-iomanX order. Probing XSIO2MAN after
	 * an IOP reset can hang loadfile before init_video (black screen). */
	SifLoadModule("rom0:SIO2MAN", 0, NULL);
	SifLoadModule("rom0:MCMAN", 0, NULL);
	SifLoadModule("rom0:MCSERV", 0, NULL);
	SifLoadModule("rom0:PADMAN", 0, NULL);

	SifExecModuleBuffer(iomanX_irx, size_iomanX_irx, 0, NULL, NULL);
	SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, NULL);
#else
	/* Hardware: iomanX first so mc / mass / pfs all attach to it.
	 * mmceman only shares SIO2 when sio2man is library version 1.2 or 2.7.
	 * rom0:SIO2MAN is older, so the lock is skipped, the directory read
	 * collides with the pad, and the controller stops after the list.
	 * This embedded module is ps2sdk sio2man 2.7. MCMAN and PADMAN stay
	 * the ROM modules, which that sio2man still speaks. */
	SifExecModuleBuffer(iomanX_irx, size_iomanX_irx, 0, NULL, NULL);
	SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, NULL);

	SifExecModuleBuffer(sio2man_irx, size_sio2man_irx, 0, NULL, NULL);
	SifLoadModule("rom0:MCMAN", 0, NULL);
	SifLoadModule("rom0:MCSERV", 0, NULL);
	SifLoadModule("rom0:PADMAN", 0, NULL);
#endif

	SifExecModuleBuffer(smscdvd_irx, size_smscdvd_irx, 0, NULL, NULL);
	SifExecModuleBuffer(usbd_irx, size_usbd_irx, 0, NULL, NULL);
	SifExecModuleBuffer(usbhdfsd_irx, size_usbhdfsd_irx, 0, NULL, NULL);
	SifExecModuleBuffer(freesd_irx, size_freesd_irx, 0, NULL, NULL);
	SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, NULL);
}

void ps2delay(int count) 
{
   int i, ret;
   for (i = 0; i < count; i++) 
   {
      ret = 0x01000000;
      while ( ret-- ) 
      {
         asm("nop\nnop\nnop\nnop");
      }
   }
}
int ps2quit()
{
#ifdef HOST
	return 1;
#else
	/* Boot already reset the IOP and replaced LaunchELF's modules.
	 * Exit(0) tries to return to that dead parent: black screen, and
	 * the front reset button can stop responding. */
	pause_audio();
	unmountAllParts();

	padPortClose(0, 0);
	padPortClose(1, 0);
	padEnd();

	fileXioExit();

	while (!SifIopReset(NULL, 0)) {}
	while (!SifIopSync()) {}

	SifExitRpc();
	SifInitRpc(0);
	FlushCache(0);
	FlushCache(2);

	ExecOSD(0, NULL);
	LoadExecPS2("rom0:OSDSYS", 0, NULL);
	while (1) {}
	return 1;
#endif
}

#ifndef HOST
static int mmce_loaded = 0;
static int mx4sio_loaded = 0;
static char mx4sio_root[16];

static int path_has_dev(const char *path, const char *dev)
{
	int i;

	if (path == NULL || dev == NULL)
		return 0;

	for (i = 0; dev[i] != 0; i++)
	{
		char a = path[i];
		char b = dev[i];

		if (a == 0)
			return 0;
		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (b >= 'A' && b <= 'Z')
			b += 32;
		if (a != b)
			return 0;
	}
	return 1;
}

static int probe_mass_unit(int unit)
{
	char p[16];
	int fd;

	sprintf(p, "mass%d:/", unit);
	fd = fileXioDopen(p);
	if (fd >= 0)
	{
		fileXioDclose(fd);
		return 1;
	}
	return 0;
}

static int load_mmce_modules(void)
{
	int ret = -1;
	int id;

	if (mmce_loaded)
		return 1;
	if (mx4sio_loaded)
		return 0;

	id = SifExecModuleBuffer(mmceman_irx, size_mmceman_irx, 0, NULL, &ret);
	mmce_loaded = (id >= 0 && ret >= 0);
	if (mmce_loaded)
		ps2delay(2);
	return mmce_loaded;
}

static int load_mx4sio_modules(void)
{
	int ret = -1;
	int id;
	int u;
	int mass0_before;

	if (mx4sio_loaded)
		return (mx4sio_root[0] != 0);
	if (mmce_loaded)
		return 0;

	/* BDM + fatfs so mx4sio_bd can export a mass unit. usbhdfsd already
	 * owns mass: — extra units (mass1:) are what we look for. */
	SifExecModuleBuffer(bdm_irx, size_bdm_irx, 0, NULL, &ret);
	SifExecModuleBuffer(bdmfs_vfat_irx, size_bdmfs_vfat_irx, 0, NULL, &ret);

	mass0_before = probe_mass_unit(0);
	id = SifExecModuleBuffer(mx4sio_bd_irx, size_mx4sio_bd_irx, 0, NULL, &ret);
	if (id < 0 || ret < 0)
		return 0;

	mx4sio_loaded = 1;
	ps2delay(5);

	for (u = 1; u <= 3; u++)
	{
		if (probe_mass_unit(u))
		{
			sprintf(mx4sio_root, "mass%d:/", u);
			return 1;
		}
	}

	if (!mass0_before && probe_mass_unit(0))
	{
		strcpy(mx4sio_root, "mass0:/");
		return 1;
	}

	return 0;
}
#endif

int ps2init(const char *argv0)
{
   (void)argv0;
   SifInitRpc(0);
#ifndef DEBUG
   while(!SifIopReset(NULL, 0)){};
   while(!SifIopSync()){};

	fioExit();
	SifExitIopHeap();
	SifLoadFileExit();
	SifExitRpc();
	SifExitCmd();

	SifInitRpc(0);
	FlushCache(0);
	FlushCache(2);
#endif
   sbv_patch_enable_lmb();          
   //sbv_patch_disable_prefix_check();
   
   load_modules();

   ps2delay(5);
#ifndef HOST 
   fileXioInit();
#endif

   //init_handles();
   initdirs();
   ps2time_init();
   ps2delay(2);
   DPRINTF("-- PS2 inited --\n");
   return 1;
}

void WaitPadReady(int port, int slot)
{
	int state;
	int spins = 0;

	state = padGetState(port, slot);
	while((state != PAD_STATE_DISCONN)
		&& (state != PAD_STATE_STABLE)
		&& (state != PAD_STATE_FINDCTP1)){
		if (++spins > 2000000)
			return;
		state=padGetState(port, slot);
	}
}
void Wait_Pad_Ready(void)
{
	int state_1, state_2;
	int spins = 0;

	state_1 = padGetState(0, 0);
	state_2 = padGetState(1, 0);
	while((state_1 != PAD_STATE_DISCONN) && (state_2 != PAD_STATE_DISCONN)
		&& (state_1 != PAD_STATE_STABLE) && (state_2 != PAD_STATE_STABLE)
		&& (state_1 != PAD_STATE_FINDCTP1) && (state_2 != PAD_STATE_FINDCTP1)){
		if (++spins > 2000000)
			return;
		state_1 = padGetState(0, 0);
		state_2 = padGetState(1, 0);
	}
}
int Setup_Pad(void)
{
	int ret, i, port, state, modes;

	padInit(0);

	for(port=0; port<2; port++){
		if((ret = padPortOpen(port, 0, &padBuf_t[port][0])) == 0)
			return 0;
		WaitPadReady(port, 0);
		state = padGetState(port, 0);
		if(state != PAD_STATE_DISCONN){
			modes = padInfoMode(port, 0, PAD_MODETABLE, -1);
			if (modes != 0){
				i = 0;
				do{
					if (padInfoMode(port, 0, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK){
						padSetMainMode(port, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
						break;
					}
					i++;
				} while (i < modes);
			}
		}
	}
	return 1;
}

int fileXio_mode =  FIO_S_IRUSR | FIO_S_IWUSR | FIO_S_IXUSR | FIO_S_IRGRP | FIO_S_IWGRP | FIO_S_IXGRP | FIO_S_IROTH | FIO_S_IWOTH | FIO_S_IXOTH;

#ifdef HOST 
static int ps2FioDread(int fd, struct ps2dirent *dir)
{
	fio_dirent_t buf;
	int ret;
	
	ret = fioDread(fd, &buf);
	
	if(ret <= 0)
		return ret;
		
	strcpy(dir->d_name, buf.name);
	
	switch(buf.stat.mode & FIO_SO_IFMT)  /* mode */
    {
        case FIO_SO_IFLNK:
            dir->d_type = DT_LNK; break; 
        case FIO_SO_IFREG:
            dir->d_type = DT_REG; break;
        case FIO_SO_IFDIR:
            dir->d_type = DT_DIR; break;
        default:
            dir->d_type = DT_UNKNOWN; break;
    }
    
    return ret;
}
#else
static int ps2XioDread(int fd, struct ps2dirent *dir)
{
	iox_dirent_t buf;
	int ret;
	
	ret = fileXioDread(fd, &buf);
	
	if(ret <= 0)
		return ret;
		
	strcpy(dir->d_name, buf.name);
	
	switch(buf.stat.mode & FIO_S_IFMT)  /* mode */
    {
        case FIO_S_IFLNK:
            dir->d_type = DT_LNK; break; 
        case FIO_S_IFREG:
            dir->d_type = DT_REG; break;
        case FIO_S_IFDIR:
            dir->d_type = DT_DIR; break;
        default:
            dir->d_type = DT_UNKNOWN; break;
    }
    
    return ret;
}
#endif
int ps2Dopen(char *path)
{
#ifdef HOST
	return fioDopen(path);
#else
    return fileXioDopen(path);
#endif
}
int ps2Dclose(int fd)
{
#ifdef HOST
	return fioDclose(fd);
#else
    return fileXioDclose(fd);
#endif
}

#define MOUNT_LIMIT 4
#define MAX_PARTITIONS 20
#define MAX_NAME 256

static char partlist[MAX_PARTITIONS][MAX_NAME];
static char mountedParty[MOUNT_LIMIT][MAX_NAME];
static int allparts = 0;
static int alwaysMounted = 0;
static int latestMount = -1;
static char tmp[MAX_NAME];

static int hddinited = 0;
static char mainPath[MAX_NAME];

PS2DIR * ps2Opendir(char *path)
{
    int fd = -1;
	PS2DIR *ptr;
	char real[MAX_NAME];
	
	DPRINTF("opendir %s\n", path);

	strncpy(real, path, MAX_NAME - 1);
	real[MAX_NAME - 1] = 0;
#ifndef HOST
	if (path_has_dev(real, "mx4sio:") && mx4sio_root[0])
		strncpy(real, mx4sio_root, MAX_NAME - 1);
#endif
    
    if(!strncmp(real, "MAIN", 4) || !strcmp(real, "hdd0:/"))
    goto end;
	
	fd = ps2Dopen(real);
	
	DPRINTF("fd %d\n", fd);
		
	if(fd < 0)
		return NULL;
	
	end:
	ptr = (PS2DIR *)malloc(sizeof(PS2DIR));
	ptr->d_entry = NULL;
	
	if(ptr == NULL)
		return NULL;
	
	ptr->d_fd = fd;
	strcpy(ptr->d_name, real);
	
	return ptr;
}

int ps2Closedir(PS2DIR *d)
{
	int ret = 1;
	
	if(d != NULL)
	{
		DPRINTF("ps2dclose %d \n", d->d_fd);
		
		if(d->d_fd >= 0)
			ret = ps2Dclose(d->d_fd);
		
		free(d->d_entry);
		free(d);
	}
	
	return ret;
}

static int dir_ctr;
static int part_ctr;

struct ps2dirent *ps2Readdir(PS2DIR *d)
{   
    int ret;
	
	DPRINTF("ps2readdir %s \n", d->d_name);
	
	if(d->d_entry == NULL)
	{
		d->d_entry = (struct ps2dirent *)malloc(sizeof(struct ps2dirent));
	}

	if(!strncmp(d->d_name, "MAIN", 4))
    {
#ifdef HOST
        if(dir_ctr > 5)
        {
            dir_ctr = 0;
            return NULL;
        }

        switch(dir_ctr)
        {
              case 0:
                   if (main_path[0])
                       sprintf(d->d_entry->d_name, "%s/", main_path);
                   else
                       strcpy(d->d_entry->d_name, "host:/");
                   break;
              case 1:
                   sprintf(d->d_entry->d_name, "host:/"); break;
              case 2:
                   sprintf(d->d_entry->d_name, "mc0:/"); break;
              case 3:
                   sprintf(d->d_entry->d_name, "mc1:/"); break;
              case 4:
                   sprintf(d->d_entry->d_name, "mass:/"); break;
              case 5:
                   sprintf(d->d_entry->d_name, "cdfs:/"); break;
              default:
                      break;
        }
#else
        if(dir_ctr > 7)
        {
            dir_ctr = 0;
            return NULL;      
        }
                                      
        switch(dir_ctr)
        {
              case 0:
                   sprintf(d->d_entry->d_name, "mc0:/"); break;
              case 1:
                   sprintf(d->d_entry->d_name, "mc1:/"); break;
              case 2:
                   sprintf(d->d_entry->d_name, "mass:/"); break;
              case 3:
                   sprintf(d->d_entry->d_name, "MMCE0:/"); break;
              case 4:
                   sprintf(d->d_entry->d_name, "MMCE1:/"); break;
              case 5:
                   sprintf(d->d_entry->d_name, "MX4SIO:/"); break;
              case 6:
                   sprintf(d->d_entry->d_name, "cdfs:/"); break;
              case 7:
                   sprintf(d->d_entry->d_name, "hdd0:/"); break;
              default:
                      break;     
        }
#endif
		d->d_entry->d_type = DT_DIR;
		
		dir_ctr++;                     
    }
    else if(!strncmp(d->d_name, "hdd", 3))
    {
    	if(part_ctr < 0)
    	{
    		part_ctr = allparts;
      		return NULL;
		}
		strcpy(d->d_entry->d_name, partlist[part_ctr]);
    	d->d_entry->d_type = DT_DIR;
    	
    	part_ctr--;
    }
	else
	{
#ifdef HOST
		ret = ps2FioDread(d->d_fd, d->d_entry);
#else
		ret = ps2XioDread(d->d_fd, d->d_entry);
#endif

		if(ret <= 0)
		{
			return NULL;
		}
	}
	
	return d->d_entry;
}

int ps2Chdir(char *path)
{
    char *p;
	
    DPRINTF("chdir %s mainPath %s\n", path, mainPath);
    
    if((path == NULL) || !strncmp(path, "MAIN", 4))
    {
		strcpy(mainPath, "MAIN");
		return 1;
    }
    
    if(!strcmp(path, ".."))
    {
		if((p=strrchr(mainPath, '/'))!=NULL)
		{
			if(*(p-1) == ':')
			{
				if(*(p+1) != 0)
					*(p+1) = 0;
				else if(!strcmp(mainPath, "pfs0:/"))
					strcpy(mainPath, "hdd0:/");
				else
					strcpy(mainPath, "MAIN");
			}
			else
			{
				*p = 0;
			}
		}
		else
			strcpy(mainPath, "MAIN");
	}
    else
    {
    	if(!strcmp(mainPath, "MAIN"))
    	{
    		if(!strcmp(path, "hdd0:/"))
			{
				if(!hddinited)
        		{
					load_hddmodules();
					hddinit();
					get_part_list();       
					hddinited = 1;            
        		}
    		}
#ifndef HOST
			else if (path_has_dev(path, "mmce0:"))
			{
				load_mmce_modules();
				strcpy(mainPath, "mmce0:/");
				return 1;
			}
			else if (path_has_dev(path, "mmce1:"))
			{
				load_mmce_modules();
				strcpy(mainPath, "mmce1:/");
				return 1;
			}
			else if (path_has_dev(path, "mx4sio:"))
			{
				if (load_mx4sio_modules() && mx4sio_root[0])
					strcpy(mainPath, mx4sio_root);
				else
					strcpy(mainPath, "mx4sio:/");
				return 1;
			}
#endif
    		
    		strcpy(mainPath, path);
		}
		else
		{
			if(!strcmp(mainPath, "hdd0:/"))
			{
        		sprintf(mainPath, "hdd0:%s", path);
        		
				if(!fix_hddpath(mainPath))
					strcpy(mainPath, "MAIN");
			}
			else
			{
				p=strrchr(mainPath, '/');
			
				if(*(p - 1) == ':' && *(p + 1) == 0)
					sprintf(mainPath, "%s%s", mainPath, path);
				else
					sprintf(mainPath, "%s/%s", mainPath, path);
			}
		}	
	}

    return 1;
}

int ps2Getcwd(char *current_dir_name, int max_path)
{ 
    strcpy(current_dir_name, mainPath);
    
    return 1;
}

static int fix_hddpath(char *name)
{
     char *p;
     int i;
	 
	 memset(tmp, 0, MAX_NAME);
	 
     if((p = strchr(name, '/')) != NULL) //partition name with directory
     {
		  for(i = 0; i < MAX_PARTITIONS; i++)
          {
              if(!strncmp((name + 5), partlist[i], strlen(partlist[i]) - 1)) //is partition on the list
              {   
					i = mountParty(partlist[i]);
					
					if(i == -1)
					return 0;
					
					sprintf(tmp, "pfs%d:/%s", i, p+1);
					strcpy(name, tmp);
					
					DPRINTF("%s\n", name);
					
					return 1;
              }   
          }             
	 }
	 else //only partition name
	 {
		i = mountParty(name + 5);
		
		DPRINTF("%s i = %d\n", name, i);
					
		if(i == -1)
		return 0;
					
		sprintf(tmp, "pfs%d:/", i);
		strcpy(name, tmp);
		
		return 1;
	 }
	 
	 return 0;
}

static int initdirs()
{
   memset((void *)mainPath, 0, MAX_NAME);
   memset((void *)partlist, 0, MAX_PARTITIONS * MAX_NAME);
   memset((void *)mountedParty, 0, MOUNT_LIMIT * MAX_NAME);
   
   strcpy(mainPath, "MAIN");
   
   return 1;
}

static int get_part_list()
{
   iox_dirent_t dirEnt;
   int fd;     
   
   fd = fileXioDopen("hdd0:");
   
   part_ctr = -1;
               
    while(fileXioDread(fd, &dirEnt) > 0 )
    {             
        if(part_ctr >= MAX_PARTITIONS)
		{
           part_ctr = MAX_PARTITIONS;
           break;         
        }
		
        if((dirEnt.stat.attr != ATTR_MAIN_PARTITION) 
				|| (dirEnt.stat.mode != FS_TYPE_PFS))
			continue;

		//Patch this to see if new CB versions use valid PFS format
		//NB: All CodeBreaker versions up to v9.3 use invalid formats
		if(!strncmp(dirEnt.name, "PP.",3))
        {
			int len = strlen(dirEnt.name);
			if(!strcmp(dirEnt.name+len-4, ".PCB"))
				continue;
		}

		if(!strncmp(dirEnt.name, "__", 2) &&
			strcmp(dirEnt.name, "__boot") &&
			strcmp(dirEnt.name, "__net") &&
			strcmp(dirEnt.name, "__system") &&
			strcmp(dirEnt.name, "__sysconf") &&
			strcmp(dirEnt.name, "__common"))
			continue;
		
		part_ctr++;
		allparts = part_ctr;
		strcpy(partlist[part_ctr], dirEnt.name);
   }
   
   fileXioDclose(fd);
   
   return 1;
}

static void load_hddmodules()
{
   	// set the arguments for loading 'ps2fs'
	// -m 4  (maxmounts 4)
	// -o 10 (maxopen 10)
	// -n 40 (number of buffers 40)
   static char pfsarg[] = "-m" "\0" "4" "\0" "-o" "\0" "10" "\0" "-n" "\0" "40";
    // set the arguments for loading 'ps2hdd'
	// -o 4 (maxopen 4)
	// -n 20 (cachesize 20) 
   static char hddarg[] = "-o" "\0" "4" "\0" "-n" "\0" "20";
 
#ifndef DEBUG
    SifExecModuleBuffer(poweroff_irx, size_poweroff_irx, 0, NULL, NULL);
    SifExecModuleBuffer(ps2dev9_irx, size_ps2dev9_irx, 0, NULL, NULL);
#endif
    SifExecModuleBuffer(ps2atad_irx, size_ps2atad_irx, 0, NULL, NULL);
	SifExecModuleBuffer(ps2hdd_irx, size_ps2hdd_irx, sizeof(hddarg), hddarg, NULL);
	SifExecModuleBuffer(ps2fs_irx, size_ps2fs_irx, sizeof(pfsarg), pfsarg, NULL);
}

static int hddinit()
{
	if(hddCheckPresent() < 0)
	{
		DPRINTF("NO HDD FOUND!\n");
    	return -1;
    }
	else
	{
		DPRINTF("Found HDD!\n");
	}

	if(hddCheckFormatted() < 0)
    {
		DPRINTF("HDD Not Formatted!\n");
		return -1;	
	}
    else
    {
    	DPRINTF("HDD Is Formatted!\n");
	}

    
    return 1;
}

static int mountParty(char *party)
{
	int i, j;
	char pfs_str[6];
	
	for(i = 0; i < MOUNT_LIMIT; i++) //check already mounted PFS indexes
	{
		if(!strcmp(party, mountedParty[i]))
			goto return_i;
	}

	for(i = 0, j = -1; i < MOUNT_LIMIT; i++) //search for a free PFS index
	{ 
		if(mountedParty[i][0] == 0)
		{
			j = i;
			break;
		}
	}

	if(j == -1) //search for a suitable PFS index to unmount
	{
		for(i = 0; i < MOUNT_LIMIT; i++)
		{
			if((i != latestMount) && (i != alwaysMounted))
			{
				j = i;
				break;
			}
		}
		unmountParty(j);
	}

	i = j;
	strcpy(pfs_str, "pfs0:");

	pfs_str[3] = '0' + i;
	
	memset(tmp, 0, MAX_NAME);
	
	sprintf(tmp, "hdd0:%s", party);
	
	DPRINTF("Mount %s as %s\n", tmp, pfs_str);
	
	if(fileXioMount(pfs_str, tmp, FIO_MT_RDWR) < 0)
	{
		DPRINTF("Mount failed 1\n");
		for(i = 0; i <= MOUNT_LIMIT; i++)
		{
			if((i != latestMount) && (i != alwaysMounted))
			{
				DPRINTF("Mount try again %d\n", i);
				unmountParty(i);
				pfs_str[3] = '0' + i;
				if(fileXioMount(pfs_str, tmp, FIO_MT_RDWR) >= 0)
					break;
			}
		}
		DPRINTF("Mount failed 2\n");
		if(i > MOUNT_LIMIT)
			return -1;
	}
	strcpy(mountedParty[i], party);
return_i:
	if(i != alwaysMounted)
	latestMount = i;
	return i;
}

static void unmountParty(int party_ix)
{
	char pfs_str[6];

	strcpy(pfs_str, "pfs0:");
	pfs_str[3] += party_ix;
	
	DPRINTF("Umount party %s\n", pfs_str);
	
	if(fileXioUmount(pfs_str) < 0)
		return;
		
	if(party_ix < MOUNT_LIMIT)
		mountedParty[party_ix][0] = 0;

	if(latestMount == party_ix)
		latestMount = -1;
}

static void unmountAllParts()
{
    int i;
	
    for(i = 0; i < MOUNT_LIMIT; i++)
		unmountParty(i);
    
    fileXioStop();
}

static int ensure_hdd(void)
{
	if (hddinited)
		return 1;

	load_hddmodules();
	if (hddinit() < 0)
		return 0;
	get_part_list();
	hddinited = 1;
	return 1;
}

static int is_pfs_dev(const char *s)
{
	return s != NULL
		&& s[0] == 'p' && s[1] == 'f' && s[2] == 's'
		&& s[3] >= '0' && s[3] <= '9'
		&& s[4] == ':';
}

/* LaunchELF often passes pfs0:/folder/ELF. IOP reset drops that mount.
 * Try each PFS partition until the directory opens. */
static int resolve_pfs_dir(char *path, int is_main)
{
	char dir[MAX_NAME];
	char try_path[MAX_NAME];
	const char *rest;
	int i, fd, m;

	if (!is_pfs_dev(path))
		return 0;
	if (!ensure_hdd())
		return 0;

	rest = path + 5;
	if (*rest == '/')
		rest++;
	strncpy(dir, rest, MAX_NAME - 1);
	dir[MAX_NAME - 1] = 0;

	for (i = 0; i < MAX_PARTITIONS; i++)
	{
		if (partlist[i][0] == 0)
			continue;
		m = mountParty(partlist[i]);
		if (m < 0)
			continue;
		if (dir[0] != 0)
			sprintf(try_path, "pfs%d:/%s", m, dir);
		else
			sprintf(try_path, "pfs%d:/", m);
		fd = ps2Dopen(try_path);
		if (fd >= 0)
		{
			ps2Dclose(fd);
			strcpy(path, try_path);
			if (is_main)
				alwaysMounted = m;
			return 1;
		}
	}

	return 0;
}

/* hdd0:PARTITION:pfs:/dir/file.elf → hdd0:PARTITION:/dir */
static int parse_hdd0_pfs_argv(const char *argv, char *out)
{
	const char *pfs;
	const char *slash;
	size_t part_len;

	if (argv == NULL || strncmp(argv, "hdd0:", 5) != 0)
		return 0;

	pfs = strstr(argv, ":pfs:");
	if (pfs == NULL)
		return 0;

	part_len = (size_t)(pfs - argv);
	if (part_len == 0 || part_len >= MAX_NAME)
		return 0;

	memcpy(out, argv, part_len);
	out[part_len] = 0;

	slash = strrchr(pfs + 5, '/');
	if (slash != NULL && slash > pfs + 5)
	{
		size_t n = (size_t)(slash - (pfs + 5));
		if (part_len + n >= MAX_NAME)
			n = MAX_NAME - part_len - 1;
		memcpy(out + part_len, pfs + 5, n);
		out[part_len + n] = 0;
	}

	return 1;
}

int check_dir(char *path, int is_main)
{
	int i, fd;
	char *p;
	
	memset(tmp, 0, MAX_NAME);
	//strcpy(tmp, path);
	
	if(!strncmp(path, "hdd0:", 5))
    {
    	if(!ensure_hdd())
    		return 0;
		
		if(path[5] == '/')
			sprintf(path, "hdd0:%s", (path + 6));
			
		//sprintf(partlist[0], "+Test");
		
        for(i = 0; i < MAX_PARTITIONS; i++)
        {
            if(!strncmp((path + 5), partlist[i], strlen(partlist[i]) - 1))
            {		
					if(!fix_hddpath(path))
					return 0;
					
					if(is_main)
					alwaysMounted = i;
					
					break;
            }    
        }                               
    }

	
	fd = ps2Dopen(path);

	DPRINTF("dopen %s fd %d\n", path, fd);

	if(fd >= 0)
	{
		//DPRINTF("close %s\n", path);
		ps2Dclose(fd);
		
		if((p=strrchr(path, '/'))!=NULL && *(p+1) == 0)
    		*p = 0;
		
		//DPRINTF("argv %s\n", path);
		
		return 1;
	}

	/* pfs0:/dir after IOP reset — remount and find the folder. */
	if (resolve_pfs_dir(path, is_main))
		return 1;
	
	return 0;
}

int ps2DebugScreenInit()
{
#ifndef HOST
	init_scr();
	scr_clear();
	scr_printf("\n\n\n          ");
	
	return 1;
#endif
	return 0;
}

const char cnf_path_mc[] = "mc0:/PS2GBA/MAIN.CFG";

void ps2HostMainPath(char *path, const char *argv0)
{
	const char *slash;
	const char *bslash;
	const char *cut;

	if (path == NULL)
		return;

	path[0] = 0;
	if (argv0 == NULL || argv0[0] == 0)
	{
		strcpy(path, "host:");
		return;
	}

	slash = strrchr(argv0, '/');
	bslash = strrchr(argv0, '\\');
	cut = slash;
	if (bslash != NULL && (cut == NULL || bslash > cut))
		cut = bslash;

	if (cut != NULL)
	{
		size_t n = (size_t)(cut - argv0);
		if (n >= MAX_PATH)
			n = MAX_PATH - 1;
		memcpy(path, argv0, n);
		path[n] = 0;
	}
	else if (!strncmp(argv0, "host:", 5))
		strcpy(path, "host:");
	else
		strcpy(path, argv0);

	printf("HOST main_path=%s (argv0=%s)\n", path, argv0);
}

extern u32 parse_line(char *current_line, char *current_str);

int ps2GetMainPath(char *path, char *argv)
{
	FILE_TAG_TYPE cfg_file;
	char current_line[MAX_NAME];
	char current_str[MAX_NAME];
	char *p;
	int i;

	memset(current_str , 0, MAX_NAME);
	if (path != NULL)
		path[0] = 0;

	if(argv != NULL)
	{
		strcpy(current_str, argv);
		DPRINTF("argv %s\n", argv);
	}

	FILE_OPEN(cfg_file, cnf_path_mc, READ);
	
	if(FILE_CHECK_VALID(cfg_file))
	{
		ps2fgets(current_line, 256, cfg_file);

		FILE_CLOSE(cfg_file);

		for(i = 0; i < strlen(current_line); i++)
			if(current_line[i] == '\r' || current_line[i] == '\n')
				current_line[i] = 0;

		strcpy(path, current_line);

		DPRINTF("cnf_path %s\n", path);

		if(check_dir(path, 1))
			return 1;
	}

	if (argv == NULL || argv[0] == 0)
		return 0;
	
	// hdd0:PARTITION:pfs:/dir/ELF  (LaunchELF / FMCB)
	if(!strncmp(argv, "hdd0:", 5))
	{
		if (parse_hdd0_pfs_argv(argv, path))
			return check_dir(path, 1);

		if((p=strrchr(argv, ':'))!=NULL)
		{
			sprintf(current_str + (p - argv) - 4, "%s", argv + (p - argv) + 1);
			
			if((p=strrchr(current_str, '/')) != NULL)
			{
				*p = 0;
			}
			
			strcpy(path, current_str);
		}
		else
			return 0; 
	}
	else if (is_pfs_dev(argv))
	{
		/* pfs0:/dir/ELF — mount is gone after IOP reset. */
		p = strrchr(argv, '/');
		if (p != NULL && p > argv + 4)
		{
			memcpy(path, argv, (size_t)(p - argv));
			path[p - argv] = 0;
		}
		else
		{
			memcpy(path, argv, 5);
			path[5] = 0;
		}
		return resolve_pfs_dir(path, 1);
	}
	else if(!strncmp(argv, "cdfs:", 5) || !strncmp(argv, "cdrom", 5))
	{
#ifdef HOST
		mkdir("mc0:/PS2GBA");
		mkdir("mc0:/PS2GBA/TEMPGBA");
#else
		fileXioMkdir("mc0:/PS2GBA", fileXio_mode);
		fileXioMkdir("mc0:/PS2GBA/TEMPGBA", fileXio_mode);
#endif
		strcpy(path, "mc0:/PS2GBA/TEMPGBA");
	}
	else
	{
		memset(path , 0, 255);
		
		if((p=strrchr(argv, '/'))!=NULL)
		{
			memcpy(path, current_str, (p - argv));
		}
		else if((p=strrchr(argv, ':'))!=NULL)
		{
			memcpy(path, current_str, (p - argv) + 1);
		}
		else
			return 0;
	}
	
	DPRINTF("argv %s\n", path);
	
	if (check_dir(path, 1))
		return 1;
#ifndef HOST
	/* LaunchELF maps MX4SIO as mass#:/ — driver is gone after IOP reset. */
	if (path_has_dev(path, "mass:") || path_has_dev(path, "mx4sio:") ||
		(argv != NULL && (path_has_dev(argv, "mass:") || path_has_dev(argv, "mx4sio:"))))
	{
		if (load_mx4sio_modules())
			return check_dir(path, 1);
	}
#endif
	return 0;
}

#define PS2FGETS_BUF 8192

static FILE_TAG_TYPE ps2fgets_fd = -1;
static unsigned char ps2fgets_buf[PS2FGETS_BUF];
static int ps2fgets_len = 0;
static int ps2fgets_pos = 0;

void ps2fgets_invalidate(FILE_TAG_TYPE stream)
{
	if (ps2fgets_fd == stream)
	{
		ps2fgets_fd = -1;
		ps2fgets_len = 0;
		ps2fgets_pos = 0;
	}
}

static int ps2fgets_getc(FILE_TAG_TYPE stream)
{
	int n;

	if (ps2fgets_fd != stream)
	{
		ps2fgets_fd = stream;
		ps2fgets_len = 0;
		ps2fgets_pos = 0;
	}

	if (ps2fgets_pos >= ps2fgets_len)
	{
		n = FILE_READ(stream, ps2fgets_buf, PS2FGETS_BUF);
		if (n <= 0)
		{
			ps2fgets_len = 0;
			ps2fgets_pos = 0;
			return -1;
		}
		ps2fgets_len = n;
		ps2fgets_pos = 0;
	}

	return ps2fgets_buf[ps2fgets_pos++];
}

char *ps2fgets(char *str, int size, FILE_TAG_TYPE stream)
{
	int i, c;

	if (str == NULL || stream < 0 || size < 2)
		return NULL;

	for (i = 0; i < size - 1; i++)
	{
		c = ps2fgets_getc(stream);
		if (c < 0)
		{
			if (i == 0)
				return NULL;
			str[i] = 0;
			return str;
		}

		if (c == '\r')
		{
			int n = ps2fgets_getc(stream);
			if (n != '\n' && n >= 0 && ps2fgets_pos > 0)
				ps2fgets_pos--;
			str[i] = '\n';
			str[i + 1] = 0;
			return str;
		}

		if (c == '\n' || c == 0)
		{
			str[i] = (c == 0) ? 0 : '\n';
			str[i + 1] = 0;
			return str;
		}

		str[i] = (char)c;
	}

	str[size - 1] = 0;
	return str;
}

int clock_gettime(struct timespec *ts)
{
	struct timeval tv;
	int ret;
	
	ret = ps2time_gettimeofday(&tv, NULL);
	
	ts->tv_sec = tv.tv_sec;
	ts->tv_nsec = tv.tv_usec * 1000;
	
	return ret;
}

