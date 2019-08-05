#ifndef _SCULL_H_
#define _SCULL_H_

#include <linux/ioctl.h>

#ifndef NR_DEVS
#define NR_DEVS 16  
#endif

#ifndef MULTIPIPE_BUFFER
#define MULTIPIPE_BUFFER 4000
#endif

/* Ioctl definitions */

/* Use 'k' as magic number */
#define MULTIP_IOC_MAGIC  'm'
/* Please use a different 8-bit number in your code */

#define MULTIP_IOCRESET    _IO(MULTIP_IOC_MAGIC, 0)
#define MULTIP_IOCSETRSET  _IO(MULTIP_IOC_MAGIC, 1)
#define MULTIP_IOCQGETRSET _IO(MULTIP_IOC_MAGIC, 2)
#define MULTIP_IOCGETREADY _IO(MULTIP_IOC_MAGIC, 3, short)
#define MULTIP_IOCTSENDEOF _IO(MULTIP_IOC_MAGIC, 4)

#define MULTIP_IOC_MAXNR 4

#endif

