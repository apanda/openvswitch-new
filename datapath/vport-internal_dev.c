/*
 * Copyright (c) 2007-2012 Nicira Networks.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#include <linux/hardirq.h>
#include <linux/if_vlan.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/skbuff.h>
#include <linux/version.h>

#include "checksum.h"
#include "datapath.h"
#include "vlan.h"
#include "vport-generic.h"
#include "vport-internal_dev.h"
#include "vport-netdev.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,1,0)
#define HAVE_NET_DEVICE_OPS
#endif

struct internal_dev {
	struct vport *vport;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
	struct net_device_stats stats;
#endif
};

static struct internal_dev *internal_dev_priv(struct net_device *netdev)
{
	return netdev_priv(netdev);
}

/* This function is only called by the kernel network layer.*/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
static struct rtnl_link_stats64 *internal_dev_get_stats(struct net_device *netdev,
							struct rtnl_link_stats64 *stats)
{
#else
static struct net_device_stats *internal_dev_sys_stats(struct net_device *netdev)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
	struct net_device_stats *stats = &internal_dev_priv(netdev)->stats;
#else
	struct net_device_stats *stats = &netdev->stats;
#endif
#endif
	struct vport *vport = ovs_internal_dev_get_vport(netdev);
	struct ovs_vport_stats vport_stats;

	ovs_vport_get_stats(vport, &vport_stats);

	/* The tx and rx stats need to be swapped because the
	 * switch and host OS have opposite perspectives. */
	stats->rx_packets	= vport_stats.tx_packets;
	stats->tx_packets	= vport_stats.rx_packets;
	stats->rx_bytes		= vport_stats.tx_bytes;
	stats->tx_bytes		= vport_stats.rx_bytes;
	stats->rx_errors	= vport_stats.tx_errors;
	stats->tx_errors	= vport_stats.rx_errors;
	stats->rx_dropped	= vport_stats.tx_dropped;
	stats->tx_dropped	= vport_stats.rx_dropped;

	return stats;
}

static int internal_dev_mac_addr(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;
	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
	return 0;
}

/* Called with rcu_read_lock_bh. */
static int internal_dev_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	if (unlikely(compute_ip_summed(skb, true))) {
		kfree_skb(skb);
		return 0;
	}

	vlan_copy_skb_tci(skb);
	OVS_CB(skb)->flow = NULL;

	rcu_read_lock();
	ovs_vport_receive(internal_dev_priv(netdev)->vport, skb);
	rcu_read_unlock();
	return 0;
}

static int internal_dev_open(struct net_device *netdev)
{
	netif_start_queue(netdev);
	return 0;
}

static int internal_dev_stop(struct net_device *netdev)
{
	netif_stop_queue(netdev);
	return 0;
}

static void internal_dev_getinfo(struct net_device *netdev,
				 struct ethtool_drvinfo *info)
{
	strcpy(info->driver, "openvswitch");
}

static const struct ethtool_ops internal_dev_ethtool_ops = {
	.get_drvinfo	= internal_dev_getinfo,
	.get_link	= ethtool_op_get_link,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39)
	.get_sg		= ethtool_op_get_sg,
	.set_sg		= ethtool_op_set_sg,
	.get_tx_csum	= ethtool_op_get_tx_csum,
	.set_tx_csum	= ethtool_op_set_tx_hw_csum,
	.get_tso	= ethtool_op_get_tso,
	.set_tso	= ethtool_op_set_tso,
#endif
};

static int internal_dev_change_mtu(struct net_device *netdev, int new_mtu)
{
	if (new_mtu < 68)
		return -EINVAL;

	netdev->mtu = new_mtu;
	return 0;
}

static int internal_dev_do_ioctl(struct net_device *dev,
				 struct ifreq *ifr, int cmd)
{
	if (ovs_dp_ioctl_hook)
		return ovs_dp_ioctl_hook(dev, ifr, cmd);

	return -EOPNOTSUPP;
}

static void internal_dev_destructor(struct net_device *dev)
{
	struct vport *vport = ovs_internal_dev_get_vport(dev);

	ovs_vport_free(vport);
	free_netdev(dev);
}

#ifdef HAVE_NET_DEVICE_OPS
static const struct net_device_ops internal_dev_netdev_ops = {
	.ndo_open = internal_dev_open,
	.ndo_stop = internal_dev_stop,
	.ndo_start_xmit = internal_dev_xmit,
	.ndo_set_mac_address = internal_dev_mac_addr,
	.ndo_do_ioctl = internal_dev_do_ioctl,
	.ndo_change_mtu = internal_dev_change_mtu,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
	.ndo_get_stats64 = internal_dev_get_stats,
#else
	.ndo_get_stats = internal_dev_sys_stats,
#endif
};
#endif

static void do_setup(struct net_device *netdev)
{
	ether_setup(netdev);

#ifdef HAVE_NET_DEVICE_OPS
	netdev->netdev_ops = &internal_dev_netdev_ops;
#else
	netdev->do_ioctl = internal_dev_do_ioctl;
	netdev->get_stats = internal_dev_sys_stats;
	netdev->hard_start_xmit = internal_dev_xmit;
	netdev->open = internal_dev_open;
	netdev->stop = internal_dev_stop;
	netdev->set_mac_address = internal_dev_mac_addr;
	netdev->change_mtu = internal_dev_change_mtu;
#endif

	netdev->priv_flags &= ~IFF_TX_SKB_SHARING;
	netdev->destructor = internal_dev_destructor;
	SET_ETHTOOL_OPS(netdev, &internal_dev_ethtool_ops);
	netdev->tx_queue_len = 0;

	netdev->features = NETIF_F_LLTX | NETIF_F_SG | NETIF_F_FRAGLIST |
			   NETIF_F_HIGHDMA | NETIF_F_HW_CSUM | NETIF_F_TSO;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
	netdev->vlan_features = netdev->features;
	netdev->features |= NETIF_F_HW_VLAN_TX;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39)
	netdev->hw_features = netdev->features & ~NETIF_F_LLTX;
#endif
	random_ether_addr(netdev->dev_addr);
}

static struct vport *internal_dev_create(const struct vport_parms *parms)
{
	struct vport *vport;
	struct netdev_vport *netdev_vport;
	struct internal_dev *internal_dev;
	int err;

	vport = ovs_vport_alloc(sizeof(struct netdev_vport),
				&ovs_internal_vport_ops, parms);
	if (IS_ERR(vport)) {
		err = PTR_ERR(vport);
		goto error;
	}

	netdev_vport = netdev_vport_priv(vport);

	netdev_vport->dev = alloc_netdev(sizeof(struct internal_dev),
					 parms->name, do_setup);
	if (!netdev_vport->dev) {
		err = -ENOMEM;
		goto error_free_vport;
	}

	dev_net_set(netdev_vport->dev, ovs_dp_get_net(vport->dp));
	internal_dev = internal_dev_priv(netdev_vport->dev);
	internal_dev->vport = vport;

	/* Restrict bridge port to current netns. */
	if (vport->port_no == OVSP_LOCAL)
		netdev_vport->dev->features |= NETIF_F_NETNS_LOCAL;

	err = register_netdevice(netdev_vport->dev);
	if (err)
		goto error_free_netdev;

	dev_set_promiscuity(netdev_vport->dev, 1);
	netif_start_queue(netdev_vport->dev);

	return vport;

error_free_netdev:
	free_netdev(netdev_vport->dev);
error_free_vport:
	ovs_vport_free(vport);
error:
	return ERR_PTR(err);
}

static void internal_dev_destroy(struct vport *vport)
{
	struct netdev_vport *netdev_vport = netdev_vport_priv(vport);

	netif_stop_queue(netdev_vport->dev);
	dev_set_promiscuity(netdev_vport->dev, -1);

	/* unregister_netdevice() waits for an RCU grace period. */
	unregister_netdevice(netdev_vport->dev);
}

static int internal_dev_recv(struct vport *vport, struct sk_buff *skb)
{
	struct net_device *netdev = netdev_vport_priv(vport)->dev;
	int len;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
	if (unlikely(vlan_deaccel_tag(skb)))
		return 0;
#endif

	len = skb->len;
	skb->dev = netdev;
	skb->pkt_type = PACKET_HOST;
	skb->protocol = eth_type_trans(skb, netdev);
	forward_ip_summed(skb, false);

	netif_rx(skb);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
	netdev->last_rx = jiffies;
#endif

	return len;
}

const struct vport_ops ovs_internal_vport_ops = {
	.type		= OVS_VPORT_TYPE_INTERNAL,
	.flags		= VPORT_F_REQUIRED | VPORT_F_FLOW,
	.create		= internal_dev_create,
	.destroy	= internal_dev_destroy,
	.set_addr	= ovs_netdev_set_addr,
	.get_name	= ovs_netdev_get_name,
	.get_addr	= ovs_netdev_get_addr,
	.get_kobj	= ovs_netdev_get_kobj,
	.get_dev_flags	= ovs_netdev_get_dev_flags,
	.is_running	= ovs_netdev_is_running,
	.get_operstate	= ovs_netdev_get_operstate,
	.get_ifindex	= ovs_netdev_get_ifindex,
	.get_mtu	= ovs_netdev_get_mtu,
	.send		= internal_dev_recv,
};

int ovs_is_internal_dev(const struct net_device *netdev)
{
#ifdef HAVE_NET_DEVICE_OPS
	return netdev->netdev_ops == &internal_dev_netdev_ops;
#else
	return netdev->open == internal_dev_open;
#endif
}

struct vport *ovs_internal_dev_get_vport(struct net_device *netdev)
{
	if (!ovs_is_internal_dev(netdev))
		return NULL;

	return internal_dev_priv(netdev)->vport;
}
