/*
 * \brief  Audio handling
 * \author Alexander Boettcher
 * \date   2022-06-19
 */

/*
 * Copyright (C) 2022 Genode Labs GmbH
 *
 * This file is distributed under the terms of the GNU General Public License
 * version 2.
 *
 * The code is partially based on the Seoul VMM, which is distributed
 * under the terms of the GNU General Public License version 2.
 */


#include "audio.h"


Seoul::Audio::Audio(Genode::Env &env, Synced_motherboard &motherboard,
                    Motherboard &unsynch_motherboard)
:
	_sigh_processed(env.ep(), *this, &Audio::_audio_out),
	_motherboard(motherboard),
	_unsynchronized_motherboard(unsynch_motherboard),
	_left (env, "left" ),
	_right(env, "right"),
	_timer(env)
{
	_timer.sigh(_sigh_processed);
//	_timer.trigger_periodic(_period_us);
}


void Seoul::Audio::_audio_out()
{
	Genode::Mutex::Guard guard(_mutex);

	if (sample_offset < max_samples) {
		Genode::log(_data_id, " ", sample_offset, "/", max_samples);
		return;
	}

#if 0
	_time_window = _left.schedule_and_enqueue(_time_window, { _period_us }, [&] (auto &submit) {
		for (auto const data : _left_data)  { submit(data); }
	});

	_right.enqueue(_time_window, [&] (auto &submit) {
		for (auto const data : _right_data) { submit(data); }
	});
#endif

	MessageAudio msg(MessageAudio::Type::AUDIO_CONTINUE_TX, _data_id);

	sample_offset = 0;
	_data_id ++;

	_mutex.release();

	_motherboard()->bus_audio.send(msg);

	_mutex.acquire();
}

bool Seoul::Audio::receive(MessageAudio &msg)
{
	if ((msg.type == MessageAudio::Type::AUDIO_CONTINUE_TX) ||
	    (msg.type == MessageAudio::Type::AUDIO_DRAIN_TX))
		return false;

	Genode::Mutex::Guard guard(_mutex);

	switch (msg.type) {
	case MessageAudio::Type::AUDIO_START:
		return true;
	case MessageAudio::Type::AUDIO_STOP:
		_audio_stop();
		return true;
	case MessageAudio::Type::AUDIO_OUT:
		break;
	default:
		Genode::warning("audio: unsupported type ", unsigned(msg.type));
		return true;
	}

	/* case MessageAudio::Type::AUDIO_OUT */

	if (sample_offset >= max_samples) {
		msg.id = _data_id;
		return true;
	}

	_audio_start();

	if (msg.consumed >= msg.size)
		Logging::panic("audio corrupt consumed=%u msg.size=%u entry\n",
		               msg.consumed, msg.size);

	{

		unsigned const guest_sample_size = sizeof(float);

		if (msg.consumed >= msg.size)
			Logging::panic("audio corrupt consumed=%u msg.size=%u loop\n",
			               msg.consumed, msg.size);

		uintptr_t const data_ptr = msg.data + msg.consumed;

		unsigned samples = (((msg.size - msg.consumed) / guest_sample_size) > max_samples)
		                 ? max_samples : ((msg.size - msg.consumed) / guest_sample_size);

		if (samples > max_samples - sample_offset)
			samples = max_samples - sample_offset;

		if (msg.consumed >= msg.size)
			Logging::panic("audio corrupt consumed=%u msg.size=%u\n",
			               msg.consumed, msg.size);

		for (unsigned i = 0; i < samples / CHANNELS; i++) {
			unsigned const sample_pos = sample_offset / CHANNELS + i;

			float f_left  = ((float *)data_ptr)[i*CHANNELS+0];
			float f_right = ((float *)data_ptr)[i*CHANNELS+1];


			if (f_left >  1.0f) f_left =  1.0f;
			if (f_left < -1.0f) f_left = -1.0f;

			if (f_right >  1.0f) f_right =  1.0f;
			if (f_right < -1.0f) f_right = -1.0f;

			_left_data [sample_pos] = f_left;
			_right_data[sample_pos] = f_right;
		}

		msg.consumed += samples * guest_sample_size;
		msg.id        = _data_id;

		sample_offset += samples;

		if (sample_offset >= max_samples) {
			_time_window = _left.schedule_and_enqueue(_time_window, { _period_us }, [&] (auto &submit) {
				for (auto const data : _left_data)  { submit(data); }
			});

			_right.enqueue(_time_window, [&] (auto &submit) {
				for (auto const data : _right_data) { submit(data); }
			});

			_timer.trigger_once(_period_us);
		}
	}

	return true;
}
