/*
 * \brief  Tool for checking input events
 * \author Emery Hemingway
 * \date   2018-10-09
 */

/*
 * Copyright (C) 2018 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <gui_session/connection.h>
#include <framebuffer_session/connection.h>
#include <base/component.h>
#include <base/attached_rom_dataspace.h>
#include <base/log.h>
#include <base/heap.h>
#include <os/pixel_rgb888.h>
#include <os/surface.h>
#include <nitpicker_gfx/tff_font.h>
#include <nitpicker_gfx/box_painter.h>

/* gems includes */
#include <gems/vfs_font.h>


namespace Show_input {
	using namespace Genode;
	typedef Surface_base::Point Point;
	typedef Surface_base::Area  Area;
	typedef Surface_base::Rect  Rect;

	struct Main;
};


struct Show_input::Main
{
	Genode::Env &_env;

	Heap _heap { _env.ram(), _env.rm() };

	Attached_rom_dataspace _config { _env, "config" };

	Root_directory _root { _env, _heap, _config.xml().sub_node("vfs") };

	Vfs_font _font { _heap, _root, "fonts/text/regular" };

	Gui::Connection _gui { _env };

	Input::Session_client &_input = *_gui.input();
	Framebuffer::Session  &_fb    = *_gui.framebuffer();

	Dataspace_capability _fb_ds_cap()
	{
		_gui.buffer(_gui.mode(), false);
		return _fb.dataspace();
	}

	Attached_dataspace _fb_ds { _env.rm(), _fb_ds_cap() };

	Gui::Session::View_handle _view = _gui.create_view();

	typedef Pixel_rgb888 PT;

	Surface_base::Area _size { _fb.mode().area.w,
	                           (unsigned)_font.height() };

	Surface<PT> _surface { _fb_ds.local_addr<PT>(), _size };

	void _refresh() { _fb.refresh(0, 0, _size.w, _size.h); }

	Signal_handler<Main> _input_sigh {
		_env.ep(), *this, &Main::_handle_input };

	void _handle_input()
	{
		using namespace Input;
		bool refresh = false;

		_input.for_each_event([&] (Input::Event const &ev) {
			ev.handle_press([&] (Keycode key, Codepoint codepoint) {
				String<128> info(
					key_name(key), " ",
					codepoint, " "
					"(", codepoint.value, ")");

				Box_painter::paint(
					_surface,
					Rect(Point(), _size),
					Color::black());


				Text_painter::paint(
					_surface,
					Text_painter::Position(0, 0),
					_font,
					Color::rgb(255, 255, 255),
					info.string());
				refresh = true;

				auto w = _font.string_width(info.string()).decimal();
				auto h = _font.height();

				_gui.enqueue<Gui::Session::Command::Geometry>(
					_view, Rect(Point(0, 0), Area(w*2, h*2)));
			});
		});

		if (refresh) {
			_gui.execute();
			_refresh();
		}
	}

	Main(Env &env) : _env(env)
	{
		_input.sigh(_input_sigh);

		_gui.enqueue<Gui::Session::Command::Geometry>(
			_view, Rect(Point(0, 0), _size));

		_gui.enqueue<Gui::Session::Command::To_front>(
			_view, Gui::Session::View_handle());
		_gui.execute();

		_surface.clip(Rect(Point(0, 0), _size));
	}
};


void Component::construct(Genode::Env &env)
{
	env.exec_static_constructors();
	static Show_input::Main inst(env);
}

/*
 * Resolve symbol required by libc. It is unused as we implement
 * 'Component::construct' directly instead of initializing the libc.
 */

#include <libc/component.h>

void Libc::Component::construct(Libc::Env &) { }

