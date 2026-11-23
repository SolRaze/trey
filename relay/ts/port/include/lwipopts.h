/* lwIP for the tunnel side only: the wireguard netif, the tcpip thread microlink posts to,
 * raw TCP for the 100.x listener. The underlay is the OS socket API, never lwIP. */
#ifndef TS_LWIPOPTS_H
#define TS_LWIPOPTS_H

#define NO_SYS                      0
#define SYS_LIGHTWEIGHT_PROT        1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_IPV4                   1
#define LWIP_IPV6                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_DHCP                   0
#define LWIP_DNS                    0
#define LWIP_IGMP                   0
#define LWIP_ARP                    0
#define LWIP_ETHERNET               0
#define LWIP_IPV6_MLD               0
#define LWIP_IPV6_AUTOCONFIG        0
#define LWIP_IPV6_DHCP6             0
#define LWIP_IPV6_ND                0
#define LWIP_ND6_QUEUEING           0
#define LWIP_IPV6_FRAG              0
#define LWIP_IPV6_REASS             0
#define IP_REASSEMBLY               0
#define IP_FRAG                     0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_TCPIP_CORE_LOCKING     1
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1
#define LWIP_ERRNO_STDINCLUDE       1   /* LWIP_PROVIDE_ERRNO stays undefined: lwIP tests it with #ifdef */

#define MEM_LIBC_MALLOC             1
#define MEMP_MEM_MALLOC             1
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (64 * 1024)
#define PBUF_POOL_SIZE              32
#define MEMP_NUM_TCP_PCB            16      /* serve.c CONNS 12 + microlink */
#define MEMP_NUM_TCP_PCB_LISTEN     104     /* serve.c: FTP 5000 + PASV 5001-5100 */
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_SYS_TIMEOUT        16
#define TCP_MSS                     1200    /* tunnel MTU 1280 */
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            32

#define TCPIP_THREAD_NAME           "tcpip"
#define TCPIP_THREAD_STACKSIZE      8192
#define TCPIP_THREAD_PRIO           10
#define TCPIP_MBOX_SIZE             32
#define DEFAULT_THREAD_STACKSIZE    8192
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_TCP_RECVMBOX_SIZE   8
#define DEFAULT_ACCEPTMBOX_SIZE     8

#define LWIP_STATS                  0
#define LWIP_DEBUG                  0
#define LWIP_CHECKSUM_ON_COPY       1

/* microlink and wireguard-lwip call ESP-lwIP's thread query; sys_arch.c provides it */
#define LWIP_CORE_LOCK_QUERY_HOLDER 1

/* core lock with holder tracking (sys_arch.c) */
void ts_lock_core(void);
void ts_unlock_core(void);
#define LOCK_TCPIP_CORE()   ts_lock_core()
#define UNLOCK_TCPIP_CORE() ts_unlock_core()

#endif
