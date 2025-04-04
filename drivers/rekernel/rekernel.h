#ifndef FREEZER_GKI_H
#define FREEZER_GKI_H

#define KERNEL_6_6

#define MIN_USERAPP_UID (10000)
#define MAX_SYSTEM_UID  (2000)
#define RESERVE_ORDER					17
#define WARN_AHEAD_SPACE				(1 << RESERVE_ORDER)
#define INTERFACETOKEN_BUFF_SIZE (140)
#define PARCEL_OFFSET (16) /* sync with the writeInterfaceToken */
#define LINE_ERROR (-1)
#define LINE_SUCCESS (0)

#endif