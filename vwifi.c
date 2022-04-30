#include <linux/module.h>

#include <linux/hash.h>
#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("virtual cfg80211 driver");

#define WIPHY_NAME "owl" /* Our WireLess */
#define NDEV_NAME WIPHY_NAME "%d"
#define SINKDEV_NAME NDEV_NAME "sink"
#define NDEV_NUMS 2

/* According to 802.11 Standard, SSID has max len 32. */
#define SSID_MAX_LENGTH 32

#ifndef DEFAULT_SSID_LIST
#define DEFAULT_SSID_LIST "[MyHomeWiFi]"
#endif

#define SCAN_TIMEOUT_MS 100

struct owl_packet {
    int datalen;
    u8 data[ETH_DATA_LEN];
    struct list_head list;
};

struct owl_context {
    struct wiphy *wiphy;
    struct mutex lock;
    struct work_struct ws_connect, ws_disconnect;
    char connecting_ssid[SSID_MAX_LENGTH + 1]; /* plus one for '\0' */
    u8 connecting_bssid[ETH_ALEN];
    u16 disconnect_reason_code;
    struct work_struct ws_scan, ws_scan_timeout;
    struct timer_list scan_timeout;
    struct cfg80211_scan_request *scan_request;
    /* List head for maintain multiple network device private context*/
    struct list_head netintf_list;
};

struct owl_wiphy_priv_context {
    struct owl_context *owl;
};

/*
 * Network Device Private Data Context
 */
struct owl_ndev_priv_context {
    struct owl_context *owl;
    struct wireless_dev wdev;
    struct net_device *ndev;
    struct net_device_stats stats;
    /* Head of received packet queue */
    struct list_head rx_queue;
    /* List entry for maintaining multiple net device private data in
     * owl_context.netintf_list.
     * In owl_create_context function, two net_device will be created.
     * one is owl0, another is owl0sink.
     * `owl0` is for wifi STATION.
     * `owl0sink` will be simulated as a mock for redirecting packet to kernel.
     */
    struct list_head list;
};

/*
 * AP information table entry.
 */
struct ap_info_entry_t {
    struct hlist_node node;
    u8 bssid[ETH_ALEN];
    char ssid[SSID_MAX_LENGTH];
};

static char *ssid_list = DEFAULT_SSID_LIST;
module_param(ssid_list, charp, 0644);
MODULE_PARM_DESC(ssid_list, "Self-defined SSIDs.");

/* AP Database */
static DECLARE_HASHTABLE(ssid_table, 4);

/* helper function to retrieve main context from "priv" data of the wiphy */
static inline struct owl_wiphy_priv_context *wiphy_get_owl_context(
    struct wiphy *wiphy)
{
    return (struct owl_wiphy_priv_context *) wiphy_priv(wiphy);
}

/* helper function to retrieve main context from "priv" data of network dev */
static inline struct owl_ndev_priv_context *ndev_get_owl_context(
    struct net_device *ndev)
{
    return (struct owl_ndev_priv_context *) netdev_priv(ndev);
}

static s32 rand_int(s32 low, s32 up)
{
    s32 result = (s32) get_random_u32();
    result %= (up - low + 1);
    result = abs(result);
    result += low;
    return result;
}

/*
 * Murmur hash.  See https://stackoverflow.com/a/57960443
 */
static inline uint64_t murmurhash(const char *str)
{
    uint64_t h = 525201411107845655ull;
    for (; *str; ++str) {
        h ^= *str;
        h *= 0x5bd1e9955bd1e995;
        h ^= h >> 47;
    }
    return h;
}

/* Helper function for generating BSSID from SSID */
static void generate_bssid_with_ssid(u8 *result, const char *ssid)
{
    u64_to_ether_addr(murmurhash(ssid), result);
    result[0] &= 0xfe; /* clear multicast bit */
    result[0] |= 0x02; /* set local assignment bit */
}

/*
 * Update AP database from module parameter ssid_list
 */
static void update_ssids(const char *ssid_list)
{
    struct ap_info_entry_t *ap;
    const char delims[] = "[]";
    struct hlist_node *tmp;

    for (char *s = (char *) ssid_list; *s != 0; /*empty*/) {
        bool ssid_exist = false;
        char token[SSID_MAX_LENGTH] = {0};
        /* Get the number of token separator characters. */
        size_t n = strspn(s, delims);
        /* Actually skip the separators now. */
        s += n;
        /* Get the number of token (non-separator) characters. */
        n = strcspn(s, delims);
        if (n == 0)  // token not found
            continue;
        strncpy(token, s, n);
        /* Point the next token. */
        s += n;
        /* Insert the SSID into hash*/
        token[n] = '\0';
        u32 key = murmurhash((char *) token);
        hash_for_each_possible_safe (ssid_table, ap, tmp, node, key) {
            if (strncmp(token, ap->ssid, n) == 0) {
                ssid_exist = true;
                break;
            }
        }
        if (ssid_exist)  // SSID exist
            continue;
        ap = kzalloc(sizeof(struct ap_info_entry_t), GFP_KERNEL);
        if (!ap) {
            pr_err("Failed to alloc ap_info_entry_t incomming SSID=%s\n",
                   token);
            return;
        }
        u8 bssid[ETH_ALEN] = {0};
        strncpy(ap->ssid, token, n);
        generate_bssid_with_ssid(bssid, token);
        memcpy(ap->bssid, bssid, ETH_ALEN);
        hash_add(ssid_table, &ap->node, key);
    }
}

/* Helper function that will prepare structure with self-defined BSS information
 * and "inform" the kernel about "new" BSS Most of the code are copied from the
 * upcoming inform_dummy_bss function.
 */
static void inform_dummy_bss(struct owl_context *owl)
{
    struct ap_info_entry_t *ap;
    int i;
    struct hlist_node *tmp;

    update_ssids(ssid_list);
    if (hash_empty(ssid_table))
        return;
    hash_for_each_safe (ssid_table, i, tmp, ap, node) {
        struct cfg80211_bss *bss = NULL;
        struct cfg80211_inform_bss data = {
            /* the only channel */
            .chan = &owl->wiphy->bands[NL80211_BAND_2GHZ]->channels[0],
            .scan_width = NL80211_BSS_CHAN_WIDTH_20,
            .signal = rand_int(-100, -30) * 100,
        };

        size_t ssid_len = strlen(ap->ssid);
        u8 *ie = kmalloc((ssid_len + 3) * sizeof(ie), GFP_KERNEL);
        ie[0] = WLAN_EID_SSID;
        ie[1] = ssid_len;
        memcpy(ie + 2, ap->ssid, ssid_len);

        /* Using CLOCK_BOOTTIME clock, which won't be affected by
         * changes in system time-of-day clock, and includes any time
         * that the system is suspended. Thus, it's suitable for
         * tsf to synchronize the machines in BSS.
         */
        u64 tsf = div_u64(ktime_get_boottime_ns(), 1000);

        /* It is posible to use cfg80211_inform_bss() instead. */
        bss = cfg80211_inform_bss_data(
            owl->wiphy, &data, CFG80211_BSS_FTYPE_UNKNOWN, ap->bssid, tsf,
            WLAN_CAPABILITY_ESS, 100, ie, ssid_len + 2, GFP_KERNEL);

        /* cfg80211_inform_bss_data() returns cfg80211_bss structure referefence
         * counter of which should be decremented if it is unused.
         */
        cfg80211_put_bss(owl->wiphy, bss);
        kfree(ie);
    }
}

/* Informs the "dummy" BSS to kernel, and calls cfg80211_scan_done() to finish
 * scan. */
static void owl_scan_timeout_work(struct work_struct *w)
{
    struct owl_context *owl =
        container_of(w, struct owl_context, ws_scan_timeout);
    struct cfg80211_scan_info info = {
        /* if scan was aborted by user (calling cfg80211_ops->abort_scan) or by
         * any driver/hardware issue - field should be set to "true"
         */
        .aborted = false,
    };

    /* inform with dummy BSS */
    inform_dummy_bss(owl);

    if (mutex_lock_interruptible(&owl->lock))
        return;

    /* finish scan */
    cfg80211_scan_done(owl->scan_request, &info);

    owl->scan_request = NULL;

    mutex_unlock(&owl->lock);
}

/* Callback called when the scan timer timeouts. This function just schedules
 * the timeout work and offloads the job of informing "dummy" BSS to kernel
 * onto it.
 */
static void owl_scan_timeout(struct timer_list *t)
{
    struct owl_context *owl = container_of(t, struct owl_context, scan_timeout);

    if (owl->scan_request)
        schedule_work(&owl->ws_scan_timeout);
}

/* "Scan routine". It simulates a fake BSS scan (In fact, do nothing.), and sets
 * a scan timer to start from then. Once the timer timeouts, the timeout
 * routine owl_scan_timeout() will be invoked, which schedules a timeout work,
 * and the timeout work will inform the kernel about "dummy" BSS and finish the
 * scan.
 */
static void owl_scan_routine(struct work_struct *w)
{
    struct owl_context *owl = container_of(w, struct owl_context, ws_scan);

    /* In real world driver, we scan BSS here. But viwifi doesn't, because we
     * already store dummy BSS in ssid hash table. So we just set a scan timeout
     * after specific jiffies, and inform "dummy" BSS to kernel and call
     * cfg80211_scan_done() by timeout worker.
     */
    mod_timer(&owl->scan_timeout, jiffies + msecs_to_jiffies(SCAN_TIMEOUT_MS));
}

/* It checks SSID of the ESS to connect and informs the kernel that connection
 * is finished. It should call cfg80211_connect_bss() when connect is finished
 * or cfg80211_connect_timeout() when connect is failed. This module can connect
 * only to ESS with SSID equal to SSID_DUMMY value.
 * This routine is called through workqueue, when the kernel asks to connect
 * through cfg80211_ops.
 */
static bool is_valid_ssid(const char *connecting_ssid)
{
    bool is_valid = false;
    struct ap_info_entry_t *ap;
    struct hlist_node *tmp;
    u32 key = murmurhash((char *) connecting_ssid);
    hash_for_each_possible_safe (ssid_table, ap, tmp, node, key) {
        if (!strcmp(connecting_ssid, ap->ssid)) {
            is_valid = true;
            break;
        }
    }
    return is_valid;
}

static void get_connecting_bssid(const char *connecting_ssid,
                                 u8 *connecting_bssid)
{
    struct ap_info_entry_t *ap;
    struct hlist_node *tmp;
    u32 key = murmurhash((char *) connecting_ssid);
    hash_for_each_possible_safe (ssid_table, ap, tmp, node, key) {
        if (!strcmp(connecting_ssid, ap->ssid)) {
            memcpy(connecting_bssid, ap->bssid, ETH_ALEN);
            break;
        }
    }
}

static void owl_connect_routine(struct work_struct *w)
{
    struct owl_context *owl = container_of(w, struct owl_context, ws_connect);
    struct owl_ndev_priv_context *item = NULL;
    if (mutex_lock_interruptible(&owl->lock))
        return;
    if (!is_valid_ssid(owl->connecting_ssid)) {
        item = list_first_entry(&owl->netintf_list,
                                struct owl_ndev_priv_context, list);
        cfg80211_connect_timeout(item->ndev, NULL, NULL, 0, GFP_KERNEL,
                                 NL80211_TIMEOUT_SCAN);
    } else {
        /* First network device always is owl0 station */
        item = list_first_entry(&owl->netintf_list,
                                struct owl_ndev_priv_context, list);
        /* It is possible to use cfg80211_connect_result() or
         * cfg80211_connect_done()
         */
        cfg80211_connect_result(item->ndev, NULL, NULL, 0, NULL, 0,
                                WLAN_STATUS_SUCCESS, GFP_KERNEL);
    }
    owl->connecting_ssid[0] = 0;

    mutex_unlock(&owl->lock);
}

/* Invoke cfg80211_disconnected() that informs the kernel that disconnect is
 * complete. Overall disconnect may call cfg80211_connect_timeout() if
 * disconnect interrupting connection routine, but for this module let's keep
 * it simple as possible. This routine is called through workqueue, when the
 * kernel asks to disconnect through cfg80211_ops.
 */
static void owl_disconnect_routine(struct work_struct *w)
{
    struct owl_context *owl =
        container_of(w, struct owl_context, ws_disconnect);
    struct owl_ndev_priv_context *item = NULL;
    if (mutex_lock_interruptible(&owl->lock))
        return;
    item = list_first_entry(&owl->netintf_list, struct owl_ndev_priv_context,
                            list);
    cfg80211_disconnected(item->ndev, owl->disconnect_reason_code, NULL, 0,
                          true, GFP_KERNEL);
    owl->disconnect_reason_code = 0;

    mutex_unlock(&owl->lock);
}

/* callback called by the kernel when user decided to scan.
 * This callback should initiate scan routine(through work_struct) and exit with
 * 0 if everything is ok.
 */
static int owl_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
    struct owl_context *owl = wiphy_get_owl_context(wiphy)->owl;

    if (mutex_lock_interruptible(&owl->lock))
        return -ERESTARTSYS;

    if (owl->scan_request) {
        mutex_unlock(&owl->lock);
        return -EBUSY;
    }
    owl->scan_request = request;

    mutex_unlock(&owl->lock);

    if (!schedule_work(&owl->ws_scan))
        return -EBUSY;
    return 0;
}

/* callback called by the kernel when there is need to "connect" to some
 * network. It initializes connection routine through work_struct and exits
 * with 0 if everything is ok. connect routine should be finished with
 * cfg80211_connect_bss()/cfg80211_connect_result()/cfg80211_connect_done() or
 * cfg80211_connect_timeout().
 */
static int owl_connect(struct wiphy *wiphy,
                       struct net_device *dev,
                       struct cfg80211_connect_params *sme)
{
    struct owl_context *owl = wiphy_get_owl_context(wiphy)->owl;
    size_t ssid_len =
        sme->ssid_len > SSID_MAX_LENGTH ? SSID_MAX_LENGTH : sme->ssid_len;

    if (mutex_lock_interruptible(&owl->lock))
        return -ERESTARTSYS;

    memcpy(owl->connecting_ssid, sme->ssid, ssid_len);
    owl->connecting_ssid[ssid_len] = 0;
    get_connecting_bssid(owl->connecting_ssid, owl->connecting_bssid);
    mutex_unlock(&owl->lock);

    if (!schedule_work(&owl->ws_connect))
        return -EBUSY;
    return 0;
}

/* callback called by the kernel when there is need to "diconnect" from
 * currently connected network. It initializes disconnect routine through
 * work_struct and exits with 0 if everything ok. disconnect routine should
 * call cfg80211_disconnected() to inform the kernel that disconnection is
 * complete.
 */
static int owl_disconnect(struct wiphy *wiphy,
                          struct net_device *dev,
                          u16 reason_code)
{
    struct owl_context *owl = wiphy_get_owl_context(wiphy)->owl;

    if (mutex_lock_interruptible(&owl->lock))
        return -ERESTARTSYS;

    owl->disconnect_reason_code = reason_code;

    mutex_unlock(&owl->lock);

    if (!schedule_work(&owl->ws_disconnect))
        return -EBUSY;

    return 0;
}

/**
 * Called when rtnl lock was acquired.
 */
static int owl_get_station(struct wiphy *wiphy,
                           struct net_device *dev,
                           const u8 *mac,
                           struct station_info *sinfo)
{
    /* TODO: Get station infomation */
    return 0;
}

/* Structure of functions for FullMAC 80211 drivers.
 * Functions implemented along with fields/flags in wiphy structure would
 * represent drivers features. This module can only perform "scan" and
 * "connect". Some functions cant be implemented alone, for example: with
 * "connect" there is should be function "disconnect".
 */
static struct cfg80211_ops owl_cfg_ops = {
    .scan = owl_scan,
    .connect = owl_connect,
    .disconnect = owl_disconnect,
    .get_station = owl_get_station,
};

static int owl_ndo_open(struct net_device *dev)
{
    /* The first byte is '\0' to avoid being a multicast
     * address (the first byte of multicast addrs is odd).
     */
    char intf_name[ETH_ALEN + 1] = {0};
    snprintf(&intf_name[1], ETH_ALEN, "%s", dev->name);
    memcpy(dev->dev_addr, intf_name, ETH_ALEN);
    netif_start_queue(dev);
    return 0;
}

static int owl_ndo_stop(struct net_device *dev)
{
    struct owl_ndev_priv_context *np = ndev_get_owl_context(dev);
    struct owl_packet *pkt, *is = NULL;
    list_for_each_entry_safe (pkt, is, &np->rx_queue, list) {
        list_del(&pkt->list);
        kfree(pkt);
    }
    netif_stop_queue(dev);
    return 0;
}

static struct net_device_stats *owl_ndo_get_stats(struct net_device *dev)
{
    struct owl_ndev_priv_context *np = ndev_get_owl_context(dev);
    return &np->stats;
}

/*
 * Receive a packet: retrieve, encapsulate and pass over to upper levels
 */
static void owl_handle_rx(struct net_device *dev)
{
    struct owl_ndev_priv_context *np = ndev_get_owl_context(dev);
    struct sk_buff *skb;
    char prefix[16];
    struct owl_packet *pkt;
    if (list_empty(&np->rx_queue)) {
        printk(KERN_NOTICE "owl rx: No packet in rx_queue\n");
        return;
    }
    pkt = list_first_entry(&np->rx_queue, struct owl_packet, list);
    snprintf(prefix, 16, "%s Rx ", dev->name);
    print_hex_dump(KERN_DEBUG, prefix, DUMP_PREFIX_OFFSET, 16, 1, pkt->data,
                   pkt->datalen, false);
    /* Put raw packet into socket buffer */
    skb = dev_alloc_skb(pkt->datalen + 2);
    if (!skb) {
        printk(KERN_NOTICE "owl rx: low on mem - packet dropped\n");
        np->stats.rx_dropped++;
        goto pkt_free;
    }
    skb_reserve(skb, 2); /* align IP on 16B boundary */
    memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);
    /* Write metadata, and then pass to the receive level */
    skb->dev = dev;
    skb->protocol = eth_type_trans(skb, dev);
    skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */
    netif_rx(skb);
    np->stats.rx_packets++;
    np->stats.rx_bytes += pkt->datalen;
pkt_free:
    list_del(&pkt->list);
    kfree(pkt);
}

/* Network packet transmit.
 * Callback called by the kernel when packet of data should be sent.
 * In this example it does nothing.
 */
static netdev_tx_t owl_ndo_start_xmit(struct sk_buff *skb,
                                      struct net_device *dev)
{
    struct owl_ndev_priv_context *np = ndev_get_owl_context(dev);
    struct owl_context *owl = ndev_get_owl_context(dev)->owl;
    struct owl_ndev_priv_context *dest_np = NULL;
    /* Don't forget to cleanup skb, as its ownership moved to xmit callback. */
    np->stats.tx_packets++;
    np->stats.tx_bytes += skb->len;
    struct owl_packet *pkt;

    pkt = kmalloc(sizeof(struct owl_packet), GFP_KERNEL);
    if (!pkt) {
        printk(KERN_NOTICE "Ran out of memory allocating packet pool\n");
        return NETDEV_TX_OK;
    }
    memcpy(&pkt->data, skb->data, skb->len);
    pkt->datalen = skb->len;
    dest_np =
        list_last_entry(&owl->netintf_list, struct owl_ndev_priv_context, list);
    if (dest_np->ndev == dev)
        dest_np = list_first_entry(&owl->netintf_list,
                                   struct owl_ndev_priv_context, list);
    char prefix[16];
    snprintf(prefix, 16, "%s Tx ", dev->name);
    print_hex_dump(KERN_DEBUG, prefix, DUMP_PREFIX_OFFSET, 16, 1, pkt->data,
                   pkt->datalen, false);
    /* enqueue to destination */
    list_add_tail(&pkt->list, &dest_np->rx_queue);
    kfree_skb(skb);
    /* Directly send to rx_queue, simulate the rx interrupt*/
    owl_handle_rx(dest_np->ndev);
    return NETDEV_TX_OK;
}

/* Structure of functions for network devices.
 * It should have at least ndo_start_xmit functions called for packet to be
 * sent.
 */
static struct net_device_ops owl_ndev_ops = {
    .ndo_open = owl_ndo_open,
    .ndo_stop = owl_ndo_stop,
    .ndo_start_xmit = owl_ndo_start_xmit,
    .ndo_get_stats = owl_ndo_get_stats,
};

/* Array of "supported" channels in 2GHz band. It is required for wiphy.
 * For demo - the only channel 6.
 */
static struct ieee80211_channel owl_supported_channels_2ghz[] = {
    {
        .band = NL80211_BAND_2GHZ,
        .hw_value = 6,
        .center_freq = 2437,
    },
};

/* Array of supported rates, required to support at least those next rates
 * for 2GHz band.
 */
static struct ieee80211_rate owl_supported_rates_2ghz[] = {
    {
        .bitrate = 10,
        .hw_value = 0x1,
    },
    {
        .bitrate = 20,
        .hw_value = 0x2,
    },
    {
        .bitrate = 55,
        .hw_value = 0x4,
    },
    {
        .bitrate = 110,
        .hw_value = 0x8,
    },
};

/* Describes supported band of 2GHz. */
static struct ieee80211_supported_band nf_band_2ghz = {
    /* FIXME: add other band capabilities if nedded, such as 40 width */
    .ht_cap.cap = IEEE80211_HT_CAP_SGI_20,
    .ht_cap.ht_supported = false,

    .channels = owl_supported_channels_2ghz,
    .n_channels = ARRAY_SIZE(owl_supported_channels_2ghz),

    .bitrates = owl_supported_rates_2ghz,
    .n_bitrates = ARRAY_SIZE(owl_supported_rates_2ghz),
};

void owl_ndev_priv_setup_helper(struct owl_ndev_priv_context *ndev_data)
{
    /* fill wireless_dev context.
     * wireless_dev with net_device can be represented as inherited class of
     * single net_device.
     */

    ndev_data->wdev.wiphy = ndev_data->owl->wiphy;
    ndev_data->wdev.netdev = ndev_data->ndev;
    ndev_data->wdev.iftype = NL80211_IFTYPE_STATION;
    ndev_data->ndev->ieee80211_ptr = &ndev_data->wdev;

    /* set network device hooks. should implement ndo_start_xmit() at least
     */

    ndev_data->ndev->netdev_ops = &owl_ndev_ops;

    /* Add here proper net_device initialization. */
    ndev_data->ndev->features |= NETIF_F_HW_CSUM;
    /* Initialize rx_queue */
    INIT_LIST_HEAD(&ndev_data->rx_queue);
}

/* Creates wiphy context and net_device with wireless_dev.
 * wiphy/net_device/wireless_dev is basic interfaces for the kernel to interact
 * with driver as wireless one. It returns driver's main "owl" context.
 */
static struct owl_context *owl_create_context(void)
{
    struct owl_context *ret = NULL;
    struct owl_wiphy_priv_context *wiphy_data = NULL;
    struct owl_ndev_priv_context *ndev_data = NULL;

    /* allocate for owl context */
    ret = kmalloc(sizeof(*ret), GFP_KERNEL);
    if (!ret)
        goto l_error;

    /* allocate wiphy context. It is possible just to use wiphy_new().
     * wiphy should represent physical FullMAC wireless device. One wiphy can
     * have serveral network interfaces - for that, we need to implement
     * add_virtual_intf() from cfg80211_ops.
     */
    ret->wiphy = wiphy_new_nm(
        &owl_cfg_ops, sizeof(struct owl_wiphy_priv_context), WIPHY_NAME);
    if (!ret->wiphy)
        goto l_error_wiphy;

    /* save owl context in wiphy private data. */
    wiphy_data = wiphy_get_owl_context(ret->wiphy);
    wiphy_data->owl = ret;

    /* FIXME: set device object as wiphy "parent" */
    /* set_wiphy_dev(ret->wiphy, dev); */

    /* wiphy should determinate its type.
     * add other required types like  "BIT(NL80211_IFTYPE_STATION) |
     * BIT(NL80211_IFTYPE_AP)" etc.
     */
    ret->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);

    /* wiphy should have at least 1 band.
     * Also fill NL80211_BAND_5GHZ if required. In this module, only 1 band
     * with 1 "channel"
     */
    ret->wiphy->bands[NL80211_BAND_2GHZ] = &nf_band_2ghz;

    /* scan - if the device supports "scan", we need to define max_scan_ssids
     * at least.
     */
    ret->wiphy->max_scan_ssids = 69;

    /* Signal type
     * CFG80211_SIGNAL_TYPE_UNSPEC allows us specify signal strength from 0 to
     * 100. The reasonable value for CFG80211_SIGNAL_TYPE_MBM is -3000 to -10000
     * (mdBm).
     */
    ret->wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;

    ret->wiphy->flags |= WIPHY_FLAG_NETNS_OK;

    /* register wiphy, if everything ok - there should be another wireless
     * device in system. use command: $ iw list
     * Wiphy owl
     */
    if (wiphy_register(ret->wiphy) < 0)
        goto l_error_wiphy_register;

    INIT_LIST_HEAD(&ret->netintf_list);
    for (int i = 0; i < NDEV_NUMS; i++) {
        /* allocate network device context. */
        struct net_device *ndev = NULL;
        ndev = alloc_netdev(sizeof(struct owl_ndev_priv_context),
                            i == 0 ? NDEV_NAME : SINKDEV_NAME, NET_NAME_ENUM,
                            ether_setup);
        if (!ndev)
            goto l_error_alloc_ndev;
        /* fill private data of network context. */
        ndev_data = ndev_get_owl_context(ndev);
        ndev_data->owl = ret;
        ndev_data->ndev = ndev;
        owl_ndev_priv_setup_helper(ndev_data);

        /* register network device. If everything is ok, there should be new
         * network device: $ ip a owl0: <BROADCAST,MULTICAST> mtu 1500 qdisc
         * noop state DOWN group default link/ether 00:00:00:00:00:00 brd
         * ff:ff:ff:ff:ff:ff
         */

        if (register_netdev(ndev_data->ndev))
            goto l_error_ndev_register;
        list_add_tail(&ndev_data->list, &ret->netintf_list);
    }

    return ret;

l_error_ndev_register:
    list_for_each_entry (ndev_data, &ret->netintf_list, list) {
        free_netdev(ndev_data->ndev);
        list_del(&ndev_data->list);
    }
l_error_alloc_ndev:
    wiphy_unregister(ret->wiphy);
l_error_wiphy_register:
    wiphy_free(ret->wiphy);
l_error_wiphy:
    kfree(ret);
l_error:
    return NULL;
}

static void owl_free(struct owl_context *ctx)
{
    struct owl_ndev_priv_context *item = NULL;
    if (!ctx)
        return;
    if (list_empty(&ctx->netintf_list)) {
        printk(KERN_NOTICE "owl netintf: No interfcae found in netintf_list\n");
        return;
    }
    list_for_each_entry (item, &ctx->netintf_list, list) {
        unregister_netdev(item->ndev);
        free_netdev(item->ndev);
    }
    wiphy_unregister(ctx->wiphy);
    wiphy_free(ctx->wiphy);
    kfree(ctx);
}

static struct owl_context *g_ctx = NULL;

static int __init vwifi_init(void)
{
    g_ctx = owl_create_context();
    if (!g_ctx)
        return 1;

    mutex_init(&g_ctx->lock);

    INIT_WORK(&g_ctx->ws_connect, owl_connect_routine);
    g_ctx->connecting_ssid[0] = 0;
    INIT_WORK(&g_ctx->ws_disconnect, owl_disconnect_routine);
    g_ctx->disconnect_reason_code = 0;
    INIT_WORK(&g_ctx->ws_scan, owl_scan_routine);
    g_ctx->scan_request = NULL;
    INIT_WORK(&g_ctx->ws_scan_timeout, owl_scan_timeout_work);
    timer_setup(&g_ctx->scan_timeout, owl_scan_timeout, 0);

    return 0;
}

static void __exit vwifi_exit(void)
{
    /* make sure that no work is queued */
    cancel_work_sync(&g_ctx->ws_connect);
    cancel_work_sync(&g_ctx->ws_disconnect);
    cancel_work_sync(&g_ctx->ws_scan);
    owl_free(g_ctx);
}

module_init(vwifi_init);
module_exit(vwifi_exit);
