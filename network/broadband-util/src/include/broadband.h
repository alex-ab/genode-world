/*
 * \brief  Code shared by mbimcli/qmicli
 * \author Sebastian Sumpf
 * \date   2026-03-16
 */

/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _BROADBAND_H_
#define _BROADBAND_H_

#include <net/ipv4.h>

namespace Broadband {

	struct Connection;
	struct Network;
	struct State;
	class  Config_reporter;
	class  State_reporter;

	using namespace Genode;
	using namespace Net;
	using String = Genode::String<32>;
}


struct Broadband::Connection
{
	Ipv4_address ip;
	uint32_t     mask;
	Ipv4_address gateway;
	Ipv4_address dns[2];
	bool         connected;
};


struct Broadband::Network
{
	String apn;
	String user;
	String password;
	String pin;
};


struct Broadband::State
{
	enum Signal { RSSI_DISCONNECT = -175l };

	String sim        { };
	String error      { };
	String network    { };
	String provider   { };
	String data_class { };
	String roaming    { };
	long     rssi       { 99 };
	float    rsrq       { 0  };
	long     rsrp       { 0  };
	float    rssnr      { 0  };
};


/*
 * Reporter that generates a nic_router configuration
 */
class Broadband::Config_reporter
{
	private:

		Reporter               &_reporter;
		Attached_rom_dataspace &_config;

	public:

		Config_reporter(Reporter &reporter,
		                Attached_rom_dataspace &config)
		: _reporter(reporter), _config(config) { }

		void report(Connection &connection)
		{
			/* handle intermediate disconnect */
			if (!connection.connected) {
				_reporter.enabled(true);
				(void)_reporter.generate([&] (Generator &g) {
					g.attribute("verbose", "no");
					g.attribute("verbose_packets", "no");
					g.attribute("verbose_domain_state", "yes");
					g.attribute("verbose_packet_drop", "no");
				});
				return;
			}

			String interface = "10.0.1.1/24";
			String ip_first  = "10.0.1.2";
			String ip_last   = "10.0.1.200";

			_config.node().with_optional_sub_node("default-domain",
				[&] (Node const &net) {
					interface = net.attribute_value("interface", interface);
					ip_first  = net.attribute_value("ip_first",  ip_first);
					ip_last   = net.attribute_value("ip_first",  ip_last);
				});

			_reporter.enabled(true);
			_reporter.generate([&] (Generator &g) {
				g.attribute("verbose", "no");
				g.attribute("verbose_packets", "no");
				g.attribute("verbose_domain_state", "yes");
				g.attribute("verbose_packet_drop", "no");

					g.node("default-policy", [&] () {
						g.attribute("domain", "default");
					});

					g.node("policy", [&] () {
						g.attribute("label_prefix", "usb_net");
						g.attribute("domain", "uplink");
					});

					/* uplink */
					g.node("domain", [&] () {
						g.attribute("name", "uplink");
						Genode::String<18> ip { connection.ip, "/", connection.mask };
						g.attribute("interface", ip);
						Genode::String<15> gw { connection.gateway };
						g.attribute("gateway", gw);
						/* no ARP */
						g.attribute("use_arp", "no");

						g.node("nat", [&] () {
							g.attribute("domain", "default");
							g.attribute("tcp-ports", "1000");
							g.attribute("udp-ports", "1000");
							g.attribute("icmp-ids", "1000");
						});
						if (_config.node().attribute_value("nic_client_enable", false)) {
							g.node("nat", [&] () {
								g.attribute("domain", "downlink");
								g.attribute("tcp-ports", "1000");
								g.attribute("udp-ports", "1000");
								g.attribute("icmp-ids", "1000");
							});
						}
					});

					/* link to another nic_router */
					if (_config.node().attribute_value("nic_client_enable", false)) {
						g.node("nic-client", [&] () {
							g.attribute("domain", "downlink");
						});
						g.node("domain", [&] () {
							g.attribute("name", "downlink");

							g.attribute("interface", "10.0.2.1/24");

							g.node("dhcp-server", [&] () {
								g.attribute("ip_first", "10.0.2.2");
								g.attribute("ip_last",  "10.0.2.3");

								g.node("dns-server", [&] () {
									g.attribute("ip", Genode::String<15>(connection.dns[0]));
								});

								g.node("dns-server", [&] () {
									g.attribute("ip", Genode::String<15>(connection.dns[1]));
								});
							});

							g.node("tcp", [&] () {
								g.attribute("dst", "0.0.0.0/0");
								g.node("permit-any", [&] () {
									g.attribute("domain", "uplink");
								});
							});
							g.node("udp", [&] () {
								g.attribute("dst", "0.0.0.0/0");
								g.node("permit-any", [&] () {
									g.attribute("domain", "uplink");
								});
							});
							g.node("icmp", [&] () {
								g.attribute("dst", "0.0.0.0/0");
								g.attribute("domain", "uplink");
							});
						});
					}

					/* default */
					g.node("domain", [&] () {
						g.attribute("name", "default");

						g.attribute("interface", interface);

						g.node("dhcp-server", [&] () {
							g.attribute("ip_first", ip_first);
							g.attribute("ip_last",  ip_last);

							g.node("dns-server", [&] () {
								g.attribute("ip", Genode::String<15>(connection.dns[0]));
							});

							g.node("dns-server", [&] () {
								g.attribute("ip", Genode::String<15>(connection.dns[1]));
							});
						});

						g.node("tcp", [&] () {
							g.attribute("dst", "0.0.0.0/0");
							g.node("permit-any", [&] () {
								g.attribute("domain", "uplink");
							});
						});
						g.node("udp", [&] () {
							g.attribute("dst", "0.0.0.0/0");
							g.node("permit-any", [&] () {
								g.attribute("domain", "uplink");
							});
						});
						g.node("icmp", [&] () {
							g.attribute("dst", "0.0.0.0/0");
							g.attribute("domain", "uplink");
						});
					});
				}).with_error([] (Buffer_error) {
					warning("Could not report NIC router configuration");
				});
		}
};


class Broadband::State_reporter
{
	private:

		Reporter &_reporter;

	public:

		State_reporter(Reporter &reporter) : _reporter(reporter) { }

		void report(State &report)
		{
			_reporter.enabled(true);
			try {
				(void)_reporter.generate([&] (Genode::Generator &g) {
					g.node("device", [&] () {
						g.attribute("sim", report.sim);
					});

					g.node("network", [&] () {
						g.attribute("error",      report.error);
						g.attribute("registered", report.network);
						g.attribute("provider",   report.provider);
						g.attribute("data_class", report.data_class);
						g.attribute("roaming",    report.roaming);
					});

					g.node("signal", [&] () {
						if (report.rssi < State::RSSI_DISCONNECT)
							g.attribute("rssi_dbm", "unknown");
						else
							g.attribute("rssi_dbm", report.rssi);

						g.attribute("rsrq_db",    report.rsrq);
						g.attribute("rsrp_dbm",   report.rsrp);

						gchar rssnr[8];
						g_snprintf(rssnr, sizeof(rssnr), "%.1lf", report.rssnr);
						g.attribute("rssnr_db",   rssnr);
						
					});
				});
			}
			catch (...) { warning("Could not report state."); }
		}
};

#endif /* _BROADBAND_H_ */
