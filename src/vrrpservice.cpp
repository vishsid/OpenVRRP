/*
 * Copyright (C) 2012 Peter Christensen <pch@ordbogen.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "arpsocket.h"
#include "netlink.h"
#include "vrrpservice.h"
#include "vrrpsocket.h"
#include "arpservice.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>

#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <net/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>

VrrpService::VrrpService (int interface, int family, std::uint_fast8_t virtualRouterId, std::uint_fast16_t vlanId) :
	m_virtualRouterId(virtualRouterId),
	m_priority(100),
	m_primaryIpAddress(Netlink::getPrimaryIpAddress(interface, family)),
	m_autoPrimaryIpAddress(true),
	m_advertisementInterval(100),
	m_masterAdvertisementInterval(m_advertisementInterval),
	m_preemptMode(true),
	m_acceptMode(family == AF_INET6 || vlanId != 0 ? true : false),
	m_masterDownTimer(timerCallback, this),
	m_advertisementTimer(timerCallback, this),
	m_state(Disabled),
	m_family(family),
	m_interface(interface),
	m_macvlanInterface(-1),
	m_vlanInterface(-1),
	m_outputInterface(interface),
	m_inputInterface(interface),
	m_socket(VrrpSocket::instance(m_family)),
	m_vlanId(vlanId),
	m_error(0),

	m_statsMasterTransitions(0),
	m_statsNewMasterReason(NotMaster),
	m_statsRcvdAdvertisements(0),
	m_statsAdvIntervalErrors(0),
	m_statsIpTtlErrors(0),
	m_statsProtocolErrReason(NoError),
	m_statsRcvdPriZeroPackets(0),
	m_statsSentPriZeroPackets(0),
	m_statsRcvdInvalidTypePackets(0),
	m_statsAddressListErrors(0),
	m_statsPacketLengthErrors(0),

	m_pendingNewMasterReason(MasterNotResponding)
{
	if (m_family == AF_INET)
		m_name = "VRRP IPv4 Service";
	else // if (m_family == AF_INET6)
		m_name = "VRRP IPv6 Service";

	m_mac[0] = 0x00;
	m_mac[1] = 0x00;
	m_mac[2] = 0x5E;
	m_mac[3] = 0x00;
	m_mac[4] = (family == AF_INET ? 1 : 2);
	m_mac[5] = virtualRouterId;

	if (m_socket == 0)
	{
		m_error = 1;
		return;
	}


	// Create MACVLAN interface
	char name[sizeof("vrrpx.xxx.xxxx")];
	std::sprintf(name, "vrrp%s.%hhu", (family == AF_INET6 ? "6" : ""), virtualRouterId);
	m_macvlanInterface = Netlink::addMacvlanInterface(m_outputInterface, m_mac, name);
	if (m_macvlanInterface < 0)
	{
		// We could not create a MACVLAN interface, so the MAC we're using is not the VRRP MAC
		int s = socket(AF_INET, SOCK_DGRAM, 0);

		ifreq req;
		if_indextoname(interface, req.ifr_name);
		if (ioctl(s, SIOCGIFHWADDR, &req) == -1)
		{
			syslog(LOG_WARNING, "Failed to get MAC address of VRRP interface: %s", std::strerror(errno));
		}
		else
		{
			std::memcpy(m_mac, req.ifr_hwaddr.sa_data, sizeof(m_mac));
		}
		close(s);
	}
	else
		m_outputInterface = m_macvlanInterface;

	// Create VLAN interface if necessary
	if (m_vlanId > 0)
	{
		std::sprintf(name, "vrrp%s.%hhu.%hu", (family == AF_INET6 ? "6" : ""), virtualRouterId, static_cast<unsigned short int>(m_vlanId));
		m_vlanInterface = Netlink::addVlanInterface(m_outputInterface, m_vlanId, name);
		if (m_vlanInterface < 0)
		{
			syslog(LOG_WARNING, "Failed to create VLAN interface: %s", std::strerror(errno));
			m_error = 1;
			return;
		}

		m_outputInterface = m_vlanInterface;
		m_inputInterface = m_vlanInterface;
	}
	
	m_socket->addInterface(m_inputInterface);
	m_socket->addEventListener(m_inputInterface, m_virtualRouterId, this);

	Netlink::addInterfaceMonitor(m_interface, interfaceCallback, this);
}

VrrpService::~VrrpService ()
{
	shutdown(Disabled);

	Netlink::removeInterfaceMonitor(m_interface, interfaceCallback, this);

	if (m_socket != 0)
	{
		m_socket->removeInterface(m_inputInterface);
		m_socket->removeEventListener(m_inputInterface, m_virtualRouterId);
	}

	if (m_vlanInterface != -1)
		Netlink::removeInterface(m_vlanInterface);
	if (m_macvlanInterface != -1)
		Netlink::removeInterface(m_macvlanInterface);


}

int VrrpService::error () const
{
	return m_error;
}

int VrrpService::interface () const
{
	return m_interface;
}

int VrrpService::family () const
{
	return m_family;
}

std::uint_fast8_t VrrpService::virtualRouterId () const
{
	return m_virtualRouterId;
}

const std::uint8_t *VrrpService::mac () const
{
	return m_mac;
}

IpAddress VrrpService::primaryIpAddress () const
{
	return m_primaryIpAddress;
}

bool VrrpService::hasAutoPrimaryIpAddress () const
{
	return m_autoPrimaryIpAddress;
}

IpAddress VrrpService::masterIpAddress () const
{
	return m_masterIpAddress;
}

bool VrrpService::setPrimaryIpAddress (const IpAddress &address)
{
	if (address.family() == m_family)
	{
		m_primaryIpAddress = address;
		m_autoPrimaryIpAddress = false;
		return true;
	}
	else
		return false;
}

void VrrpService::unsetPrimaryIpAddress ()
{
	if (!m_autoPrimaryIpAddress)
	{
		m_primaryIpAddress = Netlink::getPrimaryIpAddress(m_interface, m_family);
		m_autoPrimaryIpAddress = false;
	}
}

bool VrrpService::setPriority (std::uint_fast8_t priority)
{
	if (priority == 0)
		return false;

	if (m_state == Master && !m_acceptMode)
	{
		if (m_priority != 255 && priority == 255)
		{
			// We are going from non-owner in non-accept mode to the address owner, so we must move
			// IP addresses from ARP service to interfaces
			for (IpSubnetSet::const_iterator subnet = m_subnets.begin(); subnet != m_subnets.end(); ++subnet)
			{
				Netlink::addIpAddress(m_outputInterface, *subnet);
				ArpService::removeFakeArp(m_interface, subnet->address());
			}

		}
		else if (m_priority == 255 && priority != 255)
		{
			// We are going from a being address owner to a non-owner in non-accept mode, so we must move
			// IP addresses from interfaces to ARP service
			for (IpSubnetSet::const_iterator subnet = m_subnets.begin(); subnet != m_subnets.end(); ++subnet)
			{
				ArpService::addFakeArp(m_interface, subnet->address(), m_mac);
				Netlink::removeIpAddress(m_outputInterface, *subnet);
			}
		}
	}

	m_priority = priority;

	return true;
}

std::uint_fast8_t VrrpService::priority () const
{
	return m_priority;
}

bool VrrpService::setAdvertisementInterval (unsigned int advertisementInterval)
{
	if (advertisementInterval < 1 || advertisementInterval > 4095)
		return false;
	m_advertisementInterval = advertisementInterval;
	return true;
}

unsigned int VrrpService::advertisementInterval () const
{
	return m_advertisementInterval;
}

unsigned int VrrpService::masterAdvertisementInterval () const
{
	return m_masterAdvertisementInterval;
}

unsigned int VrrpService::skewTime () const
{
	return ((256 - m_priority) * m_masterAdvertisementInterval) / 256;
}

unsigned int VrrpService::masterDownInterval () const
{
	return 3 * m_masterAdvertisementInterval + skewTime();
}

void VrrpService::setPreemptMode (bool enabled)
{
	m_preemptMode = enabled;
}

bool VrrpService::preemptMode () const
{
	return m_preemptMode;
}

void VrrpService::setAcceptMode (bool enabled)
{
	if (m_acceptMode == enabled)
		return;

	if (m_family == AF_INET6 || m_vlanId != 0)
		return;

	if (m_state == Master)
	{
		// We are master, so we need to move the IP addresses between interfaces and the ARP service
		if (enabled)
		{
			for (IpSubnetSet::const_iterator subnet = m_subnets.begin(); subnet != m_subnets.end(); ++subnet)
			{
				Netlink::addIpAddress(m_outputInterface, *subnet);
				ArpService::removeFakeArp(m_interface, subnet->address());
			}
		}
		else
		{
			for (IpSubnetSet::const_iterator subnet = m_subnets.begin(); subnet != m_subnets.end(); ++subnet)
			{
				ArpService::addFakeArp(m_interface, subnet->address(), m_mac);
				Netlink::removeIpAddress(m_outputInterface, *subnet);
			}
		}
	}

	m_acceptMode = enabled;
}

bool VrrpService::acceptMode () const
{
	return m_acceptMode;
}

void VrrpService::timerCallback (Timer *timer, void *userData)
{
	VrrpService *self = reinterpret_cast<VrrpService *>(userData);
	if (timer == &self->m_masterDownTimer)
		self->onMasterDownTimer();
	else if (timer == &self->m_advertisementTimer)
		self->onAdvertisementTimer();
}

void VrrpService::enable ()
{
	if (m_state == Disabled)
	{
		if (Netlink::isInterfaceUp(m_interface))
			startup();
		else
			setState(LinkDown);
	}
}

void VrrpService::disable ()
{
	if (m_state != Disabled)
	{
		shutdown(Disabled);
	}
}

bool VrrpService::enabled () const
{
	return m_state != Disabled;
}

void VrrpService::setMasterCommand (const std::string &command)
{
	m_masterCommand = command;
}

std::string VrrpService::masterCommand () const
{
	return m_masterCommand;
}

void VrrpService::setBackupCommand (const std::string &command)
{
	m_backupCommand = command;
}

std::string VrrpService::backupCommand () const
{
	return m_backupCommand;
}

std::uint_fast16_t VrrpService::vlanId() const
{
	return m_vlanId;
}

std::uint_fast32_t VrrpService::statsMasterTransitions () const
{
	return m_statsMasterTransitions;
}

VrrpService::NewMasterReason VrrpService::statsNewMasterReason () const
{
	return m_statsNewMasterReason;
}

std::uint_fast64_t VrrpService::statsRcvdAdvertisements () const
{
	return m_statsRcvdAdvertisements;
}

std::uint_fast64_t VrrpService::statsAdvIntervalErrors () const
{
	return m_statsAdvIntervalErrors;
}

std::uint_fast64_t VrrpService::statsIpTtlErrors () const
{
	return m_statsIpTtlErrors;
}

VrrpService::ProtocolErrorReason VrrpService::statsProtocolErrReason () const
{
	return m_statsProtocolErrReason;
}

std::uint_fast64_t VrrpService::statsRcvdPriZeroPackets () const
{
	return m_statsRcvdPriZeroPackets;
}

std::uint_fast64_t VrrpService::statsSentPriZeroPackets() const
{
	return m_statsSentPriZeroPackets;
}

std::uint_fast64_t VrrpService::statsRcvdInvalidTypePackets () const
{
	return m_statsRcvdInvalidTypePackets;
}

std::uint_fast64_t VrrpService::statsAddressListErrors () const
{
	return m_statsAddressListErrors;
}

std::uint_fast64_t VrrpService::statsPacketLengthErrors () const
{
	return m_statsPacketLengthErrors;
}

void VrrpService::startup ()
{
	if (m_priority == 255)
	{
		// We are starting up and we are the owner of the virtual IP addresses, so transition to master immediately
		setState(Master);
		if (m_family == AF_INET)
			sendARPs();
		else // if (m_family == AF_INET6)
		{
			// Neighbor advertisements are sent automatically
		}

		m_advertisementTimer.start(m_advertisementInterval * 10);
		
		// Update statistics
		++m_statsMasterTransitions;
		m_statsNewMasterReason = Preempted;
		m_masterIpAddress = m_primaryIpAddress;
	}
	else
	{
		// We are starting up, but we are not the owner. Transition to backup and wait for an advertisement from a master
		m_masterAdvertisementInterval = m_advertisementInterval;
		m_masterDownTimer.start(masterDownInterval() * 10);
		setState(Backup);
		m_statsNewMasterReason = NotMaster;
	}
}

void VrrpService::shutdown (State newState)
{
	if (state() == Backup)
	{
		// We are backup, so just stop our timer and switch to initialize state
		m_masterDownTimer.stop();
		setState(newState);
	}
	else if (state() == Master)
	{
		// We are master, so inform everybody that we're leaving
		m_advertisementTimer.stop();
		sendAdvertisement(0);
		setState(newState);
		
		++m_statsSentPriZeroPackets;
	}
}

void VrrpService::onMasterDownTimer ()
{
	if (m_state == Backup)
	{
		// We are backup and the master down timer triggered, so we should transition to master
		setState(Master);
		if (m_family == AF_INET)
			sendARPs();
		else if (m_family == AF_INET6)
		{
			// Solicited multicast is automatically joined by Linux
			// Neighbor advertisements is sent automatically by Linux
		}
		m_advertisementTimer.start(m_advertisementInterval * 10);

		// Update statistics
		++m_statsMasterTransitions;
		m_statsNewMasterReason = m_pendingNewMasterReason;
		m_masterIpAddress = m_primaryIpAddress;
	}
}

void VrrpService::onAdvertisementTimer ()
{
	if (m_state == Master)
	{
		// We are master and the advertisement timer fired, so send an advertisement
		sendAdvertisement(m_priority);
		m_advertisementTimer.start(m_advertisementInterval * 10);
	}
}

void VrrpService::onIncomingVrrpPacket (
		unsigned int,
		const IpAddress &address,
		std::uint_fast8_t,
		std::uint_fast8_t priority,
		std::uint_fast16_t maxAdvertisementInterval,
		const IpAddressList &addresses)
{
	++m_statsRcvdAdvertisements;
	m_statsProtocolErrReason = NoError;

	if (!maxAdvertisementInterval != m_advertisementInterval)
		++m_statsAdvIntervalErrors;

	if (m_state == Backup)
	{
		if (priority == 0)
		{
			// The master decided to stop gracefully, wait skew time before transitioning to master
			m_masterDownTimer.start(skewTime() * 10);

			++m_statsRcvdPriZeroPackets;
			m_pendingNewMasterReason = Priority;
		}
		else if (!m_preemptMode || priority >= this->priority())
		{
			// The right master is running, wait for the next announcement
			m_masterAdvertisementInterval = maxAdvertisementInterval;
			m_masterDownTimer.start(masterDownInterval() * 10);
			m_masterIpAddress = address;

			// Check address list
			std::vector<IpAddress> incomingList(addresses.size());
			std::copy(addresses.begin(), addresses.end(), incomingList.begin());
			std::sort(incomingList.begin(), incomingList.end());
			
			std::vector<IpAddress> serviceList(m_addresses.size());
			std::copy(m_addresses.begin(), m_addresses.end(), serviceList.begin());
			std::sort(serviceList.begin(), serviceList.end());

			std::vector<IpAddress> difference(std::max(incomingList.size(), serviceList.size()));
			std::vector<IpAddress>::iterator differenceEnd = std::set_difference(incomingList.begin(), incomingList.end(), serviceList.begin(), serviceList.end(), difference.begin());

			std::vector<IpAddress>::size_type differenceCount = differenceEnd - difference.begin();
			if (differenceCount > 0)
			{
				// There are differences between incoming address list and our list
				syslog(LOG_WARNING, "%s (Router %u, Interface %u): Address list mismatch", m_name, (unsigned int)m_virtualRouterId, (unsigned int)m_interface);
				++m_statsAddressListErrors;
			}

			m_pendingNewMasterReason = MasterNotResponding;
		}
		else if (m_preemptMode)
			m_pendingNewMasterReason = Preempted;
		else
			m_pendingNewMasterReason = Priority;
	}
	else if (m_state == Master)
	{
		if (priority == 0)
		{
			// The conflicing master is stopping gracefully, so just remind everybody that we are the master
			sendAdvertisement(m_priority);
			m_advertisementTimer.start(advertisementInterval() * 10);

			++m_statsRcvdPriZeroPackets;
			m_pendingNewMasterReason = Priority;
		}
		else if (priority > m_priority || (priority == m_priority && address > m_primaryIpAddress))
		{
			// The conflicing master has higher priority than us, so we transition to backup
			m_advertisementTimer.stop();
			m_masterAdvertisementInterval = maxAdvertisementInterval;
			m_masterDownTimer.start(masterDownInterval() * 10);
			setState(Backup);
			setDefaultMac();

			m_pendingNewMasterReason = Priority;
		}
		else
		{
			// The conflicing master has a lower priority than us. We expect it to transition to backup
		}
	}
}

bool VrrpService::sendAdvertisement (std::uint_least8_t priority)
{
	return m_socket->sendPacket(
			m_outputInterface,
			m_primaryIpAddress,
			m_virtualRouterId,
			priority,
			m_advertisementInterval,
			m_addresses);
}

void VrrpService::setState (State state)
{
	if (m_state != state)
	{
		static const char *states[] = {"Disabled", "LinkDown", "Backup", "Master"};
		State oldState = m_state;
		m_state = state;
		if (m_state == Master)
		{
			static const char *reasons[] = {"NotMaster", "Priority", "Preempted", "MasterNotResponding"};
			syslog(LOG_INFO, "%s (Router %u, Interface %u): Changed state to %s (Reason: %s)", m_name, (unsigned int)m_virtualRouterId, (unsigned int)m_interface, states[m_state], reasons[m_pendingNewMasterReason - 1]);
		}
		else
			syslog(LOG_INFO, "%s (Router %u, Interface %u): Changed state to %s", m_name, (unsigned int)m_virtualRouterId, (unsigned int)m_interface, states[m_state]);
		if (m_state == Master)
		{
			setVirtualMac();
			addIpAddresses();
			sendAdvertisement(m_priority);
		}
		else
		{
			if (oldState == Master)
				removeIpAddresses();
			setDefaultMac();
		}

		if (state == Backup)
		{
			if (m_backupCommand.size() != 0)
				executeScript(m_backupCommand);
		}
		else if (state == Master)
		{
			if (m_masterCommand.size() != 0)
				executeScript(m_masterCommand);
		}
	}
}

void VrrpService::sendARPs ()
{
	for (IpAddressSet::const_iterator address = m_addresses.begin(); address != m_addresses.end(); ++address)
		ArpSocket::sendGratuitiousArp(m_outputInterface, *address);
}

bool VrrpService::setVirtualMac ()
{
	bool ret = true;
	if (m_macvlanInterface != -1)
		ret &= Netlink::toggleInterface(m_macvlanInterface, true);
	if (m_vlanInterface != -1)
		ret &= Netlink::toggleInterface(m_vlanInterface, true);
	return ret;
}

bool VrrpService::setDefaultMac ()
{
	bool ret = true;
	if (m_vlanInterface != -1)
		ret &= Netlink::toggleInterface(m_vlanInterface, false);
	if (m_macvlanInterface != -1)
		ret &= Netlink::toggleInterface(m_macvlanInterface, false);
	return ret;
}

bool VrrpService::addIpAddresses ()
{
	bool ret = true;
	if (m_acceptMode || m_priority == 255)
	{
		for (IpSubnetSet::const_iterator subnet = m_subnets.begin(); subnet != m_subnets.end(); ++subnet)
			ret &= (Netlink::addIpAddress(m_outputInterface, *subnet));
	}
	else
	{
		for (IpSubnetSet::const_iterator subnet = m_subnets.begin(); subnet != m_subnets.end(); ++subnet)
			ret &= ArpService::addFakeArp(m_interface, subnet->address(), m_mac);
	}
	return ret;
}

bool VrrpService::removeIpAddresses ()
{
	bool ret = true;
	if (m_acceptMode || m_priority == 255)
	{
		for (IpSubnetSet::const_iterator subnet = m_subnets.begin(); subnet != m_subnets.end(); ++subnet)
			ret &= (Netlink::removeIpAddress(m_outputInterface, *subnet));
	}
	else
	{
		for (IpSubnetSet::const_iterator subnet = m_subnets.begin(); subnet != m_subnets.end(); ++subnet)
			ret &= ArpService::removeFakeArp(m_interface, subnet->address());
	}
	return ret;
}

bool VrrpService::addIpAddress (const IpSubnet &subnet)
{
	if (subnet.address().family() != m_family)
		return false;
	m_subnets.insert(subnet);
	m_addresses.insert(subnet.address());

	if (m_state == Master)
	{
		if (m_acceptMode || m_priority == 255)
			Netlink::addIpAddress(m_outputInterface, subnet);
		else
			ArpService::addFakeArp(m_outputInterface, subnet.address(), m_mac);
	}

	return true;
}

bool VrrpService::removeIpAddress (const IpSubnet &subnet)
{
	bool ret = false;
	ret |= m_subnets.erase(subnet);
	ret |= m_addresses.erase(subnet.address());

	if (m_state == Master)
	{
		if (m_acceptMode || m_priority == 255)
			Netlink::removeIpAddress(m_outputInterface, subnet);
		else
			ArpService::removeFakeArp(m_outputInterface, subnet.address());
	}

	return ret;
}

const IpSubnetSet &VrrpService::subnets () const
{
	return m_subnets;
}

const IpAddressSet &VrrpService::addresses () const
{
	return m_addresses;
}

VrrpService::State VrrpService::state () const
{
	return m_state;
}

void VrrpService::setProtocolErrorReason (ProtocolErrorReason reason)
{
	m_statsProtocolErrReason = reason;
	// TODO Send SNMP notificaiton
}

void VrrpService::onIncomingVrrpError (unsigned int, std::uint_fast8_t, VrrpEventListener::Error error)
{
	switch (error)
	{
		case VrrpEventListener::ChecksumError:
			setProtocolErrorReason(ChecksumError);
			break;

		case VrrpEventListener::VersionError:
			setProtocolErrorReason(VersionError);
			break;

		case VrrpEventListener::VrIdError:
			setProtocolErrorReason(VrIdError);
			break;
			
		case VrrpEventListener::AdvIntervalError:
			++m_statsAdvIntervalErrors;
			break;

		case VrrpEventListener::IpTtlError:
			setProtocolErrorReason(IpTtlError);
			break;

		case VrrpEventListener::InvalidTypeError:
			++m_statsRcvdInvalidTypePackets;
			break;

		case VrrpEventListener::PacketLengthError:
			++m_statsPacketLengthErrors;
			break;
	}
}

void VrrpService::interfaceCallback (int, bool isUp, void *userData)
{
	VrrpService *service = reinterpret_cast<VrrpService *>(userData);
	if (isUp)
	{
		if (service->state() == LinkDown)
			service->startup();
	}
	else
	{
		if (service->state() != Disabled)
			service->shutdown(LinkDown);
	}
}

void VrrpService::executeScript (const std::string &command)
{
	static bool initialized = false;
	if (!initialized)
		signal(SIGCHLD, SIG_IGN);

	pid_t pid = fork();
	if (pid == 0)
	{
		// Child process

		// Setup environment variables
		// VRRP_IF = Physical network interface
		// VRRP_VIF = MACVLAN interface
		// VRRP_VRID = Virtual router id
		// VRRP_PRIO = Priority
		// VRRP_PRIMARY_IP = Primary IP of VRRP router
		// VRRP_MASTER_IP = IP of VRRP master
		// VRRP_ADVER_INT = Advertisement interval in units of 10ms
		// VRRP_MASTER_ADVER_INT = Advertisement interval of master
		// VRRP_PREEMPT = 1 if preempt mode
		// VRRP_ACCEPT = 1 if accept mode
		// VRRP_IPLIST = Comma separated list of IP addresses
		// VRRP_PROTO = IPv4 or IPv6 depending on setup
		// VRRP_STATE = master or backup depending on state

		char buffer[IFNAMSIZ];

		setenv("VRRP_IF", if_indextoname(m_interface, buffer), 1);
		setenv("VRRP_VIF", if_indextoname(m_outputInterface, buffer), 1);

		std::sprintf(buffer, "%hhu", m_virtualRouterId);
		setenv("VRRP_VRID", buffer, 1);

		std::sprintf(buffer, "%hhu", m_priority);
		setenv("VRRP_PRIO", buffer, 1);

		setenv("VRRP_PRIMARY_IP", m_primaryIpAddress.toString().c_str(), 1);
		setenv("VRRP_MASTER_IP", m_masterIpAddress.toString().c_str(), 1);

		std::sprintf(buffer, "%u", m_advertisementInterval);
		setenv("VRRP_ADVER_INT", buffer, 1);

		std::sprintf(buffer, "%u", m_masterAdvertisementInterval);
		setenv("VRRP_MASTER_ADVER_INT", buffer, 1);

		setenv("VRRP_PREEMPT", m_preemptMode ? "1" : "0", 1);
		setenv("VRRP_ACCEPT", m_acceptMode ? "1" : "0", 1);
		setenv("VRRP_STATE", m_state == Master ? "master" : "backup", 1);

		std::string ipList;
		for (IpSubnetSet::const_iterator subnet = m_subnets.begin(); subnet != m_subnets.end(); ++subnet)
		{
			if (ipList.size() > 0)
				ipList.append(",");
			ipList.append(subnet->address().toString());
		}

		setenv("VRRP_IP_LIST", ipList.c_str(), 1);

		setenv("VRRP_PROTO", m_family == AF_INET ? "IPv4" : "IPv6", 1);

		// Execute
		execl("/bin/sh", "sh", "-c", command.c_str(), 0);
		syslog(LOG_ERR, "Error executing command `%s': %s", command.c_str(), std::strerror(errno));
		_exit(EXIT_FAILURE);
	}
	else
	{
		syslog(LOG_INFO, "Executed command: %s", command.c_str());
	}
}
