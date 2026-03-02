/*
 * \brief  MBIM connection bindings
 * \author Sebastian Sumpf
 * \date   2020-10-27
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

/* Genode includes */
#include <base/attached_rom_dataspace.h>
#include <libc/component.h>
#include <os/reporter.h>
#include <net/ipv4.h>

/* libc includes */
#include <stdlib.h> /* 'exit'   */

#include <libmbim-glib.h>
extern "C" {
#include <mbimcli.h>
}

#include <broadband.h>


using namespace Genode;
using namespace Broadband;

class Mbim
{
	enum { TRACE = FALSE };

	enum State { NONE, UNLOCK, PIN, QUERY, ATTACH, CONNECT, READY };

	enum Backoff {
		BACKOFF_START = 1000,    /* first retry after one second */
		BACKOFF_LIMIT = 64000,   /* increase retry timeout up to 64 seconds */
		RSSI_DISCONNECT = 31,
	};

	using String       = Genode::String<32>;
	using Cstring      = Genode::Cstring;
	using Connection   = Broadband::Connection;
	using State_report = Broadband::State;

	private:

		Env      &_env;

		Reporter  _config_reporter { _env, "config", "nic_router.config" };
		Reporter  _state_reporter  { _env, "state",  "state" };

		Attached_rom_dataspace _config_rom   { _env, "config" };

		Config_reporter _broadband_config { _config_reporter, _config_rom };
		State_reporter  _broadband_state  { _state_reporter };

		State       _state      { NONE };
		GMainLoop  *_loop       { nullptr };
		MbimDevice *_device     { nullptr };
		unsigned    _retry      { 0 };
		unsigned    _backoff    { Mbim::BACKOFF_START };
		guint32     _session_id { 0 };
		Connection  _connection { };
		GError     *_error      { nullptr };

		Network      _network      { };
		State_report _state_report { };
		bool         _report_atds  { true };

		Signal_handler<Mbim> _config_handler {
			_env.ep(), *this, &Mbim::_report_config };

		static Mbim *_mbim(gpointer user_data)
		{
			return reinterpret_cast<Mbim *>(user_data);
		}

		MbimMessage *_command_response(GAsyncResult *res, bool may_fail = false)
		{
			GError *error         = nullptr;
			MbimMessage *response = mbim_device_command_finish (_device, res, &error);

			_error = nullptr;

			if (!response ||
			    !mbim_message_response_get_result(response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error)) {

				_error = error;

				if (may_fail) return response;

				warning("operation failed: ", error ? (char const*)error->message : "", " ", error ? error->code : 0);
				return nullptr;
			}

			return response;
		}

		gchar const *_pin()
		{
			//XXX: hide
			return _network.pin.string();
		}

		static void _device_close_ready (MbimDevice   *,
		                                 GAsyncResult *res, gpointer user_data)
		{
			Mbim *mbim = reinterpret_cast<Mbim *>(user_data);
			g_autoptr(GError) error = nullptr;
			if (!mbim_device_close_finish(mbim->_device, res, &error)) {
				Genode::error("couldn't close device: ", (char const*)error->message);
				g_error_free(error);
			}

			g_main_loop_quit(mbim->_loop);
		}

		void _shutdown(gboolean)
		{
			/* Set the in-session setup */
			g_object_set(_device,
			             MBIM_DEVICE_IN_SESSION, FALSE,
			             nullptr);

			/* Close the device */
			mbim_device_close(_device,
			                  15,
			                  nullptr,
			                  (GAsyncReadyCallback) _device_close_ready,
			                  this);
		}

		void _send_request()
		{
			g_autoptr(MbimMessage) request = nullptr;
			g_autoptr(GError)      error   = nullptr;

			switch (_state) {

				case NONE:
					request = mbim_message_subscriber_ready_status_query_new(nullptr);
					if (!request) {
						Genode::error("couldn't create request: ", (char const*)error->message);
						_shutdown (FALSE);
						return;
					}
					
					mbim_device_command(_device,
					                    request,
					                    10,
					                    nullptr,
					                    (GAsyncReadyCallback)_subscriber_state,
					                    this);
					break;

				case UNLOCK:
					request = (mbim_message_pin_set_new(MBIM_PIN_TYPE_PIN1,
					                                    MBIM_PIN_OPERATION_ENTER,
					                                    _pin(),
					                                    nullptr,
					                                    &error));
					if (!request) {
						Genode::error("couldn't create request: ", (char const*)error->message);
						_shutdown (FALSE);
						return;
					}

					mbim_device_command(_device,
					                    request,
					                    10,
					                    nullptr,
					                    (GAsyncReadyCallback)_pin_ready,
					                    this);
					break;

				case PIN:
					request = mbim_message_register_state_query_new(nullptr);
					if (!request) {
						Genode::error("couldn't create request: ", (char const*)error->message);
						_shutdown (FALSE);
						return;
					}

					mbim_device_command(_device,
					                    request,
					                    10,
					                    nullptr,
					                    (GAsyncReadyCallback)_register_state,
					                    this);
					break;

				case QUERY:
					request = mbim_message_packet_service_set_new(MBIM_PACKET_SERVICE_ACTION_ATTACH
					                                              , &error);
					if (!request) {
						Genode::error("couldn't create request: ", (char const *)error->message);
						_shutdown(FALSE);
						return;
					}

					mbim_device_command(_device,
					                    request,
					                    120,
					                    nullptr,
					                    (GAsyncReadyCallback)_packet_service_ready,
					                    this);
					break;

				case ATTACH: {
					guint32            session_id { 0 };
					gchar             *apn { (gchar *)_network.apn.string() };
					MbimAuthProtocol   auth_protocol { MBIM_AUTH_PROTOCOL_PAP };
					gchar             *username { (char *)_network.user.string() };
					gchar             *password { (char *)_network.password.string() };
					MbimContextIpType  ip_type { MBIM_CONTEXT_IP_TYPE_DEFAULT };

					request = mbim_message_connect_set_new(session_id,
					                                       MBIM_ACTIVATION_COMMAND_ACTIVATE,
					                                       apn,
					                                       username,
					                                       password,
					                                       MBIM_COMPRESSION_NONE,
					                                       auth_protocol,
					                                       ip_type,
					                                       mbim_uuid_from_context_type (MBIM_CONTEXT_TYPE_INTERNET),
					                                       &error);

					if (!request) {
						Genode::error("couldn't create request: ", (char const *)error->message);
						_shutdown (FALSE);
						return;
					}

					mbim_device_command (_device,
					                     request,
					                     120,
					                     nullptr,
					                     (GAsyncReadyCallback)_connect_ready,
					                     this);
					break;
				}

				case CONNECT:
					request = (mbim_message_ip_configuration_query_new (
					             _session_id,
					             MBIM_IP_CONFIGURATION_AVAILABLE_FLAG_NONE, /* ipv4configurationavailable */
					             MBIM_IP_CONFIGURATION_AVAILABLE_FLAG_NONE, /* ipv6configurationavailable */
					             0, /* ipv4addresscount */
					             nullptr, /* ipv4address */
					             0, /* ipv6addresscount */
					             nullptr, /* ipv6address */
					             nullptr, /* ipv4gateway */
					             nullptr, /* ipv6gateway */
					             0, /* ipv4dnsservercount */
					             nullptr, /* ipv4dnsserver */
					             0, /* ipv6dnsservercount */
					             nullptr, /* ipv6dnsserver */
					             0, /* ipv4mtu */
					             0, /* ipv6mtu */
					             &error));
					if (!request) {
						Genode::error("couldn't create IP config request: ", (char const *)error->message);
						_shutdown (FALSE);
						return;
					}

					mbim_device_command (_device,
					                     request,
					                     60,
					                     nullptr,
					                     (GAsyncReadyCallback)_ip_configuration_query_ready,
					                     this);
					break;
				case READY:
					break;
			}
		}

		static void _pin_ready(MbimDevice   *,
		                       GAsyncResult *res, gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);
			g_autoptr(GError)      error    = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res, true);

			MbimPinType  pin_type;
			MbimPinState pin_state;
			guint32      remaining_attempts;
			if (!mbim_message_pin_response_parse(response,
			                                     &pin_type,
			                                     &pin_state,
			                                     &remaining_attempts,
			                                     &error)) {
				Genode::error("couldn't parse response message: ", (char const *)error->message);
				mbim->_shutdown (FALSE);
				return;
			}

			mbim->_state_report.sim = mbim_pin_state_get_string(pin_state);
			mbim->_report_state();

			if (mbim->_error && mbim->_error->code == MBIM_STATUS_ERROR_FAILURE &&
			    pin_state != MBIM_PIN_STATE_UNLOCKED) {
				Genode::error("Unable to unlock SIM card. Wrong PIN?"
				              " Remaining attempts: ", remaining_attempts);
				mbim->_shutdown(FALSE);
				return;
			}

			if (TRACE)
				log("PIN: state: ", mbim_pin_state_get_string(pin_state),
				            " remaining attempts: ", remaining_attempts);

			mbim->_state = Mbim::PIN;
			mbim->_send_request();
		}

		static void _subscriber_state(MbimDevice   *,
		                              GAsyncResult *res, gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);
			g_autoptr(GError)      error    = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res);

			if (!response) return;

			MbimSubscriberReadyState ready_state;

			if (!mbim_message_subscriber_ready_status_response_parse(response,
			                                                         &ready_state,
			                                                         nullptr,
			                                                         nullptr,
			                                                         nullptr,
			                                                         nullptr,
			                                                         nullptr,
			                                                         &error)) {
				Genode::error("couldn't parse response message: ", (char const *)error->message);
				mbim->_shutdown (FALSE);
				return;
			}

			mbim->_state_report.sim = mbim_subscriber_ready_state_get_string(ready_state);
			mbim->_report_state();

			if (ready_state == MBIM_SUBSCRIBER_READY_STATE_DEVICE_LOCKED) {
				mbim->_state = Mbim::UNLOCK;
				mbim->_send_request();
				return;
			}

			if (ready_state != MBIM_SUBSCRIBER_READY_STATE_INITIALIZED && mbim->_state == Mbim::NONE) {
				Genode::error("subscriber not initialized: ",
				              mbim_subscriber_ready_state_get_string(ready_state));
				mbim->_shutdown (FALSE);
				return;
			}

			mbim->_state = Mbim::PIN;
			mbim->_send_request();
		}

		static void _register_state(MbimDevice   *,
		                            GAsyncResult *res, gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);
			g_autoptr(GError)      error    = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res);

			if (!response) return;

			MbimNwError          nw_error;
			MbimRegisterState    register_state;
			MbimRegisterMode     register_mode;
			MbimDataClass        available_data_classes;
			MbimCellularClass    cellular_class;
			g_autofree gchar    *provider_id = nullptr;
			g_autofree gchar    *provider_name = nullptr;
			g_autofree gchar    *roaming_text = nullptr;
			MbimRegistrationFlag registration_flag;
			if (!mbim_message_register_state_response_parse(response,
			                                                &nw_error,
			                                                &register_state,
			                                                &register_mode,
			                                                &available_data_classes,
			                                                &cellular_class,
			                                                &provider_id,
			                                                &provider_name,
			                                                &roaming_text,
			                                                &registration_flag,
			                                                &error)) {
				Genode::error("couldn't parse response message: ", (char const *)error->message);
				mbim->_shutdown (FALSE);
				return;
			}

			/* store info for state report */
			mbim->_state_report.error      = mbim_nw_error_get_string(nw_error);
			mbim->_state_report.network    = mbim_register_state_get_string(register_state);
			mbim->_state_report.provider   = Cstring(provider_name);
			mbim->_state_report.data_class = Cstring(mbim_data_class_build_string_from_mask(available_data_classes));
			mbim->_state_report.roaming    = Cstring(roaming_text);
			mbim->_report_state();

			if (register_state == MBIM_REGISTER_STATE_HOME ||
			    register_state == MBIM_REGISTER_STATE_ROAMING ||
			    register_state == MBIM_REGISTER_STATE_PARTNER) {
				/* check our state to allow polling registered state periodically
				 * even if we're already connected */
				if (mbim->_state == Mbim::PIN) {
					mbim->_state = Mbim::QUERY;
					mbim->_retry = 0;
					mbim->_send_request();
					return;
				}
			}
			else {
				/* reset state and wait+retry until registered */
				mbim->_state = Mbim::PIN;
			}

			if ((++mbim->_retry) % 10 == 0)
				warning("Device not registered after ", mbim->_retry, " tries");

			/*
			 * We delay request retries to leave device time for network
			 * registration. The delay is based on exponential backoff with
			 * upper bound.
			 */
			guint const delay = mbim->_backoff < unsigned(Mbim::BACKOFF_LIMIT)
			                  ? mbim->_backoff *= 2
			                  : guint(Mbim::BACKOFF_LIMIT);
			g_timeout_add(delay, _handle_timeout, mbim);
		}

		static gboolean _handle_timeout(gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);
			mbim->_send_request();

			/* discard timer */
			return FALSE;
		}

		static void _packet_service_ready(MbimDevice   *,
		                                  GAsyncResult *res, gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);
			GError *error                   = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res);

			/* clear backoff upon successful connection */
			mbim->_backoff = Mbim::BACKOFF_START;

			if (!response) return;

			guint32                 nw_error;
			MbimPacketServiceState  packet_service_state;
			MbimDataClass           highest_available_data_class;
			g_autofree gchar       *highest_available_data_class_str = nullptr;
			guint64                 uplink_speed;
			guint64                 downlink_speed;
			if (!mbim_message_packet_service_response_parse(response,
			                                                &nw_error,
			                                                &packet_service_state,
			                                                &highest_available_data_class,
			                                                &uplink_speed,
			                                                &downlink_speed,
			                                                &error)) {
				Genode::error("couldn't parse response message: ", (char const *)error->message);
				mbim->_shutdown (FALSE);
				return;
			}

			mbim->_state = Mbim::ATTACH;

			highest_available_data_class_str = mbim_data_class_build_string_from_mask (highest_available_data_class);
			log("Successfully attached packet service");

			if (TRACE)
				log("Packet service status:\n",
				    "\tAvailable data classes: '", (char const *)highest_available_data_class_str, "'\n",
				    "\t          Uplink speed: '", uplink_speed, "'\n",
				    "\t        Downlink speed: '", downlink_speed, "'");

			mbim->_send_request();
		}

		static void _connect_ready(MbimDevice   *,
		                           GAsyncResult *res, gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);
			GError *error                   = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res);

			if (!response) return;

			guint32              session_id;
			MbimActivationState  activation_state;
			MbimVoiceCallState   voice_call_state;
			MbimContextIpType    ip_type;
			const MbimUuid      *context_type;
			guint32              nw_error;
			if (!mbim_message_connect_response_parse (
			        response,
			        &session_id,
			        &activation_state,
			        &voice_call_state,
			        &ip_type,
			        &context_type,
			        &nw_error,
			        &error)) {
				Genode::error("couldn't parse response message: ", (char const *)error->message);
				mbim->_shutdown(FALSE);
				return;
			}

			mbim->_session_id = session_id;
			mbim->_state      = Mbim::CONNECT;
			mbim->_send_request();
		}

		static void _ip_configuration_query_ready(MbimDevice   *,
		                                          GAsyncResult *res,
		                                          gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);
			GError *error                   = nullptr;
			g_autoptr(MbimMessage) response = mbim->_command_response(res);

			if (!response) return;

			MbimIPConfigurationAvailableFlag  ipv4configurationavailable;
			g_autofree gchar                 *ipv4configurationavailable_str = nullptr;
			MbimIPConfigurationAvailableFlag  ipv6configurationavailable;
			g_autofree gchar                 *ipv6configurationavailable_str = nullptr;
			guint32                           ipv4addresscount;
			g_autoptr(MbimIPv4ElementArray)   ipv4address = nullptr;
			guint32                           ipv6addresscount;
			g_autoptr(MbimIPv6ElementArray)   ipv6address = nullptr;
			const MbimIPv4                   *ipv4gateway;
			const MbimIPv6                   *ipv6gateway;
			guint32                           ipv4dnsservercount;
			g_autofree MbimIPv4              *ipv4dnsserver = nullptr;
			guint32                           ipv6dnsservercount;
			g_autofree MbimIPv6              *ipv6dnsserver = nullptr;
			guint32                           ipv4mtu;
			guint32                           ipv6mtu;

			if (!mbim_message_ip_configuration_response_parse(
			        response,
			        nullptr, /* sessionid */
			        &ipv4configurationavailable,
			        &ipv6configurationavailable,
			        &ipv4addresscount,
			        &ipv4address,
			        &ipv6addresscount,
			        &ipv6address,
			        &ipv4gateway,
			        &ipv6gateway,
			        &ipv4dnsservercount,
			        &ipv4dnsserver,
			        &ipv6dnsservercount,
			        &ipv6dnsserver,
			        &ipv4mtu,
			        &ipv6mtu,
			        &error))
			return;

			if (!ipv4configurationavailable) {
				Genode::error("No ipv4 configuration available");
				return;
			}

			Net::Ipv4_address address { ipv4address[0]->ipv4_address.addr };
			Net::Ipv4_address gateway { (void *)ipv4gateway->addr };

			log("ip     : ",  address, "/", ipv4address[0]->on_link_prefix_length);
			log("gateway: ",  gateway);

			Net::Ipv4_address dns[2];
			for (uint32_t i = 0; i < ipv4dnsservercount && i < 2; i++) {
				dns[i] = Net::Ipv4_address { (void *)ipv4dnsserver[i].addr };
				log("dns", i, "   : ", dns[i]);
			}

			mbim->_connection.ip        = address;
			mbim->_connection.mask      = ipv4address[0]->on_link_prefix_length;
			mbim->_connection.gateway   = gateway;
			mbim->_connection.dns[0]    = dns[0];
			mbim->_connection.dns[1]    = dns[1];
			mbim->_connection.connected = true;
			mbim->_state = Mbim::READY;
			mbim->_report_config();
		}

		static void _device_open_ready(MbimDevice   *dev,
		                               GAsyncResult *res, gpointer user_data)
		{
			GError *error = nullptr;
			Mbim *mbim = reinterpret_cast<Mbim *>(user_data);
			if (!mbim_device_open_finish(dev, res, &error)) {
				Genode::error("couldn't open the MbimDevice: ",
				              (char const *)error->message);
				exit (EXIT_FAILURE);
			}

			mbim->_retry = 0;
			mbim->_send_request();
		}


		static void _device_new_ready(GObject *, GAsyncResult *res, gpointer user_data)
		{
			Mbim *mbim = reinterpret_cast<Mbim *>(user_data);
			GError *error = nullptr;
			MbimDeviceOpenFlags open_flags = MBIM_DEVICE_OPEN_FLAGS_NONE;

			mbim->_device = mbim_device_new_finish (res, &error);
			if (!mbim->_device) {
				Genode::error("couldn't create MbimDevice: ",
				              (char const*)error->message);
				exit (EXIT_FAILURE);
			}

			/* register handler for status messages */
			g_signal_connect(mbim->_device,
			                 MBIM_DEVICE_SIGNAL_INDICATE_STATUS,
			                 G_CALLBACK(_handle_indicate_status),
			                 mbim);

			/* register handler for hangup messages */
			g_signal_connect(mbim->_device,
			                 MBIM_DEVICE_SIGNAL_REMOVED,
			                 G_CALLBACK(_handle_hangup),
			                 mbim);

			if (!mbim_device_is_open(mbim->_device)) {
				if (TRACE)
					log("opening device");
				mbim_device_open_full(mbim->_device,
				                      open_flags,
				                      45,
				                      nullptr,
				                      (GAsyncReadyCallback) _device_open_ready,
				                      mbim);
			}
			else {
				mbim->_retry = 0;
				mbim->_send_request();
			}
		}

		static void _signal_state(GObject *, GAsyncResult *res, gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);
			GError *error                   = nullptr;
			g_autoptr(MbimMessage) response = mbim_device_command_finish(mbim->_device,
			                                                             res,
			                                                             &error);

			/* disable signal state report on timeout error -> not supported */
			if (error &&
			    g_error_matches(error, MBIM_CORE_ERROR, MBIM_CORE_ERROR_TIMEOUT)) {
				mbim->_report_atds = false;
				mbim->_report_state();
				return;
			}

			if (!response ||
			    !mbim_message_response_get_result(response,
			                                      MBIM_MESSAGE_TYPE_COMMAND_DONE,
			                                      &error)) {
				mbim->_report_state();
				return;
			}

			uint32_t rsrq, rsrp, rssnr;

			mbim_message_atds_signal_response_parse(response,
			                                        nullptr, /* rssi */
			                                        nullptr, /* error_rate */
			                                        nullptr, /* rscp */
			                                        nullptr, /* ecno */
			                                        &rsrq,
			                                        &rsrp,
			                                        &rssnr,
			                                        &error);

			mbim->_state_report.rsrq  = -19.5f + ((float)rsrq/2);
			mbim->_state_report.rsrp  = -140l + rsrp;
			mbim->_state_report.rssnr = -5.0f + rssnr;

			mbim->_report_state();
		}

		static void _log_handler(const gchar *,
		                         GLogLevelFlags log_level,
		                         const gchar *message,
		                         gpointer)
		{
			String level;
			switch (log_level) {
				case G_LOG_LEVEL_WARNING:
					level = "[Warning]";
					break;
				case G_LOG_LEVEL_CRITICAL:
				case G_LOG_LEVEL_ERROR:
					level = "[Error]";
					break;
				case G_LOG_LEVEL_DEBUG:
					level = "[Debug]";
					break;
				case G_LOG_LEVEL_MESSAGE:
				case G_LOG_LEVEL_INFO:
					level = "[Info]";
				break;
				case G_LOG_FLAG_FATAL:
				case G_LOG_LEVEL_MASK:
				case G_LOG_FLAG_RECURSION:
				default:
					g_assert_not_reached ();
			}

				log(level, " ", message);
		}

		void _init()
		{
			if (TRACE) {
				g_log_set_handler(nullptr, G_LOG_LEVEL_MASK, _log_handler, nullptr);
				g_log_set_handler("Mbim", G_LOG_LEVEL_MASK, _log_handler, nullptr);
			}
			mbim_utils_set_traces_enabled(TRACE);

			g_autoptr(GFile) file = nullptr;
			file = g_file_new_for_commandline_arg ("/dev/cdc-wdm0");
			_loop = g_main_loop_new(nullptr, FALSE);
			mbim_device_new(file, nullptr, (GAsyncReadyCallback)_device_new_ready, this);
		}

		static void _handle_indicate_status(MbimDevice *,
		                             MbimMessage* msg,
		                             gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);

			GError     *error   = nullptr;
			MbimService service = mbim_message_indicate_status_get_service(msg);
			guint32     cid     = mbim_message_indicate_status_get_cid(msg);

			if (service == MBIM_SERVICE_BASIC_CONNECT) {
				switch(cid) {

					case MBIM_CID_BASIC_CONNECT_SIGNAL_STATE: {
						uint32_t rssi;
						if (!mbim_message_signal_state_notification_parse(msg,
						                                                  &rssi,
						                                                  nullptr,
						                                                  nullptr,
						                                                  nullptr,
						                                                  nullptr,
						                                                  &error)) {
							Genode::error("couldn't parse signal state notification message: ",
							              (char const *)error->message);
							return;
						}

						mbim->_state_report.rssi = -113l-2*rssi;

						/* handle RSSI connection-state change */
						if (rssi > RSSI_DISCONNECT) {
							if (mbim->_connection.connected) {
								mbim->_connection.connected = false;
								mbim->_report_config();
								mbim->_state = NONE;
							}
						} else {
							if (!mbim->_connection.connected) {
								mbim->_connection.connected = true;
								mbim->_report_config();
								mbim->_send_request();
							}
						}

						{
							if (mbim->_state != READY || !mbim->_report_atds) {
								mbim->_report_state();
								break;
							}

							/* retrieve signal states from AT&T device service */
							g_autoptr(MbimMessage) request = nullptr;
							request = (mbim_message_atds_signal_query_new(nullptr));
							mbim_device_command(mbim->_device,
							                    request,
							                    10,
							                    nullptr,
							                    (GAsyncReadyCallback)_signal_state,
							                    mbim);
						}
						break;
					}
					case MBIM_CID_BASIC_CONNECT_REGISTER_STATE:
					{
						MbimNwError          nw_error;
						MbimRegisterState    register_state;
						MbimDataClass        available_data_classes;
						g_autofree gchar    *provider_name = nullptr;
						g_autofree gchar    *roaming_text = nullptr;

						if (!mbim_message_register_state_notification_parse(msg,
						                                                    &nw_error,
						                                                    &register_state,
						                                                    nullptr,
						                                                    &available_data_classes,
						                                                    nullptr,
						                                                    nullptr,
						                                                    &provider_name,
						                                                    &roaming_text,
						                                                    nullptr,
						                                                    &error)) {
							Genode::error("couldn't parse register state notification message: ",
							              (char const *)error->message);
							return;
						}

						/* store info for state report */
						mbim->_state_report.error      = mbim_nw_error_get_string(nw_error);
						mbim->_state_report.network    = mbim_register_state_get_string(register_state);
						mbim->_state_report.provider   = Cstring(provider_name);
						mbim->_state_report.data_class = Cstring(mbim_data_class_build_string_from_mask(available_data_classes));
						mbim->_state_report.roaming    = Cstring(roaming_text);
						mbim->_report_state();

						if (register_state != MBIM_REGISTER_STATE_HOME &&
						    register_state != MBIM_REGISTER_STATE_ROAMING &&
						    register_state != MBIM_REGISTER_STATE_PARTNER) {
							warning("Lost network registration");
						}

						break;
					}
					case MBIM_CID_BASIC_CONNECT_PACKET_SERVICE:
					case MBIM_CID_BASIC_CONNECT_CONNECT:
						/* ignore */
						break;
					case MBIM_CID_BASIC_CONNECT_SUBSCRIBER_READY_STATUS:
					{
						MbimSubscriberReadyState ready_state;

						if (!mbim_message_subscriber_ready_status_notification_parse(msg,
						                                                             &ready_state,
						                                                             nullptr,
						                                                             nullptr,
						                                                             nullptr,
						                                                             nullptr,
						                                                             nullptr,
						                                                             &error)) {
							Genode::error("couldn't parse notification message: ", (char const *)error->message);
							mbim->_shutdown (FALSE);
							return;
						}

						mbim->_state_report.sim = mbim_subscriber_ready_state_get_string(ready_state);
						mbim->_report_state();

						if (ready_state == MBIM_SUBSCRIBER_READY_STATE_DEVICE_LOCKED) {
							/* unlock with PIN */
							mbim->_state = Mbim::UNLOCK;
							mbim->_send_request();
						}
						else if (ready_state == MBIM_SUBSCRIBER_READY_STATE_INITIALIZED) {
							if (mbim->_state == Mbim::NONE) {
								/* jump ahead and wait for network registration */
								mbim->_state = Mbim::PIN;
								mbim->_send_request();
							}
						}
						else {
							/* reset */
							mbim->_state = Mbim::NONE;
							mbim->_send_request();
						}

						break;
					}
					default:
						const gchar *cid_printable = mbim_cid_get_printable(mbim_message_indicate_status_get_service (msg),
						                                                    mbim_message_indicate_status_get_cid (msg));
						warning("Received unknown status message with cid: ", cid_printable);
						warning(Cstring(mbim_message_get_printable(msg, "  ", FALSE)));
				}
			}
		}

		static void _handle_hangup(MbimDevice *,
		                    gpointer user_data)
		{
			Mbim *mbim = _mbim(user_data);

			warning("Device hung-up. Reconnecting...");
			mbim->_state = Mbim::PIN;
			mbim->_send_request();
		}

		void _connect()
		{
			g_main_loop_run(_loop);
			g_object_unref(_device);
			g_main_loop_unref(_loop);
		}

		void _report_state()
		{
			_broadband_state.report(_state_report);
		}

		void _report_config()
		{
			if (_state != Mbim::READY)
				return;

			_broadband_config.report(_connection);
		}

	public:

		Mbim(Libc::Env &env) : _env(env)
		{
			_config_rom.node().with_sub_node("network",
				[&] (Node const &net) {
					_network.apn      = net.attribute_value("apn", String());
					_network.user     = net.attribute_value("user", String());
					_network.password = net.attribute_value("password", String());
					_network.pin      = net.attribute_value("pin", String());
				}, [] {
					Genode::error("No valid <network> configuration found");
					exit(1);
				});

			_init();
			_connect();
		}

		Mbim(Mbim const &) = delete;
		Mbim & operator = (Mbim const &) = delete;
};


void Libc::Component::construct(Libc::Env &env)
{
	Libc::with_libc([&] () {
		static Mbim main { env };
	});

	exit(0);
}
