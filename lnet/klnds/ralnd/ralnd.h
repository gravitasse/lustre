/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2004 Cluster File Systems, Inc.
 *   Author: Eric Barton <eric@bartonsoftware.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <linux/uio.h>

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <linux/kmod.h>
#include <linux/sysctl.h>

#include <net/sock.h>
#include <linux/in.h>

#define DEBUG_SUBSYSTEM S_NAL

#include <libcfs/kp30.h>
#include <portals/p30.h>
#include <portals/lib-p30.h>
#include <portals/nal.h>

#include <rapl.h>

#define RANAL_MAXDEVS       2                   /* max # devices RapidArray supports */

#define RANAL_N_CONND       4                   /* # connection daemons */

#define RANAL_MIN_RECONNECT_INTERVAL 1          /* first failed connection retry (seconds)... */
#define RANAL_MAX_RECONNECT_INTERVAL 60         /* ...exponentially increasing to this */

#define RANAL_FMA_MAX_PREFIX      232           /* max size of FMA "Prefix" */
#define RANAL_FMA_MAX_DATA        ((7<<10)-256) /* Max FMA MSG is 7K including prefix */

#define RANAL_PEER_HASH_SIZE  101               /* # peer lists */
#define RANAL_CONN_HASH_SIZE  101               /* # conn lists */

#define RANAL_NTX             64                /* # tx descs */
#define RANAL_NTX_NBLK        256               /* # reserved tx descs */

#define RANAL_FMA_CQ_SIZE     8192              /* # entries in receive CQ
                                                 * (overflow is a performance hit) */

#define RANAL_RESCHED         100               /* # scheduler loops before reschedule */

#define RANAL_MIN_TIMEOUT     5                 /* minimum timeout interval (seconds) */
#define RANAL_TIMEOUT2KEEPALIVE(t) (((t)+1)/2)  /* timeout -> keepalive interval */

/* default vals for runtime tunables */
#define RANAL_TIMEOUT           30              /* comms timeout (seconds) */
#define RANAL_LISTENER_TIMEOUT   5              /* listener timeout (seconds) */
#define RANAL_BACKLOG          127              /* listener's backlog */
#define RANAL_PORT             988              /* listener's port */
#define RANAL_MAX_IMMEDIATE    (2<<10)          /* immediate payload breakpoint */

typedef struct
{
        int               kra_timeout;          /* comms timeout (seconds) */
        int               kra_listener_timeout; /* max time the listener can block */
        int               kra_backlog;          /* listener's backlog */
        int               kra_port;             /* listener's TCP/IP port */
        int               kra_max_immediate;    /* immediate payload breakpoint */

        struct ctl_table_header *kra_sysctl;    /* sysctl interface */
} kra_tunables_t;

typedef struct
{
        RAP_PVOID               rad_handle;     /* device handle */
        RAP_PVOID               rad_fma_cqh;    /* FMA completion queue handle */
        RAP_PVOID               rad_rdma_cqh;   /* rdma completion queue handle */
        int                     rad_id;         /* device id */
        int                     rad_idx;        /* index in kra_devices */
        int                     rad_ready;      /* set by device callback */
        struct list_head        rad_connq;      /* connections requiring attention */
        wait_queue_head_t       rad_waitq;      /* scheduler waits here */
        spinlock_t              rad_lock;       /* serialise */
        void                   *rad_scheduler;  /* scheduling thread */
        int                     rad_setri_please; /* ++ when connd wants to setri */
        struct semaphore        rad_setri_mutex; /* serialise setri */
} kra_device_t;

typedef struct
{
        int               kra_init;             /* initialisation state */
        int               kra_shutdown;         /* shut down? */
        atomic_t          kra_nthreads;         /* # live threads */

        struct semaphore  kra_nid_mutex;        /* serialise NID/listener ops */
        struct semaphore  kra_listener_signal;  /* block for listener startup/shutdown */
        struct socket    *kra_listener_sock;    /* listener's socket */
        int               kra_listener_shutdown; /* ask listener to close */

        kra_device_t      kra_devices[RANAL_MAXDEVS]; /* device/ptag/cq etc */
        int               kra_ndevs;            /* # devices */

        rwlock_t          kra_global_lock;      /* stabilize peer/conn ops */

        struct list_head *kra_peers;            /* hash table of all my known peers */
        int               kra_peer_hash_size;   /* size of kra_peers */
        atomic_t          kra_npeers;           /* # peers extant */

        struct list_head *kra_conns;            /* conns hashed by cqid */
        int               kra_conn_hash_size;   /* size of kra_conns */
        __u64             kra_peerstamp;        /* when I started up */
        __u64             kra_connstamp;        /* conn stamp generator */
        int               kra_next_cqid;        /* cqid generator */
        atomic_t          kra_nconns;           /* # connections extant */

        long              kra_new_min_timeout;  /* minimum timeout on any new conn */
        wait_queue_head_t kra_reaper_waitq;     /* reaper sleeps here */
        spinlock_t        kra_reaper_lock;      /* serialise */

        struct list_head  kra_connd_peers;      /* peers waiting for a connection */
        struct list_head  kra_connd_acceptq;    /* accepted sockets to handshake */
        wait_queue_head_t kra_connd_waitq;      /* connection daemons sleep here */
        spinlock_t        kra_connd_lock;       /* serialise */

        struct list_head  kra_idle_txs;         /* idle tx descriptors */
        struct list_head  kra_idle_nblk_txs;    /* idle reserved tx descriptors */
        __u64             kra_next_tx_cookie;   /* RDMA completion cookie */
        wait_queue_head_t kra_idle_tx_waitq;    /* block here for tx descriptor */
        spinlock_t        kra_tx_lock;          /* serialise */
} kra_data_t;

#define RANAL_INIT_NOTHING         0
#define RANAL_INIT_DATA            1
#define RANAL_INIT_LIB             2
#define RANAL_INIT_ALL             3

typedef struct kra_acceptsock                   /* accepted socket queued for connd */
{
        struct list_head     ras_list;          /* queue for attention */
        struct socket       *ras_sock;          /* the accepted socket */
} kra_acceptsock_t;

/************************************************************************
 * Wire message structs.  These are sent in sender's byte order
 * (i.e. receiver checks magic and flips if required).
 */

typedef struct kra_connreq                      /* connection request/response */
{                                               /* (sent via socket) */
        __u32             racr_magic;           /* I'm an ranal connreq */
        __u16             racr_version;         /* this is my version number */
        __u16             racr_devid;           /* sender's device ID */
        __u64             racr_srcnid;          /* sender's NID */
        __u64             racr_dstnid;          /* who sender expects to listen */
        __u64             racr_peerstamp;       /* sender's instance stamp */
        __u64             racr_connstamp;       /* sender's connection stamp */
        __u32             racr_timeout;         /* sender's timeout */
        RAP_RI_PARAMETERS racr_riparams;        /* sender's endpoint info */
} kra_connreq_t;

typedef struct
{
        RAP_MEM_KEY       rard_key;
        RAP_PVOID64       rard_addr;
        RAP_UINT32        rard_nob;
} kra_rdma_desc_t;

typedef struct
{
        ptl_hdr_t         raim_hdr;             /* portals header */
        /* Portals payload is in FMA "Message Data" */
} kra_immediate_msg_t;

typedef struct
{
        ptl_hdr_t         raprm_hdr;            /* portals header */
        __u64             raprm_cookie;         /* opaque completion cookie */
} kra_putreq_msg_t;

typedef struct
{
        __u64             rapam_src_cookie;     /* reflected completion cookie */
        __u64             rapam_dst_cookie;     /* opaque completion cookie */
        kra_rdma_desc_t   rapam_desc;           /* sender's sink buffer */
} kra_putack_msg_t;

typedef struct
{
        ptl_hdr_t         ragm_hdr;             /* portals header */
        __u64             ragm_cookie;          /* opaque completion cookie */
        kra_rdma_desc_t   ragm_desc;            /* sender's sink buffer */
} kra_get_msg_t;

typedef struct
{
        __u64             racm_cookie;          /* reflected completion cookie */
} kra_completion_msg_t;

typedef struct                                  /* NB must fit in FMA "Prefix" */
{
        __u32             ram_magic;            /* I'm an ranal message */
        __u16             ram_version;          /* this is my version number */
        __u16             ram_type;             /* msg type */
        __u64             ram_srcnid;           /* sender's NID */
        __u64             ram_connstamp;        /* sender's connection stamp */
        union {
                kra_immediate_msg_t   immediate;
                kra_putreq_msg_t      putreq;
                kra_putack_msg_t      putack;
                kra_get_msg_t         get;
                kra_completion_msg_t  completion;
        }                    ram_u;
        __u32             ram_seq;              /* incrementing sequence number */
} kra_msg_t;

#define RANAL_MSG_MAGIC       0x0be91b92        /* unique magic */
#define RANAL_MSG_VERSION              1        /* current protocol version */

#define RANAL_MSG_FENCE             0x80        /* fence RDMA */

#define RANAL_MSG_NONE              0x00        /* illegal message */
#define RANAL_MSG_NOOP              0x01        /* empty ram_u (keepalive) */
#define RANAL_MSG_IMMEDIATE         0x02        /* ram_u.immediate */
#define RANAL_MSG_PUT_REQ           0x03        /* ram_u.putreq (src->sink) */
#define RANAL_MSG_PUT_NAK           0x04        /* ram_u.completion (no PUT match: sink->src) */
#define RANAL_MSG_PUT_ACK           0x05        /* ram_u.putack (PUT matched: sink->src) */
#define RANAL_MSG_PUT_DONE          0x86        /* ram_u.completion (src->sink) */
#define RANAL_MSG_GET_REQ           0x07        /* ram_u.get (sink->src) */
#define RANAL_MSG_GET_NAK           0x08        /* ram_u.completion (no GET match: src->sink) */
#define RANAL_MSG_GET_DONE          0x89        /* ram_u.completion (src->sink) */
#define RANAL_MSG_CLOSE             0x8a        /* empty ram_u */

/***********************************************************************/

typedef struct kra_tx                           /* message descriptor */
{
        struct list_head          tx_list;      /* queue on idle_txs/rac_sendq/rac_waitq */
        struct kra_conn          *tx_conn;      /* owning conn */
        lib_msg_t                *tx_libmsg[2]; /* lib msgs to finalize on completion */
        unsigned long             tx_qtime;     /* when tx started to wait for something (jiffies) */
        int                       tx_isnblk;    /* I'm reserved for non-blocking sends */
        int                       tx_nob;       /* # bytes of payload */
        int                       tx_buftype;   /* payload buffer type */
        void                     *tx_buffer;    /* source/sink buffer */
        int                       tx_phys_offset; /* first page offset (if phys) */
        int                       tx_phys_npages; /* # physical pages */
        RAP_PHYS_REGION          *tx_phys;      /* page descriptors */
        RAP_MEM_KEY               tx_map_key;   /* mapping key */
        RAP_RDMA_DESCRIPTOR       tx_rdma_desc; /* rdma descriptor */
        __u64                     tx_cookie;    /* identify this tx to peer */
        kra_msg_t                 tx_msg;       /* FMA message buffer */
} kra_tx_t;

#define RANAL_BUF_NONE           0              /* buffer type not set */
#define RANAL_BUF_IMMEDIATE      1              /* immediate data */
#define RANAL_BUF_PHYS_UNMAPPED  2              /* physical: not mapped yet */
#define RANAL_BUF_PHYS_MAPPED    3              /* physical: mapped already */
#define RANAL_BUF_VIRT_UNMAPPED  4              /* virtual: not mapped yet */
#define RANAL_BUF_VIRT_MAPPED    5              /* virtual: mapped already */

#define RANAL_TX_IDLE            0x00           /* on freelist */
#define RANAL_TX_SIMPLE          0x10           /* about to send a simple message */
#define RANAL_TX_PUTI_REQ        0x20           /* PUT initiator about to send PUT_REQ */
#define RANAL_TX_PUTI_WAIT_ACK   0x21           /* PUT initiator waiting for PUT_ACK */
#define RANAL_TX_PUTI_RDMA       0x22           /* PUT initiator waiting for RDMA to complete */
#define RANAL_TX_PUTI_DONE       0x23           /* PUT initiator about to send PUT_DONE */
#define RANAL_TX_PUTT_NAK        0x30           /* PUT target about to send PUT_NAK */
#define RANAL_TX_PUTT_ACK        0x30           /* PUT target about to send PUT_ACK */
#define RANAL_TX_PUTT_WAIT_DONE  0x31           /* PUT target waiting for PUT_DONE */
#define RANAL_TX_GETI_REQ        0x40           /* GET initiator about to send GET_REQ */
#define RANAL_TX_GETI_WAIT_DONE  0x41           /* GET initiator waiting for GET_DONE */
#define RANAL_TX_GETT_NAK        0x50           /* GET target about to send PUT_NAK */
#define RANAL_TX_GETT_RDMA       0x51           /* GET target waiting for RDMA to complete */
#define RANAL_TX_GETT_DONE       0x52           /* GET target about to send GET_DONE */

typedef struct kra_conn
{
        struct kra_peer    *rac_peer;           /* owning peer */
        struct list_head    rac_list;           /* stash on peer's conn list */
        struct list_head    rac_hashlist;       /* stash in connection hash table */
        struct list_head    rac_schedlist;      /* schedule (on rad_connq) for attention */
        struct list_head    rac_fmaq;           /* txs queued for FMA */
        struct list_head    rac_rdmaq;          /* txs awaiting RDMA completion */
        struct list_head    rac_replyq;         /* txs awaiting replies */
        __u64               rac_peerstamp;      /* peer's unique stamp */
        __u64               rac_peer_connstamp; /* peer's unique connection stamp */
        __u64               rac_my_connstamp;   /* my unique connection stamp */
        unsigned long       rac_last_tx;        /* when I last sent an FMA message (jiffies) */
        unsigned long       rac_last_rx;        /* when I last received an FMA messages (jiffies) */
        long                rac_keepalive;      /* keepalive interval (seconds) */
        long                rac_timeout;        /* infer peer death if no rx for this many seconds */
        __u32               rac_cqid;           /* my completion callback id (non-unique) */
        __u32               rac_tx_seq;         /* tx msg sequence number */
        __u32               rac_rx_seq;         /* rx msg sequence number */
        atomic_t            rac_refcount;       /* # users */
        unsigned int        rac_close_sent;     /* I've sent CLOSE */
        unsigned int        rac_close_recvd;    /* I've received CLOSE */
        unsigned int        rac_state;          /* connection state */
        unsigned int        rac_scheduled;      /* being attented to */
        spinlock_t          rac_lock;           /* serialise */
        kra_device_t       *rac_device;         /* which device */
        RAP_PVOID           rac_rihandle;       /* RA endpoint */
        kra_msg_t          *rac_rxmsg;          /* incoming message (FMA prefix) */
        kra_msg_t           rac_msg;            /* keepalive/CLOSE message buffer */
} kra_conn_t;

#define RANAL_CONN_ESTABLISHED     0
#define RANAL_CONN_CLOSING         1
#define RANAL_CONN_CLOSED          2

typedef struct kra_peer
{
        struct list_head    rap_list;           /* stash on global peer list */
        struct list_head    rap_connd_list;     /* schedule on kra_connd_peers */
        struct list_head    rap_conns;          /* all active connections */
        struct list_head    rap_tx_queue;       /* msgs waiting for a conn */
        ptl_nid_t           rap_nid;            /* who's on the other end(s) */
        __u32               rap_ip;             /* IP address of peer */
        int                 rap_port;           /* port on which peer listens */
        atomic_t            rap_refcount;       /* # users */
        int                 rap_persistence;    /* "known" peer refs */
        int                 rap_connecting;     /* connection forming */
        unsigned long       rap_reconnect_time; /* CURRENT_SECONDS when reconnect OK */
        unsigned long       rap_reconnect_interval; /* exponential backoff */
} kra_peer_t;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
# define sk_allocation  allocation
# define sk_data_ready  data_ready
# define sk_write_space write_space
# define sk_user_data   user_data
# define sk_prot        prot
# define sk_sndbuf      sndbuf
# define sk_socket      socket
# define sk_wmem_queued wmem_queued
# define sk_err         err
# define sk_sleep       sleep
#endif

extern lib_nal_t       kranal_lib;
extern kra_data_t      kranal_data;
extern kra_tunables_t  kranal_tunables;

extern void kranal_destroy_peer(kra_peer_t *peer);
extern void kranal_destroy_conn(kra_conn_t *conn);

static inline void
kranal_peer_addref(kra_peer_t *peer)
{
        CDEBUG(D_NET, "%p->"LPX64"\n", peer, peer->rap_nid);
        LASSERT(atomic_read(&peer->rap_refcount) > 0);
        atomic_inc(&peer->rap_refcount);
}

static inline void
kranal_peer_decref(kra_peer_t *peer)
{
        CDEBUG(D_NET, "%p->"LPX64"\n", peer, peer->rap_nid);
        LASSERT(atomic_read(&peer->rap_refcount) > 0);
        if (atomic_dec_and_test(&peer->rap_refcount))
                kranal_destroy_peer(peer);
}

static inline struct list_head *
kranal_nid2peerlist (ptl_nid_t nid)
{
        unsigned int hash = ((unsigned int)nid) % kranal_data.kra_peer_hash_size;

        return (&kranal_data.kra_peers[hash]);
}

static inline int
kranal_peer_active(kra_peer_t *peer)
{
        /* Am I in the peer hash table? */
        return (!list_empty(&peer->rap_list));
}

static inline void
kranal_conn_addref(kra_conn_t *conn)
{
        CDEBUG(D_NET, "%p->"LPX64"\n", conn, conn->rac_peer->rap_nid);
        LASSERT(atomic_read(&conn->rac_refcount) > 0);
        atomic_inc(&conn->rac_refcount);
}

static inline void
kranal_conn_decref(kra_conn_t *conn)
{
        CDEBUG(D_NET, "%p->"LPX64"\n", conn, conn->rac_peer->rap_nid);
        LASSERT(atomic_read(&conn->rac_refcount) > 0);
        if (atomic_dec_and_test(&conn->rac_refcount))
                kranal_destroy_conn(conn);
}

static inline struct list_head *
kranal_cqid2connlist (__u32 cqid)
{
        unsigned int hash = cqid % kranal_data.kra_conn_hash_size;

        return (&kranal_data.kra_conns [hash]);
}

static inline kra_conn_t *
kranal_cqid2conn_locked (__u32 cqid)
{
        struct list_head *conns = kranal_cqid2connlist(cqid);
        struct list_head *tmp;
        kra_conn_t       *conn;

        list_for_each(tmp, conns) {
                conn = list_entry(tmp, kra_conn_t, rac_hashlist);

                if (conn->rac_cqid == cqid)
                        return conn;
        }

        return NULL;
}

static inline int
kranal_tx_mapped (kra_tx_t *tx)
{
        return (tx->tx_buftype == RANAL_BUF_VIRT_MAPPED ||
                tx->tx_buftype == RANAL_BUF_PHYS_MAPPED);
}

static inline __u64
kranal_page2phys (struct page *p)
{
        return page_to_phys(p);
}

extern void kranal_free_acceptsock (kra_acceptsock_t *ras);
extern int kranal_listener_procint (ctl_table *table,
                                    int write, struct file *filp,
                                    void *buffer, size_t *lenp);
extern void kranal_update_reaper_timeout (long timeout);
extern void kranal_tx_done (kra_tx_t *tx, int completion);
extern void kranal_unlink_peer_locked (kra_peer_t *peer);
extern void kranal_schedule_conn (kra_conn_t *conn);
extern kra_peer_t *kranal_create_peer (ptl_nid_t nid);
extern kra_peer_t *kranal_find_peer_locked (ptl_nid_t nid);
extern void kranal_post_fma (kra_conn_t *conn, kra_tx_t *tx);
extern int kranal_del_peer (ptl_nid_t nid, int single_share);
extern void kranal_device_callback (RAP_INT32 devid, RAP_PVOID arg);
extern int kranal_thread_start (int(*fn)(void *arg), void *arg);
extern int kranal_connd (void *arg);
extern int kranal_reaper (void *arg);
extern int kranal_scheduler (void *arg);
extern void kranal_close_conn_locked (kra_conn_t *conn, int error);
extern void kranal_terminate_conn_locked (kra_conn_t *conn);
extern void kranal_connect (kra_peer_t *peer);
extern int kranal_conn_handshake (struct socket *sock, kra_peer_t *peer);
extern void kranal_pause(int ticks);
