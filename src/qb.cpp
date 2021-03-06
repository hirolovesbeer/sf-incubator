
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

#include <getopt.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>

#ifndef __linux__
#include <pthread_np.h>
#include <net/ethernet.h>
#else
#include <netinet/ether.h>
#endif


#include "common.hpp"
#include "netmap.hpp"
#include "ether.hpp"

#ifdef FULLTAP
#else
#include "selector.hpp"
#endif

extern bool debug;
pthread_mutex_t debug_mutex;

bool bind_cpu(int cpu, int reverse);

void* th_left_to_right(void* param);
void* th_right_to_left(void* param);
void* th_tap(void* param);

void tap_processing(netmap* nm,
        int ringid, std::vector<netmap*>* v_nm_tap);
void fw_processing(netmap* nm_rx, netmap* nm_tx,
        int rx_ringid, int tx_ringid, std::vector<netmap*>* v_nm_tap);

static inline void
slot_swap(struct netmap_ring* rxring, struct netmap_ring* txring);
static inline void
pkt_copy(struct netmap_ring* rxring, struct netmap_ring* txring, int flag);

#ifdef DROPLLC
bool llc_filter(struct netmap_ring* ring);
#endif

#ifdef DEBUG
void show_counter(netmap* l, netmap* r, std::vector<netmap*>& v);
#endif

void usage(char* prog_name);

struct thread_param {
    int rx_ringid;
    int tx_ringid;
    netmap* nm_rx;
    netmap* nm_tx;
    std::vector<netmap*>* nm_tap;
};

struct tap_info {
    int fd;
    int ringid;
    struct netmap_ring* ring;
    netmap* nm;
    //struct ether_addr mac_dest;
};


int
main(int argc, char** argv)
{
    debug = false;
    pthread_mutex_init(&debug_mutex, NULL);

    int opt;
    int option_index;
    std::string opt_l;
    std::string opt_r;
    std::string opt_t;

    std::vector<std::string> tap_list;
    std::vector<struct ether_addr> dmac_list;

    std::vector<std::string> if_list = get_ifname_list();

    struct option long_options[] = {
        {"help",  no_argument, NULL, 'h'},
        {"left",  no_argument, NULL, 'l'},
        {"right", no_argument, NULL, 'r'},
        {"tap",   no_argument, NULL, 't'},
#ifdef DEBUG
        {"verbose", no_argument, NULL, 'v'},
#endif
        {0, 0, 0, 0},
    };

    while ((opt = getopt_long(argc, argv,
                "l:r:t:hv?", long_options, &option_index)) != -1)
    {
        switch (opt)
        {
#ifdef DEBUG
        case 'v':
            debug = true;
            break;
#endif
        case 'l':
            opt_l = optarg;
            break;

        case 'r':
            opt_r = optarg;
            break;

        case 't':
            opt_t = optarg;
            break;

        case 'h':
        case '?':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
            break;

        default:
            exit(EXIT_FAILURE);
        }
    }


    netmap* nm_l;
    if (opt_l.size() == 0) {
        nm_l = NULL;
    } else if (!is_exist_if(if_list, opt_l)) {
        MESG2("-l is not exist interface.");
        exit(EXIT_FAILURE);
    } else {
        nm_l = new netmap();
        if (! nm_l->open_if(opt_l)) {
            MESG2("could not open -l (%s)", opt_l.c_str());
            exit(EXIT_FAILURE);
        }
        nm_l->set_promisc();
    }

    netmap* nm_r;
    if (opt_r.size() == 0) {
        nm_r = NULL;
    } else if (!is_exist_if(if_list, opt_r)) {
        MESG2("-r is not exist interface.");
        exit(EXIT_FAILURE);
    } else {
        nm_r = new netmap();
        nm_r->open_if(opt_r);
        if (! nm_r->open_if(opt_r)) {
            MESG2("could not open -r (%s)", opt_r.c_str());
            exit(EXIT_FAILURE);
        }
        nm_r->set_promisc();
    }

    if (nm_l == NULL && nm_r == NULL) {
        usage(argv[0]);
        MESG2("-l/-r have to assigned interface.");
        exit(EXIT_FAILURE);
    }

    if ((opt_l == opt_r) && (nm_l != NULL) && (nm_r != NULL)) {
        MESG2("-l/-r is same interface.");
        exit(EXIT_FAILURE);
    }

    /*
    tap_list = split(opt_t, ",");
    if (opt_t.size() == 0) {
        //usage(argv[0]);
        //exit(EXIT_FAILURE);
    }
    */
    for (auto i : split(opt_t, ",")) {
        //std::cout << i << std::endl;
        auto v = split(i, "@");
        size_t s = v.size();
        if (s == 1) {
            tap_list.push_back(v[0]);
            ether_addr ea_tmp;
            memset(&ea_tmp, 0xffffffff, sizeof(ea_tmp));
            dmac_list.push_back(ea_tmp);
        } else if (s == 2) {
            tap_list.push_back(v[0]);
            if (is_ether_addr((char*)v[1].c_str())) {
                dmac_list.push_back(*ether_aton(v[1].c_str()));
            } else {
                MESG2("-t is format error (ether_addr)");
                exit(EXIT_FAILURE);
            }
        } else {
            MESG2("-t is format error");
            exit(EXIT_FAILURE);
        }
    }

    for (auto it : tap_list) {
        if (!is_exist_if(if_list, it)) {
            MESG2("-t is included non exist interface.");
            exit(EXIT_FAILURE);
        }
    }

    if (is_exist_if(tap_list, opt_l)) {
        MESG2("-t is included -l interface.");
        exit(EXIT_FAILURE);
    }

    if (is_exist_if(tap_list, opt_r)) {
        MESG2("-t is included -r interface.");
        exit(EXIT_FAILURE);
    }

    /*
    std::vector<netmap*> v_nm_t;
    for (auto it : tap_list) {
        netmap* nm_tmp = new netmap();
        nm_tmp->open_if(it);
        for (int i=0; i<nm_tmp->get_tx_qnum(); i++) {
            nm_tmp->create_nmring_hard_tx(NULL, i);
        }
        v_nm_t.push_back(nm_tmp);
    }
    */
    std::vector<netmap*> v_nm_t;
    size_t s = tap_list.size();
    for (size_t j = 0; j < s; j++) {
        netmap* nm_tmp = new netmap();
        nm_tmp->open_if(tap_list[j]);
        if (! nm_tmp->open_if(tap_list[j])) {
            MESG2("could not open -t (%s)", tap_list[j].c_str());
            exit(EXIT_FAILURE);
        }
        for (int i=0; i<nm_tmp->get_tx_qnum(); i++) {
            nm_tmp->create_nmring_hard_tx(NULL, i);
        }
        nm_tmp->set_mac_dest(&dmac_list[j]);
        v_nm_t.push_back(nm_tmp);
    }

#ifdef FULLTAP
#else
    selector_init(SELECTOR_HASH_SIZE, s);
#endif

    //nm_l.dump_nmr();
    //nm_r.dump_nmr();

    if (nm_l != NULL && nm_r != NULL) {

        // inline mode
        uint16_t rx_qnum;
        uint16_t tx_qnum;

        rx_qnum = nm_l->get_rx_qnum();
        tx_qnum = nm_r->get_tx_qnum();
        pthread_t* pth_lr = (pthread_t*)malloc(sizeof(pthread_t)*rx_qnum);
        if (pth_lr == NULL) {
            PERROR("malloc");
            exit(EXIT_FAILURE);
        }
        memset(pth_lr, 0, sizeof(pthread_t)*rx_qnum);
        if (debug) printf("l2r_th:%d\n", rx_qnum);

        for (int i = 0; i < rx_qnum; i++) {
            struct thread_param* param;
            param = (struct thread_param*)malloc(sizeof(struct thread_param));
            if (param == NULL) {
                PERROR("malloc");
                exit(EXIT_FAILURE);
            }
            memset(param, 0, sizeof(struct thread_param));
            param->rx_ringid = i;
            param->tx_ringid = i % tx_qnum;
            param->nm_rx = nm_l;
            param->nm_tx = nm_r;
            param->nm_tap = &v_nm_t;
            pthread_create(&pth_lr[i], NULL, th_left_to_right, param);
        }

        rx_qnum = nm_r->get_rx_qnum();
        tx_qnum = nm_l->get_tx_qnum();
        pthread_t* pth_rl = (pthread_t*)malloc(sizeof(pthread_t)*rx_qnum);
        if (pth_rl == NULL) {
            PERROR("malloc");
            exit(EXIT_FAILURE);
        }
        memset(pth_rl, 0, sizeof(pthread_t)*rx_qnum);
        if (debug) printf("r2l_th:%d\n", rx_qnum);

        for (int i = 0; i < rx_qnum; i++) {
            struct thread_param* param;
            param = (struct thread_param*)malloc(sizeof(struct thread_param));
            if (param == NULL) {
                PERROR("malloc");
                exit(EXIT_FAILURE);
            }
            memset(param, 0, sizeof(struct thread_param));
            param->rx_ringid = i;
            param->tx_ringid = i % tx_qnum;
            param->nm_rx = nm_r;
            param->nm_tx = nm_l;
            param->nm_tap = &v_nm_t;
            pthread_create(&pth_rl[i], NULL, th_right_to_left, param);
        }

        while (1) {
            sleep(1);
#ifdef DEBUG
            show_counter(nm_l, nm_r, v_nm_t);
#endif
        }

        free(pth_lr);
        free(pth_rl);

    } else if ((nm_l == NULL && nm_r != NULL)||(nm_l != NULL && nm_r == NULL)) {

        // tap mode
        netmap* nm = NULL;
        if (nm_l != NULL) {
            nm = nm_l;
        } else if (nm_r != NULL){
            nm = nm_r;
        } else {
        }

        uint16_t rx_qnum;
        rx_qnum = nm->get_rx_qnum();
        pthread_t* pth_tap = (pthread_t*)malloc(sizeof(pthread_t)*rx_qnum);
        if (pth_tap == NULL) {
            PERROR("malloc");
            exit(EXIT_FAILURE);
        }
        memset(pth_tap, 0, sizeof(pthread_t)*rx_qnum);

        for (int i = 0; i < rx_qnum; i++) {
            struct thread_param* param;
            param = (struct thread_param*)malloc(sizeof(struct thread_param));
            if (param == NULL) {
                PERROR("malloc");
                exit(EXIT_FAILURE);
            }
            memset(param, 0, sizeof(struct thread_param));
            param->rx_ringid = i;
            param->tx_ringid = -1;
            param->nm_rx = nm;
            param->nm_tx = NULL;
            param->nm_tap = &v_nm_t;
            pthread_create(&pth_tap[i], NULL, th_tap, param);
        }

        while (1) {
            sleep(1);
#ifdef DEBUG
            show_counter(nm, NULL, v_nm_t);
#endif
        }

        free(pth_tap);

    } else {

        usage(argv[0]);
        exit(EXIT_FAILURE);

    }

    delete nm_l;
    delete nm_r;
    for (auto it : v_nm_t) {
        netmap* nm_tmp = it;
        delete nm_tmp;
    }

    return 0;
}

bool bind_cpu(int cpu, int reverse)
{
#ifdef BINDCPU
    int cpus;
    cpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (reverse) {
        cpu = (cpu - (cpus-1)) * -1;
    }

    cpuset_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(cpu%cpus, &cpuset);

    pthread_t self = pthread_self();    
    if (debug) printf("cpu %d\n", cpu);

    int retval;
    retval = pthread_setaffinity_np(self, sizeof(cpuset_t), &cpuset);

    if (retval == 0) {
        return true;
    } else {
        return false;
    }
#else
    return true;
#endif

}

void*
th_left_to_right(void* param)
{
    pthread_detach(pthread_self());

    struct thread_param* p = (struct thread_param*)param;
    bind_cpu(p->rx_ringid, 0);

    if (debug) printf("left to right\n");

    // loop
    fw_processing(p->nm_rx, p->nm_tx, p->rx_ringid, p->tx_ringid, p->nm_tap);

    free(param);
    return NULL;
}

void*
th_right_to_left(void* param)
{
    pthread_detach(pthread_self());

    struct thread_param* p = (struct thread_param*)param;
    bind_cpu(p->rx_ringid, 1);

    if (debug) printf("right to left\n");

    // loop
    fw_processing(p->nm_rx, p->nm_tx, p->rx_ringid, p->tx_ringid, p->nm_tap);

    free(param);
    return NULL;
}

void*
th_tap(void* param)
{
    pthread_detach(pthread_self());

    struct thread_param* p = (struct thread_param*)param;
    bind_cpu(p->rx_ringid, 0);

    // loop
    tap_processing(p->nm_rx, p->rx_ringid, p->nm_tap);

    free(param);
    return NULL;
}

void
tap_processing(netmap* nm, int ringid, std::vector<netmap*>* v_nm_tap)
{
    int fd;

    struct netmap_ring* rxring = NULL;
    fd = nm->create_nmring_hard_rx(&rxring, ringid);

#ifdef DEBUG
    if (debug) {
        pthread_mutex_lock(&debug_mutex);
        printf("fd    :%d\n", fd);
        printf("ringid:%d\n", ringid);
        printf("rxring:%p\n", rxring);
        pthread_mutex_unlock(&debug_mutex);
    }
#endif

    //struct ether_header* eth;
    //size_t ethlen = 0;

    std::vector<struct tap_info> v_tap_info;
    std::vector<int> v_fds;
    /*
    if (v_nm_tap->size() == 0) { 
        if (debug) printf("no tap interface\n");
        goto FUNCTAP_TAP_IF_ZERO;
    }
    */
    for (auto it : *v_nm_tap) {
        struct tap_info ti;
        memset(&ti, 0, sizeof(ti));
        ti.ringid = ringid % it->get_tx_qnum();
        ti.fd = it->get_tx_ring_info_fd(ti.ringid);
        ti.ring = it->get_tx_ring_info_ring(ti.ringid);
        ti.nm = it;
        v_tap_info.push_back(ti);
        memset(&ti, 0, sizeof(ti));
    }


#ifdef DEBUG
    if (debug) {
        pthread_mutex_lock(&debug_mutex);
        int cur = 0;
        for (auto it : v_tap_info) {
            std::cout << "tap" << cur << "_fd    :" << it.fd     << std::endl;
            std::cout << "tap" << cur << "_ringid:" << it.ringid << std::endl;
            std::cout << "tap" << cur << "_ring  :" << it.ring   << std::endl;
            cur++;
        }
        pthread_mutex_unlock(&debug_mutex);
    }
#endif

    uint32_t rx_avail = 0;
    uint32_t tap_avail = 0;

#ifdef FULLTAP //--------------------------------------------------------------

    for (;;) {

        //nm_rx->rxsync(rx_fd, rx_ringid);
        nm->rxsync_block(fd);

        rx_avail = nm->get_avail(rxring);

        while (rx_avail-- > 0) {

#ifdef DROPLLC
            if (__builtin_expect(!!(llc_filter(rxring)), 0)) { 
                nm->next(rxring);
                continue; 
            }
#endif

            for (auto it : v_tap_info) {
                it.nm->tx_ring_lock(it.ringid);

                tap_avail = it.nm->get_avail(it.ring);
                if (tap_avail == 0) {
                    it.nm->tx_ring_unlock(it.ringid);
                    continue;
                }

                pkt_copy(rxring, it.ring, 0);
                it.nm->next(it.ring);
                it.nm->tx_ring_unlock(it.ringid);
#ifdef DEBUG
                it.nm->incl_tx_ring_info_counter(it.ringid);
#endif
            }

            nm->next(rxring);

        }

        for (auto it : v_tap_info) {
            it.nm->tx_ring_lock(it.ringid);
            it.nm->txsync(it.fd, it.ringid);
            it.nm->tx_ring_unlock(it.ringid);
        }

    }

#else //FULLTAP ---------------------------------------------------------------

    int selection;
    struct tap_info* ti;
    struct ether_addr* mac;
    struct ether_header* eth;
    for (;;) {

        //nm_rx->rxsync(rx_fd, rx_ringid);
        nm->rxsync_block(fd);

        rx_avail = nm->get_avail(rxring);

        while (rx_avail-- > 0) {

#ifdef DROPLLC
            if (__builtin_expect(!!(llc_filter(rxring)), 0)) { 
                nm->next(rxring);
                continue; 
            }
#endif
            //flag:0 L3base separator
            //flag:1 L4base separator
            selection = interface_selector(rxring, v_tap_info, 1);
            if (selection != -1) {
                ti = &v_tap_info[selection];
                ti->nm->tx_ring_lock(ti->ringid);
                tap_avail = ti->nm->get_avail(ti->ring);

                if (tap_avail == 0) {
                    // XXX ToDo drop count
                    ti->nm->tx_ring_unlock(ti->ringid);
                    goto QB_TAP_SEP_SEND;
                }

                slot_swap(rxring, ti->ring);
#ifdef TAPSW
                eth = ti->nm->get_eth(ti->ring);
                mac = ETH_GDA(eth);
                *mac = *ti->nm->get_mac_dest();
                mac = ETH_GSA(eth);
                *mac = *ti->nm->get_mac();
#endif
                ti->nm->next(ti->ring);
                ti->nm->tx_ring_unlock(ti->ringid);
#ifdef DEBUG
                ti->nm->incl_tx_ring_info_counter(ti->ringid);
#endif
            }

            nm->next(rxring);

        }

        QB_TAP_SEP_SEND:
        for (auto it : v_tap_info) {
            it.nm->tx_ring_lock(it.ringid);
            it.nm->txsync(it.fd, it.ringid);
            it.nm->tx_ring_unlock(it.ringid);
        }

    }

#endif //FULLTAP --------------------------------------------------------------

    return;
}


void
fw_processing(netmap* nm_rx, netmap* nm_tx,
        int rx_ringid, int tx_ringid, std::vector<netmap*>* v_nm_tap)
{
    int tx_fd;
    int rx_fd;

    struct netmap_ring* txring = NULL;
    struct netmap_ring* rxring = NULL;

    rx_fd = nm_rx->create_nmring_hard_rx(&rxring, rx_ringid);
    tx_fd = nm_tx->create_nmring_hard_tx(&txring, tx_ringid);

#ifdef DEBUG
    if (debug) {
        pthread_mutex_lock(&debug_mutex);
        printf("tx_pointer:%p\n", txring);
        printf("rx_pointer:%p\n", rxring);
        printf("tx_fd:%d\n", tx_fd);
        printf("rx_fd:%d\n", rx_fd);
        printf("tx_ringid:%d\n", tx_ringid);
        printf("rx_ringid:%d\n", rx_ringid);
        pthread_mutex_unlock(&debug_mutex);
    }
#endif

    std::vector<struct tap_info> v_tap_info;
    std::vector<int> v_fds;

    for (auto it : *v_nm_tap) {
        struct tap_info ti;
        memset(&ti, 0, sizeof(ti));
        ti.ringid = rx_ringid % it->get_tx_qnum();
        ti.fd = it->get_tx_ring_info_fd(ti.ringid);
        ti.ring = it->get_tx_ring_info_ring(ti.ringid);
        ti.nm = it;
        v_tap_info.push_back(ti);
        memset(&ti, 0, sizeof(ti));
    }

#ifdef DEBUG
    if (debug) {
        pthread_mutex_lock(&debug_mutex);
        int cur = 0;
        for (auto it : v_tap_info) {
            std::cout << "tap" << cur << "_fd    :" << it.fd     << std::endl;
            std::cout << "tap" << cur << "_ringid:" << it.ringid << std::endl;
            std::cout << "tap" << cur << "_ring  :" << it.ring   << std::endl;
            cur++;
        }
        pthread_mutex_unlock(&debug_mutex);
    }
#endif

    uint32_t rx_avail = 0;
    uint32_t tx_avail = 0;
    uint32_t tap_avail = 0;

#ifdef FULLTAP //--------------------------------------------------------------

    int burst;
    for (;;) {

        //nm_rx->rxsync(rx_fd, rx_ringid);
        nm_rx->rxsync_block(rx_fd);

        rx_avail = nm_rx->get_avail(rxring);
        tx_avail = nm_tx->get_avail(txring);

        burst = (rx_avail <= tx_avail) ?  rx_avail : tx_avail;
        while (burst-- > 0) {

#ifdef DROPLLC
            if (__builtin_expect(!!(llc_filter(rxring)), 0)) { 
                nm_rx->next(rxring);
                continue;
            }
#endif

            for (auto it : v_tap_info) {
                it.nm->tx_ring_lock(it.ringid);
                tap_avail = it.nm->get_avail(it.ring);

                //std::cout << it.nm->get_ifname() << std::endl;
                //std::cout << it.ring->avail << std::endl;

                if (tap_avail == 0) {
                    // XXX ToDo drop count
                    it.nm->tx_ring_unlock(it.ringid);
                    continue;
                }

                pkt_copy(rxring, it.ring, 0);
                it.nm->next(it.ring);
                it.nm->tx_ring_unlock(it.ringid);
#ifdef DEBUG
                it.nm->incl_tx_ring_info_counter(it.ringid);
#endif
            }

            slot_swap(rxring, txring);

            nm_tx->next(txring);
            nm_rx->next(rxring);
#ifdef DEBUG
            nm_tx->incl_tx_ring_info_counter(tx_ringid);
            nm_rx->incl_rx_ring_info_counter(rx_ringid);
#endif
        }

        nm_tx->txsync(tx_fd, tx_ringid);
        for (auto it : v_tap_info) {
            it.nm->tx_ring_lock(it.ringid);
            it.nm->txsync(it.fd, it.ringid);
            it.nm->tx_ring_unlock(it.ringid);
        }

    }

#else //FULLTAP ---------------------------------------------------------------

    int burst;
    int selection;
    struct tap_info* ti;
    struct ether_addr* mac;
    struct ether_header* eth;
    for (;;) {

        //nm_rx->rxsync(rx_fd, rx_ringid);
        nm_rx->rxsync_block(rx_fd);

        rx_avail = nm_rx->get_avail(rxring);
        tx_avail = nm_tx->get_avail(txring);
        
        burst = (rx_avail <= tx_avail) ?  rx_avail : tx_avail;
        while (burst-- > 0) {

#ifdef DROPLLC
            if (__builtin_expect(!!(llc_filter(rxring)), 0)) { 
                nm_rx->next(rxring);
                continue; 
            }
#endif
            selection = interface_selector(rxring, v_tap_info, 1);
            if (selection != -1) {
                ti = &v_tap_info[selection];
                ti->nm->tx_ring_lock(ti->ringid);
                tap_avail = ti->nm->get_avail(ti->ring);

                if (tap_avail == 0) {
                    // XXX ToDo drop count
                    ti->nm->tx_ring_unlock(ti->ringid);
                    goto QB_FW_SEP_SEND;
                }

                pkt_copy(rxring, ti->ring, 0);
#ifdef TAPSW
                eth = ti->nm->get_eth(ti->ring);
                mac = ETH_GDA(eth);
                *mac = *ti->nm->get_mac_dest();
                mac = ETH_GSA(eth);
                *mac = *ti->nm->get_mac();
#endif
                ti->nm->next(ti->ring);
                ti->nm->tx_ring_unlock(ti->ringid);
#ifdef DEBUG
                ti->nm->incl_tx_ring_info_counter(ti->ringid);
#endif

            }

            slot_swap(rxring, txring);

            nm_tx->next(txring);
            nm_rx->next(rxring);
#ifdef DEBUG
            nm_tx->incl_tx_ring_info_counter(tx_ringid);
            nm_rx->incl_rx_ring_info_counter(rx_ringid);
#endif
        }
            
        QB_FW_SEP_SEND:
        nm_tx->txsync(tx_fd, tx_ringid);
        if(v_tap_info.size() != 0) {
            for (auto it : v_tap_info) {
                it.nm->tx_ring_lock(it.ringid);
                it.nm->txsync(it.fd, it.ringid);
                it.nm->tx_ring_unlock(it.ringid);
            }
        }

    }

#endif //FULLTAP --------------------------------------------------------------

    return;
}


static inline void
slot_swap(struct netmap_ring* rxring, struct netmap_ring* txring)
{
    struct netmap_slot* rx_slot = 
         ((netmap_slot*)&rxring->slot[rxring->cur]);

    struct netmap_slot* tx_slot = 
         ((netmap_slot*)&txring->slot[txring->cur]);

#ifdef USERLAND_COPY

    struct ether_header* rx_eth = 
        (struct ether_header*)NETMAP_BUF(rxring, rx_slot->buf_idx);
    struct ether_header* tx_eth = 
        (struct ether_header*)NETMAP_BUF(txring, tx_slot->buf_idx);
    memcpy(tx_eth, rx_eth, rx_slot->len);

#else

    uint32_t buf_idx;
    buf_idx = tx_slot->buf_idx;
    tx_slot->buf_idx = rx_slot->buf_idx;
    rx_slot->buf_idx = buf_idx;
    tx_slot->flags |= NS_BUF_CHANGED;
    rx_slot->flags |= NS_BUF_CHANGED;

#endif

    tx_slot->len = rx_slot->len;

    /*
    if (debug) printf("------\n");
    if (debug) memdump(rx_eth, rx_slot->len);
    */

    return;
}

static inline void
pkt_copy(struct netmap_ring* rxring, struct netmap_ring* txring, int flag)
{
    if (flag == 0) {

        #define pkt_copy_likely(x)       __builtin_expect(!!(x), 1)
        #define pkt_copy_unlikely(x)       __builtin_expect(!!(x), 0)

        struct netmap_slot* rx_slot = 
             ((netmap_slot*)&rxring->slot[rxring->cur]);

        struct netmap_slot* tx_slot = 
             ((netmap_slot*)&txring->slot[txring->cur]);

        uint64_t* src = (uint64_t*)(NETMAP_BUF(rxring, rx_slot->buf_idx));
        uint64_t* dst = (uint64_t*)(NETMAP_BUF(txring, tx_slot->buf_idx));
        int l = rx_slot->len;

        if (pkt_copy_unlikely(l >= 1024)) {
            bcopy(src, dst, l);
        } else {
            for (; l >= 0; l-=32) {
                *dst++ = *src++;
                *dst++ = *src++;
                *dst++ = *src++;
                *dst++ = *src++;
                *dst++ = *src++;
                *dst++ = *src++;
                *dst++ = *src++;
                *dst++ = *src++;
            }
        }
        tx_slot->len = rx_slot->len;

        #undef pkt_copy_likely
        #undef pkt_copy_unlikely

    } else if (flag == 1) {

        // dont working...

        struct netmap_slot* rx_slot = 
             ((netmap_slot*)&rxring->slot[rxring->cur]);
        struct netmap_slot* tx_slot = 
             ((netmap_slot*)&txring->slot[txring->cur]);
        char* src = NETMAP_BUF(rxring, tx_slot->buf_idx);
        //char* dst = NETMAP_BUF(txring, tx_slot->buf_idx);
        tx_slot->flags |= NS_INDIRECT;
        tx_slot->ptr = (uint64_t)src;
        tx_slot->len = rx_slot->len;

    }
    return;
}

#ifdef DROPLLC
inline bool
llc_filter(struct netmap_ring* ring) {
        struct netmap_slot* slot =
             ((netmap_slot*)&ring->slot[ring->cur]);
        struct ether_header* eth =
            (struct ether_header*)(NETMAP_BUF(ring, slot->buf_idx));

        uint16_t type = htons(eth->ether_type);
        if (__builtin_expect(!!(type>=0x05DC), 1)) {
            return false; //pass through
        }

        return true; //block out
}
#endif

#ifdef DEBUG
void show_counter(netmap* l, netmap* r, std::vector<netmap*>& v)
{
    /*
    // XXX ToDo
    int l_qnum = l->get_rx_qnum();
    int r_qnum = r->get_rx_qnum();
    for (auto it : v_nm_tap) {
    }
    */
    return;
}
#endif

void
usage(char* prog_name)
{
    printf("%s\n", prog_name);
    printf("  Must..\n");
    printf("    -l [ifname] (left interface)\n");
    printf("    -r [ifname] (right interface)\n");
#ifdef TAPSW
    printf("    -t [ifname(@mac_addr),ifname(@mac_addr),ifname(@mac_addr)...])\n");
#else
    printf("    -t [ifname,ifname,ifname...])\n");
#endif
    printf("  Option..\n");
    printf("    -h/? : help usage information\n");
#ifdef DEBUG
    printf("    -v : verbose mode\n");
#endif
    printf("\n");
    return;
}

