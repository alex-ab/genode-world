/*
 * \brief  QMI connection bindings
 * \author Sebastian Sumpf
 * \date   2026-03-12
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

#include <base/attached_rom_dataspace.h>
#include <libc/component.h>
#include <libqmi-glib.h>
#include <os/reporter.h>
#include <net/ipv4.h>

#include <broadband.h>

using namespace Genode;
using namespace Net;
using namespace Broadband;


class Qmi
{
	private:

		enum { TRACE = FALSE };

		enum State { NONE, UNLOCK, PIN, CONNECT, READY };

		enum Client { UNKNOWN, UIM, WDS, NAS };

		enum Backoff {
			BACKOFF_START   = 1000,  /* first retry after one second */
			BACKOFF_LIMIT   = 64000, /* increase retry timeout up to 64 seconds */
			STATUS_INTERVAL = 10000, /* interval status report is created */
		};

		using String       = Genode::String<32>;
		using Connection   = Broadband::Connection;
		using State_report = Broadband::State;

		Env      &_env;

		Reporter _config_reporter { _env, "config", "nic_router.config" };
		Reporter _state_reporter  { _env, "state",  "state" };

		Attached_rom_dataspace _config_rom { _env, "config" };

		Config_reporter _broadband_config { _config_reporter, _config_rom };
		State_reporter  _broadband_state  { _state_reporter };

		State       _state      { NONE };
		GFile      *_file       { nullptr };
		GMainLoop  *_loop       { nullptr };
		QmiDevice  *_device     { nullptr };
		unsigned    _retry      { 0 };
		unsigned    _backoff    { BACKOFF_START };
		Connection  _connection { };

		Network      _network      { };
		State_report _state_report { };

		Client _current_client_type { UNKNOWN };

		QmiClient *_uim_client { nullptr }; /* User Idendity Module (SIM) */
		QmiClient *_wds_client { nullptr }; /* Wireless Data Service      */
		QmiClient *_nas_client { nullptr }; /* Network Access Service     */

		gboolean _autoconnect { FALSE };

		/*
		 * static functions used in/for callbacks
		 */

		static Qmi *_qmi(gpointer user_data)
		{
			return reinterpret_cast<Qmi *>(user_data);
		}

		static void _current_settings_ready(QmiClientWds *client, GAsyncResult *res,
		                                    gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);

			GError *qmi_error = nullptr;
			QmiMessageWdsGetCurrentSettingsOutput *output = nullptr;

			output = qmi_client_wds_get_current_settings_finish(client, res, &qmi_error);
			if (!output) {
				error("No ipv4 configuration available:", (char const *)qmi_error->message);
				g_error_free(qmi_error);
				return;
			}

			if (!qmi_message_wds_get_current_settings_output_get_result(output, &qmi_error)) {
				error("couldn't get current settings: ", (char const *)qmi_error->message);
				g_error_free(qmi_error);
				qmi_message_wds_get_current_settings_output_unref(output);
			}

			guint32 addr = 0;
			if (qmi_message_wds_get_current_settings_output_get_ipv4_address(output,
			                                                                 &addr,
			                                                                 nullptr)) {
				addr = GUINT32_TO_BE(addr);
				Ipv4_address address { (void *)&addr };
				qmi->_connection.ip = address;
			}

			if (qmi_message_wds_get_current_settings_output_get_ipv4_gateway_subnet_mask(
			      output, &addr, nullptr)) {
				/* convert to CIDR */
				uint32_t cidr = 0;
				for (uint32_t i = 0; i < 32; i++)
					if (addr & (1u << i)) cidr++;

				qmi->_connection.mask = cidr;
			}

			if (qmi_message_wds_get_current_settings_output_get_ipv4_gateway_address(
				    output, &addr, nullptr)) {
				addr = GUINT32_TO_BE(addr);
				Ipv4_address address { (void *)&addr };
				qmi->_connection.gateway = address;
			}

			if (qmi_message_wds_get_current_settings_output_get_primary_ipv4_dns_address(
			      output, &addr, nullptr)) {
				addr = GUINT32_TO_BE(addr);
				Ipv4_address address { (void *)&addr };
				qmi->_connection.dns[0]= address;
			}

			if (qmi_message_wds_get_current_settings_output_get_secondary_ipv4_dns_address(
			      output, &addr, nullptr)) {
				addr = GUINT32_TO_BE(addr);
				Ipv4_address address { (void *)&addr };
				qmi->_connection.dns[1]= address;
			}

			log("ip     : ",  qmi->_connection.ip, "/", qmi->_connection.mask);
			log("gateway: ",  qmi->_connection.gateway);
			log("dns1   : ",  qmi->_connection.dns[0]);
			log("dns2   : ",  qmi->_connection.dns[1]);

			qmi->_state = READY;
			qmi->_connection.connected = true;
			qmi->report_config();
			qmi->send_request();
		}

		static gboolean _handle_timeout(gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);
			qmi->send_request();

			/* discard timer */
			return FALSE;
		}

		static void _start_network_ready(QmiClientWds *client, GAsyncResult *res,
		                                 gpointer user_data)
		{
			Qmi *qmi= _qmi(user_data);

			QmiMessageWdsStartNetworkOutput *output = nullptr;
			GError *qmi_error = nullptr;

			output = qmi_client_wds_start_network_finish(client, res, &qmi_error);
			if (!output) {
				warning("operation failed: ", (char const *)qmi_error->message);
				g_error_free(qmi_error);
				return;
			}

			if (!qmi_message_wds_start_network_output_get_result(output, &qmi_error)) {
				/*
				 * No effect happens when autoconnect is in action
				 * Call failed when service is not ready
				 */
				if (g_error_matches(qmi_error, QMI_PROTOCOL_ERROR, QMI_PROTOCOL_ERROR_NO_EFFECT))
					g_error_free(qmi_error);

				else if (g_error_matches(qmi_error, QMI_PROTOCOL_ERROR, QMI_PROTOCOL_ERROR_CALL_FAILED)) {

					if ((++qmi->_retry) % 10 == 0)
						warning("Device not registered after ", qmi->_retry, " tries");

					/*
					 * We delay request retries to leave device time for network
					 * registration. The delay is based on exponential backoff with
					 * upper bound.
					 */
					guint const delay = qmi->_backoff < unsigned(BACKOFF_LIMIT)
					                  ? qmi->_backoff *= 2
					                  : guint(BACKOFF_LIMIT);
					g_timeout_add(delay, _handle_timeout, qmi);
					g_error_free(qmi_error);
					return;
				}

				else {
					/* TODO: retrieve additional info if required (see: qmicli-wds.c) */
					error("couldn't start network: ", (char const *)qmi_error->message, "code: ", qmi_error->code);
					g_error_free(qmi_error);
					qmi_message_wds_start_network_output_unref(output);
					qmi->shutdown();
					return;
				}
			}

			/* we need to use this, in case autoconnect doesn't work
			guint32 packet_data_handle;
			qmi_message_wds_start_network_output_get_packet_data_handle(output,
			                                                            &packet_data_handle,
			                                                            nullptr);
			*/

			qmi_message_wds_start_network_output_unref(output);

			/* clear backoff upon successful connection */
			qmi->_backoff = BACKOFF_START;

			qmi->_state = CONNECT;
			qmi->send_request();
		}

		static void _verify_pin_ready(QmiClientUim *client, GAsyncResult *res,
		                              gpointer user_data)
		{
			Qmi *qmi= _qmi(user_data);

			QmiMessageUimVerifyPinOutput *output = nullptr;
			GError *qmi_error = nullptr;

			output = qmi_client_uim_verify_pin_finish(client, res, &qmi_error);
			if (!output) {
				error("error: operation failed: ", (char const *)qmi_error->message);
				g_error_free(qmi_error);
				qmi->shutdown();
				return;
			}

			if (!qmi_message_uim_verify_pin_output_get_result(output, &qmi_error)) {
				guint8 verify_retries_left;
				guint8 unblock_retries_left;

				error("couldn't verify PIN: ", (char const *)qmi_error->message);
				g_error_free(qmi_error);

				if (qmi_message_uim_verify_pin_output_get_retries_remaining(output,
				                                                            &verify_retries_left,
				                                                            &unblock_retries_left,
				                                                            nullptr)) {
					log("[", qmi_device_get_path_display(qmi->_device), "] ",
					    "Retries left:\n"
					    "\tVerify: ", verify_retries_left, "\n",
					    "\tUnblock: ", unblock_retries_left);
				}

				qmi_message_uim_verify_pin_output_unref(output);
				qmi->shutdown();
				return;
			}

			log("[", qmi_device_get_path_display(qmi->_device), "] "
			    "PIN verified successfully");

			qmi_message_uim_verify_pin_output_unref(output);

			/* re-read card status */
			qmi->_state = NONE;
			qmi->send_request();
		}

		static void _card_status_ready(QmiClientUim *client, GAsyncResult *res,
		                               gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);
			QmiMessageUimGetCardStatusOutput *output;
			GError *qmi_error = nullptr;
			GArray *cards;

			struct Unref
			{
				Qmi                              *qmi      { nullptr };
				QmiMessageUimGetCardStatusOutput *output   { nullptr };
				bool                              shutdown { true };

				Unref(Qmi *qmi,
				      QmiMessageUimGetCardStatusOutput *output)
				: qmi(qmi), output(output) { }

				~Unref()
				{
					qmi_message_uim_get_card_status_output_unref(output);
					if (shutdown) qmi->shutdown();
				}

				Unref(Unref const &) = delete;
				Unref & operator = (Unref const &) = delete;
			};

			output = qmi_client_uim_get_card_status_finish(client, res, &qmi_error);
			if (!output) {
				error("operation failed: ", (char const *)qmi_error->message);
				g_error_free(qmi_error);
				qmi->shutdown();
				return;
			}

			Unref unref { qmi, output };

			if (!qmi_message_uim_get_card_status_output_get_result(output, &qmi_error)) {
				error("couldn't get card status: ", (char const *)qmi_error->message);
				g_error_free(qmi_error);
				return;
			}

			qmi_message_uim_get_card_status_output_get_card_status (output,
				nullptr, nullptr, nullptr, nullptr, &cards, nullptr);

			if (cards->len == 0) {
				error("no SIM-slot found");
				return;
			}

			/* retrieve first slot card only */
			QmiMessageUimGetCardStatusOutputCardStatusCardsElement *card;
			card = &g_array_index(cards, QmiMessageUimGetCardStatusOutputCardStatusCardsElement, 0);

			if (card->card_state != QMI_UIM_CARD_STATE_PRESENT) {
				error("SIM card in slot 1 not present or erroneous");
				return;
			}

			/* search for usim (4G etc), isim is IP-sim for VOIP */
			QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2 *app = nullptr;
			for (guint i = 0; i < card->applications->len; i++) {
				app = &g_array_index(card->applications,
				                     QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2,
				                     i);
				if (app->type == QMI_UIM_CARD_APPLICATION_TYPE_USIM)
					break;
			}

			if (app) {
				qmi->_state_report.sim= qmi_uim_pin_state_get_string(app->pin1_state);
				qmi->report_state();
			}

			if (!app ||
			    (app->state != QMI_UIM_CARD_APPLICATION_STATE_READY &&
			     app->state != QMI_UIM_CARD_APPLICATION_STATE_PIN1_OR_UPIN_PIN_REQUIRED))
			{
				error("unsupported PIN state: ",
				       app ? qmi_uim_card_application_state_get_string(app->state) : "no app");
				return;
			}

			if (TRACE)
				log("supported PIN state: ",
				    qmi_uim_card_application_state_get_string(app->state));

			if (app->state == QMI_UIM_CARD_APPLICATION_STATE_READY)
				qmi->_state = PIN;
			else
				qmi->_state = UNLOCK;

			qmi->send_request();
			unref.shutdown = false;
		}

		static void _allocate_client_ready(QmiDevice *dev, GAsyncResult *res,
		                                   gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);
			GError *qmi_error = nullptr;

			QmiClient *client = qmi_device_allocate_client_finish(dev, res, &qmi_error);

			if (!client) {
				error("couldn't create client for service: ",
				       unsigned(qmi->_current_client_type), ":",
				      (char const *)qmi_error->message);
				qmi->shutdown();
			}

			switch (qmi->_current_client_type) {
			case UIM: qmi->_uim_client = client; break;
			case WDS: qmi->_wds_client = client; break;
			case NAS: qmi->_nas_client = client; break;
			case UNKNOWN: break;
			}

			qmi->_current_client_type = UNKNOWN;

			qmi->_retry = 0;
			qmi->send_request();
		}

		static void _release_client_ready(QmiDevice *dev, GAsyncResult *res,
		                                  gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);
			GError *qmi_error = nullptr;

			if (!qmi_device_release_client_finish (dev, res, &qmi_error)) {
				error("couldn't release client: ", (char const *)qmi_error->message);
				g_error_free(qmi_error);
			} else
				g_debug("Client released");

			switch (qmi->_current_client_type) {
			case UIM:
				g_object_unref(qmi->_uim_client);
				qmi->_uim_client = nullptr;
				break;
			case WDS:
				g_object_unref(qmi->_wds_client);
				qmi->_wds_client = nullptr;
				break;
			case NAS:
				g_object_unref(qmi->_nas_client);
				qmi->_nas_client = nullptr;
				break;
			case UNKNOWN: break;
			}

			qmi->_current_client_type = UNKNOWN;

			qmi->shutdown();
		}

		static void _device_open_ready(QmiDevice *dev,
		                               GAsyncResult *res, gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);
			GError *qmi_error = nullptr;

			if (!qmi_device_open_finish(dev, res, &qmi_error)) {
				error("couldn't open the QmiDevice: ", (char const *)qmi_error->message);
				qmi->shutdown();
			}

			qmi->_retry = 0;
			qmi->send_request();
		}

		static void _device_close_ready(QmiDevice *dev, GAsyncResult *res,
		                                gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);
			GError *qmi_error = nullptr;

			if (!qmi_device_close_finish(dev, res, &qmi_error)) {
				error("error: couldn't close:", (char const *)qmi_error->message);
				g_error_free(qmi_error);
			} else
				g_debug("Device closed");

			if (qmi->_device) {
				g_object_unref(qmi->_device);
				qmi->_device = nullptr;
			}

			qmi->shutdown();
		}

		static void _device_new_ready(GObject *, GAsyncResult *res, gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);
			GError *qmi_error = nullptr;
			QmiDeviceOpenFlags open_flags = QMI_DEVICE_OPEN_FLAGS_NONE;

			qmi->_device = qmi_device_new_finish(res, &qmi_error);
			if (!qmi->_device) {
				error("couldn't create QmiDevice: ", (char const*)qmi_error->message);
				qmi->shutdown();
			}

			/* register handler for hangup messages */
			g_signal_connect(qmi->_device,
			                 QMI_DEVICE_SIGNAL_REMOVED,
			                 G_CALLBACK(_handle_hangup),
			                 qmi);

			if (!qmi_device_is_open(qmi->_device)) {
				if (TRACE) log("opening device");
				qmi_device_open(qmi->_device,
				                open_flags,
				                45,
				                nullptr,
				                (GAsyncReadyCallback)_device_open_ready,
				                qmi);
			}
			else {
				qmi->_retry = 0;
				qmi->send_request();
			}
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
					g_assert_not_reached();
			}

				log(level, " ", message);
		}

		/*
		 * private member functions
		 */

		QmiMessageWdsStartNetworkInput *_network_input_create()
		{
			/* Create input bundle */
			QmiMessageWdsStartNetworkInput *input = nullptr;

			input = qmi_message_wds_start_network_input_new();

			/* pap */
			qmi_message_wds_start_network_input_set_authentication_preference(
				input, QMI_WDS_AUTHENTICATION_PAP, nullptr);

			/* IPv4 */
			qmi_message_wds_start_network_input_set_ip_family_preference(
				input, QMI_WDS_IP_FAMILY_IPV4, nullptr);

			if (_network.apn.valid())
				qmi_message_wds_start_network_input_set_apn(input,
				                                            (gchar *)_network.apn.string(),
				                                            nullptr);

			/* Set username, avoid empty strings */
			if (_network.user.valid())
				qmi_message_wds_start_network_input_set_username(input,
					(gchar *)_network.user.string(), nullptr);

			/* Set password, avoid empty strings */
			if (_network.password.valid())
				qmi_message_wds_start_network_input_set_password(input,
					(gchar *)_network.password.string(), nullptr);

			/* configure autoconnect */
			qmi_message_wds_start_network_input_set_enable_autoconnect(input, _autoconnect,
			                                                           nullptr);

			return input;
		}

		gchar const *_pin()
		{
			//XXX: hide
			return _network.pin.string();
		}

		QmiMessageUimVerifyPinInput *_pin_create_input()
		{
			/* slot1/pin1 */
			QmiUimSessionType session_type = QMI_UIM_SESSION_TYPE_CARD_SLOT_1;
			QmiUimPinId       pin_id       = QMI_UIM_PIN_ID_PIN1;

			GArray                      *placeholder_aid  = nullptr;
			QmiMessageUimVerifyPinInput *input            = nullptr;

			input = qmi_message_uim_verify_pin_input_new();

			GError *qmi_error = nullptr;
			if (!qmi_message_uim_verify_pin_input_set_info(input,
			                                               pin_id,
			                                               _pin(),
			                                               &qmi_error)) {
				error("set pin info failed: ", (char const *)qmi_error->message);
				input = nullptr;
			}

			placeholder_aid = g_array_new(FALSE, FALSE, sizeof(guint8));
			if(!qmi_message_uim_verify_pin_input_set_session(input,
			                                                 session_type,
			                                                 placeholder_aid, /* ignored */
			                                                 &qmi_error)) {
				error("set pin session failed: ", (char const *)qmi_error->message);
				input = nullptr;
			}
			g_array_unref(placeholder_aid);

			if (!input) {
				qmi_message_uim_verify_pin_input_unref(input);
				shutdown();
			}

			g_clear_error (&qmi_error);
			return input;
		}

		static void _handle_hangup(QmiDevice *, gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);

			warning("Device hung-up. Reconnecting...");
			qmi->_state = PIN;
			qmi->send_request();
		}


		/*
		 * Network status report
		 */

		static void _serving_system_ready(QmiClientNas *client,
		                                  GAsyncResult *res,
		                                  gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);
			GError *qmi_error = nullptr;

			QmiMessageNasGetServingSystemOutput *output;
			output = qmi_client_nas_get_serving_system_finish(client, res,
			                                                  &qmi_error);
			if (!output) {
				error("operation failed: ", (char const *)qmi_error->message);
				g_error_free(qmi_error);
				return;
			}
			if (!qmi_message_nas_get_serving_system_output_get_result(output,
			                                                          &qmi_error)) {
				error("couldn't get serving system: ",
				      (char const *)qmi_error->message);
				g_error_free(qmi_error);
				qmi_message_nas_get_serving_system_output_unref(output);
				return;
			}

			QmiNasRegistrationState registration_state;
			GArray *radio_interfaces;

			qmi_message_nas_get_serving_system_output_get_serving_system(
			  output,
			  &registration_state,
			  nullptr,
			  nullptr,
			  nullptr,
			  &radio_interfaces,
			  nullptr);

			State_report &report = qmi->state_report();

			/* registration state */
			report.network =
			  Cstring(qmi_nas_registration_state_get_string(registration_state));

			/* data class */
			if (radio_interfaces->len > 1)
				warning("found  ", radio_interfaces->len, " radio interfaces, using first");

			if (radio_interfaces->len > 0) {
				QmiNasRadioInterface iface;
				iface = g_array_index(radio_interfaces, QmiNasRadioInterface, 0);
				report.data_class = Cstring(qmi_nas_radio_interface_get_string(iface));
			}

			/* roaming */
			QmiNasRoamingIndicatorStatus roaming;

			if (qmi_message_nas_get_serving_system_output_get_roaming_indicator(
			      output, &roaming, nullptr))
				report.roaming =
				  Cstring(qmi_nas_roaming_indicator_status_get_string(roaming));

			/* provider */
			const gchar *current_plmn_description;
			if (qmi_message_nas_get_serving_system_output_get_current_plmn(
			      output,
			      nullptr,
			      nullptr,
			      &current_plmn_description,
			      nullptr))
				report.provider = Cstring(current_plmn_description);

			qmi_message_nas_get_serving_system_output_unref(output);

			qmi->report_state();
		}

		QmiMessageNasGetSignalStrengthInput * _signal_strength_input_create(void)
		{
			GError *qmi_error = nullptr;
			QmiMessageNasGetSignalStrengthInput *input;
			QmiNasSignalStrengthRequest mask;

			mask = QmiNasSignalStrengthRequest(QMI_NAS_SIGNAL_STRENGTH_REQUEST_RSSI    |
			                                   QMI_NAS_SIGNAL_STRENGTH_REQUEST_RSRQ    |
			                                   QMI_NAS_SIGNAL_STRENGTH_REQUEST_LTE_SNR |
			                                   QMI_NAS_SIGNAL_STRENGTH_REQUEST_LTE_RSRP);

			input = qmi_message_nas_get_signal_strength_input_new();
			if (!qmi_message_nas_get_signal_strength_input_set_request_mask(
			       input, mask, &qmi_error)) {
				error("couldn't create input data bundle: ",
				      (char const *)qmi_error->message);
				g_error_free(qmi_error);
				qmi_message_nas_get_signal_strength_input_unref(input);
				input = nullptr;
			}

			return input;
		}

		static void _signal_strength_ready(QmiClientNas *client,
                                       GAsyncResult *res,
                                       gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);
			GError *qmi_error = nullptr;

			QmiMessageNasGetSignalStrengthOutput *output;
			QmiNasRadioInterface radio_interface;
			GArray *array;

			output = qmi_client_nas_get_signal_strength_finish(client, res,
			                                                   &qmi_error);
			if (!output) {
				error("operation failed: ", (char const*)qmi_error->message);
				g_error_free(qmi_error);
				return;
			}

			if (!qmi_message_nas_get_signal_strength_output_get_result(output,
			                                                           &qmi_error)) {
				error("couldn't get signal strength: ",
				      (char const*)qmi_error->message);
				g_error_free(qmi_error);
				qmi_message_nas_get_signal_strength_output_unref(output);
				return;
			}

			/* RSSI */
			if (qmi_message_nas_get_signal_strength_output_get_rssi_list(output,
			                                                             &array,
			                                                             nullptr)) {
				for (guint i = 0; i < array->len; i++) {
					QmiMessageNasGetSignalStrengthOutputRssiListElement *element;

					element = &g_array_index(array, QmiMessageNasGetSignalStrengthOutputRssiListElement, i);
					/* search only for LTE */
					if (element->radio_interface == QMI_NAS_RADIO_INTERFACE_LTE) {
						qmi->_state_report.rssi = (-1l) * element->rssi;
						break;
					}
				}
			}

			/* RSRQ */
			gint8 rsrq;
			if (qmi_message_nas_get_signal_strength_output_get_rsrq(output, &rsrq,
			                                                        &radio_interface,
			                                                        nullptr))
				qmi->_state_report.rsrq = rsrq;

			/* LTE SNR */
			gint16 snr;
			if (qmi_message_nas_get_signal_strength_output_get_lte_snr(output, &snr,
			                                                           nullptr))
				qmi->_state_report.rssnr = 0.1f * snr;

			/* LTE RSRP */
			gint16 rsrp;
			if (qmi_message_nas_get_signal_strength_output_get_lte_rsrp(output, &rsrp,
			                                                            nullptr))
				qmi->_state_report.rsrp = rsrp;

			/* skip others for now */
			qmi_message_nas_get_signal_strength_output_unref(output);

			/* retrieve network information */
			qmi_client_nas_get_serving_system(client,
			                                  nullptr,
			                                  10,
			                                  nullptr,
			                                  (GAsyncReadyCallback)_serving_system_ready,
			                                  qmi);
		}

		static gboolean _handle_status_update(gpointer user_data)
		{
			Qmi *qmi = _qmi(user_data);

			if (qmi->_state != READY) return FALSE;

			QmiMessageNasGetSignalStrengthInput *input = nullptr;
			input = qmi->_signal_strength_input_create();

			qmi_client_nas_get_signal_strength(QMI_CLIENT_NAS(qmi->_nas_client),
			                                   input,
			                                   10,
			                                   nullptr,
			                                   (GAsyncReadyCallback)_signal_strength_ready,
			                                   qmi);
			qmi_message_nas_get_signal_strength_input_unref(input);

			return TRUE;
		}


		/*
		 * init/main loop
		 */

		void _init()
		{
			if (TRACE) {
				g_log_set_handler(nullptr, G_LOG_LEVEL_MASK, _log_handler, nullptr);
				g_log_set_handler("Qmi", G_LOG_LEVEL_MASK, _log_handler, nullptr);
			}

			qmi_utils_set_traces_enabled(TRACE);
			qmi_utils_set_show_personal_info(TRACE);

			_file = g_file_new_for_commandline_arg("/dev/cdc-wdm0");
			_loop = g_main_loop_new(nullptr, FALSE);
			qmi_device_new(_file, nullptr, (GAsyncReadyCallback)_device_new_ready,
			               this);
		}

		void _run_loop()
		{
			g_main_loop_run(_loop);
			g_main_loop_unref(_loop);
			g_object_unref(_file);
		}

	public:

		Qmi(Libc::Env &env) : _env(env)
		{
			_config_rom.node().with_sub_node("network",
				[&] (Node const &net) {
					_network.apn      = net.attribute_value("apn", String());
					_network.user     = net.attribute_value("user", String());
					_network.password = net.attribute_value("password", String());
					_network.pin      = net.attribute_value("pin", String());
				}, [] {
					error("No valid <network> configuration found");
					exit(1);
				});

			_init();
			_run_loop();
		}

		Qmi(Qmi const &) = delete;
		Qmi & operator = (Qmi const &) = delete;

		void send_request()
		{
			switch (_state) {

			case NONE:

				if (!_uim_client)  {
					/* allocate uim client for pin-state check */
					_current_client_type = UIM;
					qmi_device_allocate_client(_device,
					                           QMI_SERVICE_UIM,
					                           QMI_CID_NONE,
					                           10,
					                           nullptr,
					                           (GAsyncReadyCallback)_allocate_client_ready,
					                           this);
					break;
				}

				/* retrieve pin state */
				qmi_client_uim_get_card_status(QMI_CLIENT_UIM(_uim_client),
				                               nullptr,
				                               10,
				                               nullptr,
				                               (GAsyncReadyCallback)_card_status_ready,
				                               this);
				break;

			case UNLOCK: {

				QmiMessageUimVerifyPinInput *input = _pin_create_input();
				qmi_client_uim_verify_pin(QMI_CLIENT_UIM(_uim_client),
				                          input,
				                          10,
				                          nullptr,
				                          (GAsyncReadyCallback)_verify_pin_ready,
				                          this);

				qmi_message_uim_verify_pin_input_unref(input);
				break;
			}
			case PIN:

				if (!_wds_client) {
					_current_client_type = WDS;
					qmi_device_allocate_client(_device,
					                           QMI_SERVICE_WDS,
					                           QMI_CID_NONE,
					                           10,
					                           nullptr,
					                           (GAsyncReadyCallback)_allocate_client_ready,
					                           this);
					break;
				}
				{
					QmiMessageWdsStartNetworkInput *input = _network_input_create();

					qmi_client_wds_start_network(QMI_CLIENT_WDS(_wds_client),
					                             input,
					                             180,
					                             nullptr,
					                             (GAsyncReadyCallback)_start_network_ready,
					                             this);
					qmi_message_wds_start_network_input_unref (input);
				}
				break;

			case CONNECT: {

				QmiMessageWdsGetCurrentSettingsInput *input = nullptr;

				input = qmi_message_wds_get_current_settings_input_new();

				qmi_message_wds_get_current_settings_input_set_requested_settings(
				  input,
				  QmiWdsRequestedSettings(QMI_WDS_REQUESTED_SETTINGS_DNS_ADDRESS |
				                          QMI_WDS_REQUESTED_SETTINGS_IP_ADDRESS  |
				                          QMI_WDS_REQUESTED_SETTINGS_GATEWAY_INFO),
				  nullptr);

				qmi_client_wds_get_current_settings(QMI_CLIENT_WDS(_wds_client),
				                                    input,
				                                    10,
				                                    nullptr,
				                                    (GAsyncReadyCallback)_current_settings_ready,
				                                    this);

				qmi_message_wds_get_current_settings_input_unref(input);

				break;
			}

			case READY:

				if (!_nas_client) {
					_current_client_type = NAS;
					qmi_device_allocate_client(_device,
					                           QMI_SERVICE_NAS,
					                           QMI_CID_NONE,
					                           10,
					                           nullptr,
					                           (GAsyncReadyCallback)_allocate_client_ready,
					                           this);
					break;
				}
				g_timeout_add(STATUS_INTERVAL, _handle_status_update, this);
				break;
			}
		}

		void shutdown()
		{

			QmiDeviceReleaseClientFlags flags = QMI_DEVICE_RELEASE_CLIENT_FLAGS_NONE;
			//flags |= QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID;
			if (_uim_client) {
				_current_client_type = UIM;
				qmi_device_release_client(_device,
				                          _uim_client,
				                          flags, 10, nullptr,
				                          (GAsyncReadyCallback)_release_client_ready,
				                          this);
				return;
			}

			if (_wds_client) {
				_current_client_type = WDS;
				qmi_device_release_client(_device,
				                          _wds_client,
				                          flags, 10, nullptr,
				                          (GAsyncReadyCallback)_release_client_ready,
				                          this);
				return;
			}

			if (_nas_client) {
				_current_client_type = NAS;
				qmi_device_release_client(_device,
				                          _nas_client,
				                          flags, 10, nullptr,
				                          (GAsyncReadyCallback)_release_client_ready,
				                          this);
				return;
			}

			_current_client_type = UNKNOWN;

			if (_device) {
				qmi_device_close_async(_device, 10, nullptr,
				                       (GAsyncReadyCallback) _device_close_ready, this);
				return;
			}

			g_main_loop_quit(_loop);
		}

		void report_config()
		{
			if (_state != READY)
				return;

			_broadband_config.report(_connection);
		}

		void report_state()
		{
			_broadband_state.report(_state_report);
		}

		State_report &state_report() { return _state_report; }
};


void Libc::Component::construct(Libc::Env &env)
{
	Libc::with_libc([&] () {
		static Qmi main { env };
	});

	exit(0);
}
