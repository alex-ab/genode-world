/*
 * \brief  Manager of all VM requested console functionality
 * \author Markus Partheymueller
 * \author Norman Feske
 * \author Alexander Boettcher
 * \date   2012-07-31
 */

/*
 * Copyright (C) 2011-2017 Genode Labs GmbH
 * Copyright (C) 2012 Intel Corporation
 *
 * This file is distributed under the terms of the GNU General Public License
 * version 2.
 *
 * The code is partially based on the Vancouver VMM, which is distributed
 * under the terms of the GNU General Public License version 2.
 *
 * Modifications by Intel Corporation are contributed under the terms and
 * conditions of the GNU General Public License version 2.
 */

/* base includes */
#include <util/register.h>

/* nitpicker graphics backend */
#include <nitpicker_gfx/tff_font.h>

#include <nul/motherboard.h>
#include <host/screen.h>

/* local includes */
#include "console.h"

extern char _binary_mono_tff_start[];

static Tff_font::Static_glyph_buffer<4096> glyph_buffer { };
static Tff_font default_font(_binary_mono_tff_start, glyph_buffer);

static struct {
	Genode::uint64_t checksum1   = 0;
	Genode::uint64_t checksum2   = 0;
	unsigned         unchanged   = 0;
	bool             cmp_even    = 1;
	bool             active      = false;
	bool             vga_off     = false;
} fb_state;


/**
 * Layout of PS/2 mouse packet
 */
struct Ps2_mouse_packet : Genode::Register<32>
{
	struct Packet_size   : Bitfield<0, 3> { };
	struct Left_button   : Bitfield<8, 1> { };
	struct Middle_button : Bitfield<9, 1> { };
	struct Right_button  : Bitfield<10, 1> { };
	struct Rx_high       : Bitfield<12, 1> { };
	struct Ry_high       : Bitfield<13, 1> { };
	struct Rx_low        : Bitfield<16, 8> { };
	struct Ry_low        : Bitfield<24, 8> { };
};


static bool mouse_event(Input::Event const &ev)
{
	using namespace Input;

	bool result = false;

	auto mouse_button = [] (Keycode key) {
		return key == BTN_LEFT || key == BTN_MIDDLE || key == BTN_RIGHT; };

	ev.handle_press([&] (Keycode key, Genode::Codepoint) {
		result |= mouse_button(key); });

	ev.handle_release([&] (Keycode key) {
		result |= mouse_button(key); });

	result |= ev.absolute_motion() || ev.relative_motion();
	result |= ev.wheel();

	return result;
}


/**
 * Convert Genode::Input event to PS/2 packet
 *
 * This function updates _left, _middle, and _right as a side effect.
 */
unsigned Seoul::Console::_input_to_ps2mouse(Input::Event const &ev)
{
	/* track state of mouse buttons */
	auto apply_button = [] (Input::Event const &ev, Input::Keycode key, bool &state) {
		if (ev.key_press  (key)) state = true;
		if (ev.key_release(key)) state = false;
	};

	apply_button(ev, Input::BTN_LEFT,   _left);
	apply_button(ev, Input::BTN_MIDDLE, _middle);
	apply_button(ev, Input::BTN_RIGHT,  _right);

	int rx = 0;
	int ry = 0;

	ev.handle_absolute_motion([&] (int x, int y) {
		static int ox = 0, oy = 0;
		rx = x - ox; ry = y - oy;
		ox = x; oy = y;
	});

	ev.handle_relative_motion([&] (int x, int y) { rx = x; ry = y; });


	/* clamp relative motion vector to bounds */
	int const boundary = 200;
	rx =  Genode::min(boundary, Genode::max(-boundary, rx));
	ry = -Genode::min(boundary, Genode::max(-boundary, ry));

	/* assemble PS/2 packet */
	Ps2_mouse_packet::access_t packet = 0;
	Ps2_mouse_packet::Packet_size::set  (packet, 3);
	Ps2_mouse_packet::Left_button::set  (packet, _left);
	Ps2_mouse_packet::Middle_button::set(packet, _middle);
	Ps2_mouse_packet::Right_button::set (packet, _right);
	Ps2_mouse_packet::Rx_high::set      (packet, (rx >> 8) & 1);
	Ps2_mouse_packet::Ry_high::set      (packet, (ry >> 8) & 1);
	Ps2_mouse_packet::Rx_low::set       (packet, rx & 0xff);
	Ps2_mouse_packet::Ry_low::set       (packet, ry & 0xff);

	return packet;
}


void Seoul::Console::_input_to_ps2(Input::Event const &ev)
{
	/* if absolute pointing model is avail, don't use relative PS2 */
	if (_absolute)
		return;

	/* update PS2 mouse model */
	MessageInput msg(0x10001, _input_to_ps2mouse(ev), _input_to_ps2wheel(ev));
	if (_motherboard()->bus_input.send(msg) && !_relative)
		_relative = true;
}


void Seoul::Console::_input_to_virtio(Input::Event const &ev)
{
	auto button = [] (Input::Event const &ev, Input::Keycode key, bool &state) {
		if (ev.key_press  (key)) {
			state = true;
			return true;
		}
		if (ev.key_release(key)) {
			state = false;
			return true;
		}
		return false;
	};

	bool left = false, middle = false, right = false;
	unsigned data = 0, data2 = 0;

	if (button(ev, Input::BTN_LEFT, left)) {
		data  = 1u << 31;
		data2 = !!left;
	} else
	if (button(ev, Input::BTN_MIDDLE, middle)) {
		data  = 1u << 30;
		data2 = !!middle;
	} else
	if (button(ev, Input::BTN_RIGHT, right)) {
		data  = 1u << 29;
		data2 = !!right;
	} else {
		ev.handle_absolute_motion([&] (int x, int y) {
			unsigned const mask = (0xfu << 28) - 1;
			MessageInput msg(0x10002, x & mask, y & mask);
			if (_motherboard()->bus_input.send(msg) && !_absolute)
				_absolute = true;
		});
		ev.handle_wheel([&](int, int z) {
			MessageInput msg(0x10002, 1u << 28, z);
			_motherboard()->bus_input.send(msg);
		});

		return;
	}

	MessageInput msg(0x10002, data, data2);
	if (_motherboard()->bus_input.send(msg) && !_absolute)
		_absolute = true;
}


enum {
	PHYS_FRAME_VGA       = 0xa0,
	PHYS_FRAME_VGA_COLOR = 0xb8,
	FRAME_COUNT_COLOR    = 0x8
};

unsigned Seoul::Console::_input_to_ps2wheel(Input::Event const &ev)
{
	int rz = 0;
	ev.handle_wheel([&](int x, int y) { rz = y; });

	return (unsigned)rz;
}


/* bus callbacks */

bool Seoul::Console::receive(MessageConsole &msg)
{
	switch (msg.type) {
	case MessageConsole::TYPE_ALLOC_CLIENT:
	{
		Genode::String<12> name("fb", msg.id, ".", msg.view);
		auto *gui = new (_alloc) Backend_gui(_env, _guis, msg.id, _fb_mode.area,
		                                     _signal_input, name.string());
		msg.ptr = _env.rm().attach(gui->fb_ds);
		return true;
	}
	case MessageConsole::TYPE_ALLOC_VIEW :
	{
		if (msg.id == ID_VGA_VESA) { /* XXX special handling */
			_guest_fb = msg.ptr;
			_regs = msg.regs;

			msg.view = 0;
			return true;
		}
		bool const found = apply_msg(msg.id, [&](auto &gui) {
			if (msg.view == 1) {
				msg.ptr = gui.shape_ptr();
				msg.size = gui.shape_size();
				return true;
			}
			if (msg.view > 1 && msg.size) {
				Genode::Ram_dataspace_capability ds { };
				try {
					msg.ptr  = nullptr;

					ds = _env.ram().alloc(msg.size);
					msg.ptr = _env.rm().attach(ds);

					new (_alloc) Backend_gui::Pixel_buffer(gui.pixel_buffers, Backend_gui::Pixel_buffer::Id { msg.view }, ds);

					return true;
				} catch (...) {
					Genode::error("insufficient resources to create buffer");
					if (ds.valid())
						_env.ram().free(ds);
					return false;
				}
			}
			Genode::error("xxxx");
			return false;
		});
		return found;
	}
	case MessageConsole::TYPE_FREE_VIEW:
		if (msg.id == ID_VGA_VESA) { /* XXX special handling */
			return false; }

		return apply_msg(msg.id, [&](auto &gui) {

			Backend_gui::Pixel_buffer::Id const id { msg.view };
			gui.free_buffer(id, [&](Backend_gui::Pixel_buffer &buffer) {
				_env.ram().free(buffer.ds);
				Genode::destroy(_alloc, &buffer);
			});

			return true;
		});
	case MessageConsole::TYPE_SWITCH_VIEW:
		/* XXX: For now, we only have one view. */
		return true;
	case MessageConsole::TYPE_GET_MODEINFO:
		enum {
			MEMORY_MODEL_TEXT = 0,
			MEMORY_MODEL_DIRECT_COLOR = 6,
		};

		/*
		 * We supply two modes to the guest, text mode and one
		 * configured graphics mode 16-bit.
		 */
		if (msg.index == 0) {
			msg.info->_vesa_mode = 3;
			msg.info->attr = 0x1;
			msg.info->resolution[0] = 80;
			msg.info->resolution[1] = 25;
			msg.info->bytes_per_scanline = 80*2;
			msg.info->bytes_scanline = 80*2;
			msg.info->bpp = 4;
			msg.info->memory_model = MEMORY_MODEL_TEXT;
			msg.info->phys_base = PHYS_FRAME_VGA_COLOR << 12;
			msg.info->_phys_size = FRAME_COUNT_COLOR << 12;
			return true;

		} else if (msg.index == 1) {

			/*
			 * It's important to set the _vesa_mode field, otherwise the
			 * device model is going to ignore this mode.
			 */
			unsigned const bpp = _fb_mode.bytes_per_pixel();
			msg.info->_vesa_mode = 0x138;
			msg.info->attr = 0x39f;
			msg.info->resolution[0]      = _fb_mode.area.w();
			msg.info->resolution[1]      = _fb_mode.area.h();
			msg.info->bytes_per_scanline = _fb_mode.area.w()*bpp;
			msg.info->bytes_scanline     = _fb_mode.area.w()*bpp;
			msg.info->bpp = 32;
			msg.info->memory_model = MEMORY_MODEL_DIRECT_COLOR;
			msg.info->vbe1[0] =  0x8; /* red mask size */
			msg.info->vbe1[1] = 0x10; /* red field position */
			msg.info->vbe1[2] =  0x8; /* green mask size */
			msg.info->vbe1[3] =  0x8; /* green field position */
			msg.info->vbe1[4] =  0x8; /* blue mask size */
			msg.info->vbe1[5] =  0x0; /* blue field position */
			msg.info->vbe1[6] =  0x0; /* reserved mask size */
			msg.info->vbe1[7] =  0x0; /* reserved field position */
			msg.info->colormode = 0x0; /* direct color mode info */
			msg.info->phys_base = 0xe0000000;
			msg.info->_phys_size = _fb_mode.area.count()*bpp;
			return true;
		}
		return false;
	case MessageConsole::TYPE_PAUSE: /* all CPUs go idle */
		_handle_fb(fb_state.vga_off); /* refresh before going to sleep */
		fb_state.active = false;
		return true;
	case MessageConsole::TYPE_RESUME: /* first of all sleeping CPUs woke up */
		_reactivate();
		return true;
	case MessageConsole::TYPE_CONTENT_UPDATE:
	{
		bool const found = apply_msg(msg.id, [&](auto &gui) {
			if (msg.view == 0)
				gui.refresh(msg.x, msg.y, msg.width, msg.height);
			else if (msg.view == 1) {
				int x = 0, y = 0;

				/* makes mouse (somewhat) visible if solely relative input is used */
				if (_relative && !_absolute) {
					x = gui.last_host_pos.x() - msg.x;
					y = gui.last_host_pos.y() - msg.y;
				}

				gui.mouse_shape(true /* XXX */,
				                x, y,
				                msg.width, msg.height);
			} else
				return false;
			return true;
		});

		/* if the fb updating was off, reactivate timer - move into vga/vesa class XXX */
		if (msg.id == ID_VGA_VESA && !fb_state.active)
			_reactivate();

		if (!found)
			Genode::error("unknown graphical backend ", msg.id);

		return true;
	}
	default:
		return true;
	}
}

void Seoul::Console::_reactivate()
{
	fb_state.active = true;

	MessageTimer msg(_timer, _unsynchronized_motherboard.clock()->abstime(1, 1000));
	_unsynchronized_motherboard.bus_timer.send(msg);
}

bool Seoul::Console::receive(MessageMemRegion &msg)
{
	/* we had a fault in the text framebuffer */
	bool reactivate = (msg.page >= PHYS_FRAME_VGA &&
	                   msg.page < PHYS_FRAME_VGA_COLOR + FRAME_COUNT_COLOR);

	if (reactivate) {
		fb_state.vga_off = false;

		//Logging::printf("Reactivating text buffer loop.\n");

		_reactivate();
	}
	return false;
}


unsigned Seoul::Console::_handle_fb(bool const skip_update)
{
	if (!_guest_fb || !_regs)
		return 0;

	enum { TEXT_MODE = 0 };

	/* transfer text buffer content into chunky canvas */
	if (_regs->mode == TEXT_MODE) {

		if (skip_update || !fb_state.active) return 0;

		memset(_pixels, 0, _fb_size);

		if (fb_state.cmp_even) fb_state.checksum1 = 0;
		else fb_state.checksum2 = 0;

		Genode::Surface<Pixel_rgb888> _surface(reinterpret_cast<Genode::Pixel_rgb888 *>(_pixels), _fb_mode.area);

		for (int j=0; j<25; j++) {
			for (int i=0; i<80; i++) {
				Text_painter::Position const where(i*8, j*15);
				char character = *((char *) (_guest_fb +(_regs->offset << 1) +j*80*2+i*2));
				char colorvalue = *((char *) (_guest_fb+(_regs->offset << 1)+j*80*2+i*2+1));
				char buffer[2]; buffer[0] = character; buffer[1] = 0;
				char fg = colorvalue & 0xf;
				if (fg == 0x8) fg = 0x7;
				unsigned lum = ((fg & 0x8) >> 3)*127;
				Genode::Color color(((fg & 0x4) >> 2)*127+lum, /* R+luminosity */
				                    ((fg & 0x2) >> 1)*127+lum, /* G+luminosity */
				                     (fg & 0x1)*127+lum        /* B+luminosity */);

				Text_painter::paint(_surface, where, default_font, color, buffer);

				/* Checksum for comparing */
				if (fb_state.cmp_even) fb_state.checksum1 += character;
				else fb_state.checksum2 += character;
			}
		}

		fb_state.cmp_even = !fb_state.cmp_even;

		/* compare checksums to detect changed buffer */
		if (fb_state.checksum1 != fb_state.checksum2) {
			fb_state.unchanged = 0;
			_backend_gui.refresh(0, 0, _fb_mode.area.w(), _fb_mode.area.h());
			return 100;
		}

		if (++fb_state.unchanged < 10)
			return fb_state.unchanged * 30;

		/* if we copy the same data 10 times, unmap the text buffer from guest */
		_memory.detach(PHYS_FRAME_VGA_COLOR << 12,
		               FRAME_COUNT_COLOR << 12);

		fb_state.vga_off = true;
		fb_state.unchanged = 0;

		_backend_gui.refresh(0, 0, _fb_mode.area.w(), _fb_mode.area.h());

		return 0;
	}

	if (!fb_state.vga_off) {
		_memory.detach(PHYS_FRAME_VGA_COLOR << 12,
		               FRAME_COUNT_COLOR << 12);

		fb_state.vga_off = true;
	}

	if (!fb_state.active) {
		fb_state.unchanged = 0;
		return 0;
	}

	_backend_gui.refresh(0, 0, _fb_mode.area.w(), _fb_mode.area.h());

	fb_state.unchanged++;

	return (fb_state.unchanged > 4) ? 4 * 10 : fb_state.unchanged * 10;
}


void Seoul::Console::_handle_input()
{
	for (auto *gui = _guis.first(); gui; gui = gui->next()) {
		gui->input.for_each_event([&] (Input::Event const &ev) {

			if (!fb_state.active) {
				fb_state.active = true;
				MessageTimer msg(_timer, _motherboard()->clock()->abstime(1, 1000));
				_motherboard()->bus_timer.send(msg);
			}

			if (mouse_event(ev)) {
				ev.handle_absolute_motion([&] (int x, int y) {
					gui->last_host_pos = Genode::Point<unsigned>(x, y);
				});

				/* update PS2 mouse model */
				_input_to_ps2(ev);

				/* update virtio input model */
				_input_to_virtio(ev);
			}

			ev.handle_press([&] (Input::Keycode key, Genode::Codepoint) {
				if (key <= 0xee)
					_vkeyb.handle_keycode_press(key); });

			ev.handle_release([&] (Input::Keycode key) {
				if (key <= 0xee)
					_vkeyb.handle_keycode_release(key); });
		});
	}
}


void Seoul::Console::register_host_operations(Motherboard &motherboard)
{
	motherboard.bus_console  .add(this, receive_static<MessageConsole>);
	motherboard.bus_memregion.add(this, receive_static<MessageMemRegion>);
	motherboard.bus_timeout  .add(this, receive_static<MessageTimeout>);

	MessageTimer msg;
	if (!motherboard.bus_timer.send(msg))
		Logging::panic("%s can't get a timer", __PRETTY_FUNCTION__);

	_timer = msg.nr;
}


bool Seoul::Console::receive(MessageTimeout &msg) {
	if (msg.nr != _timer)
		return false;

	unsigned next_timeout_ms = _handle_fb(fb_state.vga_off);

	if (next_timeout_ms) {
		MessageTimer msg_t(_timer, _unsynchronized_motherboard.clock()->abstime(next_timeout_ms, 1000));
		_unsynchronized_motherboard.bus_timer.send(msg_t);
	}

	return true;
}


Seoul::Console::Console(Genode::Env &env, Genode::Allocator &alloc,
                        Synced_motherboard &mb,
                        Motherboard &unsynchronized_motherboard,
                        Gui::Area const area,
                        Seoul::Guest_memory &guest_memory)
:
	_env(env),
	_unsynchronized_motherboard(unsynchronized_motherboard),
	_motherboard(mb),
	_alloc(alloc),
	_backend_gui(*new Backend_gui(_env, _guis, ID_VGA_VESA, area, _signal_input, "vga/vesa")),
	_memory(guest_memory),
	_fb_mode(_backend_gui.gui.framebuffer()->mode()),
	_fb_size(Genode::Dataspace_client(_backend_gui.fb_ds).size()),
	_fb_vm_ds(env.ram().alloc(_fb_size)),
	_fb_vm_mapping(_env.rm().attach(_fb_vm_ds)),
	_vm_phys_fb(guest_memory.alloc_io_memory(_fb_size)),
	_pixels(_env.rm().attach(_backend_gui.fb_ds))
{
	guest_memory.add_region(alloc, PHYS_FRAME_VGA << 12,
	                        _fb_vm_mapping, _fb_vm_ds, _fb_size);
	guest_memory.add_region(alloc, _vm_phys_fb,
	                        Genode::addr_t(_pixels), _backend_gui.fb_ds, _fb_size);
}
