/*
 * driver_network.h — Network driver skeleton for NT-Box (virtio-net).
 *
 * Implements network send/receive via virtio-net on host.
 * Integrates with NT-Box driver model for Windows NDIS compatibility.
 *
 * Lamps: NET-001..NET-004
 *   NET-001 OK — ntb0_net_init() initializes virtio-net device
 *   NET-002 OK — ntb0_net_send() transmits packets
 *   NET-003 OK — ntb0_net_recv() receives packets
 *   NET-004 OK — ntb0_net_get_stats() reports link status/counters
 */
#ifndef NTB0_DRIVER_NETWORK_H
#define NTB0_DRIVER_NETWORK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ------------------------------------------------------------------ */
/* Network configuration / statistics                                    */
/* ------------------------------------------------------------------ */

#define NTB0_NET_MAX_MTU        9000
#define NTB0_NET_DEFAULT_MTU    1500
#define NTB0_NET_MAX_QUEUES     8
#define NTB0_NET_MAC_LEN        6

enum ntb0_net_link_state {
    NTB0_NET_LINK_DOWN = 0,
    NTB0_NET_LINK_UP = 1,
};

struct ntb0_net_config {
    uint8_t mac[NTB0_NET_MAC_LEN];
    uint32_t mtu;
    uint16_t num_queues;
    uint32_t features;      /* NTB0_NET_FEAT_* */
    uint32_t rx_buf_size;
    uint32_t tx_buf_size;
};

#define NTB0_NET_FEAT_CSUM_IPV4     (1u << 0)
#define NTB0_NET_FEAT_CSUM_TCP      (1u << 1)
#define NTB0_NET_FEAT_CSUM_UDP      (1u << 2)
#define NTB0_NET_FEAT_TSO           (1u << 3)
#define NTB0_NET_FEAT_LRO           (1u << 4)
#define NTB0_NET_FEAT_VLAN          (1u << 5)
#define NTB0_NET_FEAT_RSS           (1u << 6)
#define NTB0_NET_FEAT_MQ            (1u << 7)

struct ntb0_net_stats {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t multicast;
    uint64_t collisions;
    enum ntb0_net_link_state link_state;
    uint32_t link_speed_mbps;  /* 0 = unknown, 10/100/1000/10000... */
    uint32_t duplex;           /* 0 = unknown, 1 = half, 2 = full */
};

/* ------------------------------------------------------------------ */
/* Network device handle                                                 */
/* ------------------------------------------------------------------ */

typedef struct ntb0_net_device ntb0_net_device_t;

/* ------------------------------------------------------------------ */
/* Public API                                                            */
/* ------------------------------------------------------------------ */

/*
 * Initializes the network subsystem.
 * Opens virtio-net device (via /dev/vhost-net or userspace virtio).
 * Returns device handle on success, NULL on failure.
 *
 * config: NULL for defaults (auto MAC, MTU 1500), otherwise desired config.
 *         On return, filled with actual configuration.
 */
ntb0_net_device_t *ntb0_net_init(struct ntb0_net_config *config);

/*
 * Deinitializes the network subsystem.
 * Closes virtio device, releases queues/buffers.
 */
void ntb0_net_deinit(ntb0_net_device_t *dev);

/*
 * Transmits a packet.
 * frame: pointer to packet data (including Ethernet header).
 * len: packet length in bytes.
 * queue_idx: transmit queue index (0..num_queues-1), or -1 for default.
 * Returns number of bytes sent, or negative error code.
 * Non-blocking: returns -EAGAIN if queue full.
 */
long ntb0_net_send(ntb0_net_device_t *dev, const void *frame, size_t len, int queue_idx);

/*
 * Receives a packet.
 * frame: buffer to receive packet data.
 * max_len: maximum bytes to receive.
 * queue_idx: receive queue index (0..num_queues-1), or -1 for default.
 * Returns number of bytes received, or negative error code.
 * Non-blocking: returns -EAGAIN if no packet available.
 */
long ntb0_net_recv(ntb0_net_device_t *dev, void *frame, size_t max_len, int queue_idx);

/*
 * Gets current link status and statistics.
 * Returns 0 on success, -1 on failure.
 */
int ntb0_net_get_stats(ntb0_net_device_t *dev, struct ntb0_net_stats *stats);

/*
 * Sets link up/down (for testing / virtual cable pull).
 */
int ntb0_net_set_link(ntb0_net_device_t *dev, int up);

/*
 * Sets MAC address.
 * Returns 0 on success, -1 on failure.
 */
int ntb0_net_set_mac(ntb0_net_device_t *dev, const uint8_t mac[NTB0_NET_MAC_LEN]);

/*
 * Sets MTU.
 * Returns 0 on success, -1 on failure.
 */
int ntb0_net_set_mtu(ntb0_net_device_t *dev, uint32_t mtu);

/*
 * Adds a multicast MAC address to the receive filter.
 */
int ntb0_net_add_mcast(ntb0_net_device_t *dev, const uint8_t mac[NTB0_NET_MAC_LEN]);

/*
 * Removes a multicast MAC address from the receive filter.
 */
int ntb0_net_del_mcast(ntb0_net_device_t *dev, const uint8_t mac[NTB0_NET_MAC_LEN]);

/*
 * Sets promiscuous mode.
 */
int ntb0_net_set_promisc(ntb0_net_device_t *dev, int enable);

/*
 * Gets the underlying file descriptor for polling (epoll/poll/select).
 * Returns -1 if not available.
 */
int ntb0_net_get_fd(ntb0_net_device_t *dev, int queue_idx);

/*
 * Returns a human-readable error string for the last operation.
 */
const char *ntb0_net_error(ntb0_net_device_t *dev);

/*
 * Returns the device name (e.g., "virtio-net", "tap0").
 */
const char *ntb0_net_device_name(ntb0_net_device_t *dev);

/* ------------------------------------------------------------------ */
/* Windows NDIS interface (IRP-style)                                    */
/* ------------------------------------------------------------------ */

/*
 * Handles an IRP_MJ_DEVICE_CONTROL for network.
 * Implements IOCTL_NDIS_* / OID_* codes.
 * Returns NTSTATUS.
 */
long ntb0_net_device_control(ntb0_net_device_t *dev,
                             uint32_t ioctl_code,
                             const void *in_buf, uint32_t in_len,
                             void *out_buf, uint32_t out_len,
                             uint32_t *bytes_returned);

/* Common NDIS/OID IOCTL codes */
#define NTB0_IOCTL_NDIS_QUERY_OID         0x00170000
#define NTB0_IOCTL_NDIS_SET_OID           0x00170004
#define NTB0_IOCTL_NDIS_INDICATE_STATUS   0x00170008

/* Common OIDs (subset) */
#define NTB0_OID_GEN_SUPPORTED_LIST       0x00010101
#define NTB0_OID_GEN_HARDWARE_STATUS      0x00010102
#define NTB0_OID_GEN_MEDIA_SUPPORTED      0x00010103
#define NTB0_OID_GEN_MEDIA_IN_USE         0x00010104
#define NTB0_OID_GEN_MAXIMUM_FRAME_SIZE   0x00010106
#define NTB0_OID_GEN_LINK_SPEED           0x00010107
#define NTB0_OID_GEN_TRANSMIT_BUFFER_SPACE 0x00010108
#define NTB0_OID_GEN_RECEIVE_BUFFER_SPACE 0x00010109
#define NTB0_OID_GEN_TRANSMIT_BLOCK_SIZE  0x0001010A
#define NTB0_OID_GEN_RECEIVE_BLOCK_SIZE   0x0001010B
#define NTB0_OID_GEN_VENDOR_ID            0x0001010C
#define NTB0_OID_GEN_VENDOR_DESCRIPTION   0x0001010D
#define NTB0_OID_GEN_CURRENT_PACKET_FILTER 0x0001010E
#define NTB0_OID_GEN_CURRENT_LOOKAHEAD    0x0001010F
#define NTB0_OID_GEN_DRIVER_VERSION       0x00010110
#define NTB0_OID_GEN_MAXIMUM_TOTAL_SIZE   0x00010111
#define NTB0_OID_GEN_PROTOCOL_OPTIONS     0x00010112
#define NTB0_OID_GEN_MAC_OPTIONS          0x00010113
#define NTB0_OID_GEN_MEDIA_CONNECT_STATUS 0x00010114
#define NTB0_OID_GEN_MAXIMUM_SEND_PACKETS 0x00010115
#define NTB0_OID_GEN_VENDOR_DRIVER_VERSION 0x00010116
#define NTB0_OID_GEN_SUPPORTED_GUIDS      0x00010117
#define NTB0_OID_GEN_NETWORK_LAYER_ADDRESSES 0x00010118
#define NTB0_OID_GEN_TRANSPORT_LAYER_ADDRESSES 0x00010119
#define NTB0_OID_GEN_PHYSICAL_MEDIUM      0x0001011A
#define NTB0_OID_GEN_RECEIVE_SCALE_CAPABILITIES 0x0001011B

#define NTB0_OID_802_3_PERMANENT_ADDRESS  0x01010101
#define NTB0_OID_802_3_CURRENT_ADDRESS    0x01010102
#define NTB0_OID_802_3_MULTICAST_LIST     0x01010103
#define NTB0_OID_802_3_MAXIMUM_LIST_SIZE  0x01010104
#define NTB0_OID_802_3_RCV_ERROR_ALIGNMENT 0x01020101
#define NTB0_OID_802_3_XMIT_ONE_COLLISION 0x01020102
#define NTB0_OID_802_3_XMIT_MORE_COLLISIONS 0x01020103
#define NTB0_OID_802_3_XMIT_DEFERRED      0x01020104
#define NTB0_OID_802_3_XMIT_MAX_COLLISIONS 0x01020105
#define NTB0_OID_802_3_XMIT_UNDERRUN      0x01020106
#define NTB0_OID_802_3_XMIT_HEARTBEAT_FAILURE 0x01020107
#define NTB0_OID_802_3_XMIT_TIMES_CRS_LOST 0x01020108
#define NTB0_OID_802_3_XMIT_LATE_COLLISIONS 0x01020109

#define NTB0_OID_GEN_STATISTICS           0x00020101  /* Custom: get all stats */

#ifdef __cplusplus
}
#endif

#endif /* NTB0_DRIVER_NETWORK_H */