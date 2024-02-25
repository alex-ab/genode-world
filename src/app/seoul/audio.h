/*
 * \brief  Audio handling
 * \author Alexander Boettcher
 * \date   2022-06-19
 */

/*
 * Copyright (C) 2022-2024 Genode Labs GmbH
 *
 * This file is distributed under the terms of the GNU General Public License
 * version 2.
 *
 * The code is partially based on the Seoul VMM, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _AUDIO_H_
#define _AUDIO_H_

#include <play_session/connection.h>
#include <timer_session/connection.h>

#include "synced_motherboard.h"

namespace Seoul {
	class Audio;
}

class Seoul::Audio
{
	private:

		Genode::Signal_handler<Audio> const _sigh_processed;

		Genode::Mutex         _mutex { };

		Synced_motherboard   &_motherboard;
		Motherboard          &_unsynchronized_motherboard;


		static unsigned const _period_us         = 11'600u,
		                      _sample_rate_hz    = 44'100u,
		                      _frames_per_period = (_period_us * _sample_rate_hz)
		                                         / 1'000'000;

/*
		static unsigned const _sample_rate_hz    = 44'100u,
		                      _frames_per_period = 512, //1102,
		                      _period_us         = _frames_per_period * 1'000'0000
		                                         / _sample_rate_hz;
*/

		Play::Connection  _left;
		Play::Connection  _right;
		Play::Time_window _time_window { };

		Timer::Connection _timer;

		enum { CHANNELS = 2 };

		unsigned const max_samples = _frames_per_period * CHANNELS;
		unsigned       sample_offset { 0 };

		unsigned _data_id = 1;

		float _left_data [_frames_per_period] { };
		float _right_data[_frames_per_period] { };

		bool _audio_running { false };
		bool _audio_verbose { false };

		void _audio_out();

		void _audio_start()
		{
			if (_audio_running)
				return;

			if (_audio_verbose)
				Genode::log(__func__);

			_audio_running = true;
		}

		void _audio_stop()
		{
			if (!_audio_running)
				return;

			if (_audio_verbose)
				Genode::log(__func__);

			_audio_running = false;

			sample_offset = 0;
		}

		/*
		 * Noncopyable
		 */
		Audio(Audio const &);
		Audio &operator = (Audio const &);

	public:

		Audio(Genode::Env &, Synced_motherboard &, Motherboard &);

		bool receive(MessageAudio &);

		void verbose(bool onoff) { _audio_verbose = onoff; }
};

#endif /* _AUDIO_H_ */
