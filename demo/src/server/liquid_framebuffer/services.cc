/*
 * \brief  Implementation of Framebuffer and Input services
 * \author Norman Feske
 * \date   2006-09-22
 */

/*
 * Copyright (C) 2006-2013 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#include <base/env.h>
#include <base/semaphore.h>
#include <base/rpc_server.h>
#include <framebuffer_session/framebuffer_session.h>
#include <input/component.h>
#include <nitpicker_gfx/texture_painter.h>
#include <os/pixel_rgb565.h>

#include "services.h"


typedef Genode::Texture<Genode::Pixel_rgb565> Texture_rgb565;


/*****************
 ** Event queue **
 *****************/

class Event_queue
{
	private:

		enum { QUEUE_SIZE = 1024 };

		Input::Event      _queue[QUEUE_SIZE];
		int               _head;
		int               _tail;
		Genode::Semaphore _sem;

	public:

		/**
		 * Constructor
		 */
		Event_queue(): _head(0), _tail(0)
		{
			Scout::memset(_queue, 0, sizeof(_queue));
		}

		void post(Input::Event ev)
		{
			if ((_head + 1)%QUEUE_SIZE != _tail) {
				_queue[_head] = ev;
				_head = (_head + 1)%QUEUE_SIZE;
				_sem.up();
			}
		}

		Input::Event get()
		{
			_sem.down();
			Input::Event dst_ev = _queue[_tail];
			_tail = (_tail + 1)%QUEUE_SIZE;
			return dst_ev;
		}

		int pending() { return _head != _tail; }

} _ev_queue;


/***************************
 ** Input service backend **
 ***************************/

namespace Input {

	void event_handling(bool enable) { }
	bool event_pending() { return _ev_queue.pending(); }
	Event get_event() { return _ev_queue.get(); }

}


class Window_content : public Scout::Element
{
	private:

		class Content_event_handler : public Scout::Event_handler
		{
			private:

				Event_queue *_ev_queue;
				Scout::Point _old_mouse_position;
				Element     *_element;

			public:

				Content_event_handler(Event_queue *ev_queue,
				                      Scout::Element *element)
				:
					_ev_queue(ev_queue), _element(element) { }

				void handle(Scout::Event &ev)
				{
					using namespace Scout;

					Point mouse_position = ev.mouse_position - _element->abs_position();

					int code = 0;

					if (ev.type == Event::PRESS || ev.type == Event::RELEASE)
						code = ev.code;

					Input::Event::Type type;

					type = (ev.type == Event::MOTION)  ? Input::Event::MOTION
					     : (ev.type == Event::PRESS)   ? Input::Event::PRESS
					     : (ev.type == Event::RELEASE) ? Input::Event::RELEASE
					     : Input::Event::INVALID;

					if (type != Input::Event::INVALID)
						_ev_queue->post(Input::Event(type, code, mouse_position.x(),
						                             mouse_position.y(),
						                             mouse_position.x() - _old_mouse_position.x(),
						                             mouse_position.y() - _old_mouse_position.y()));

					_old_mouse_position = mouse_position;
				}
		};

		struct Fb_texture
		{
			unsigned                              w, h;
			Genode::Attached_ram_dataspace        ds;
			Genode::Pixel_rgb565                 *pixel;
			unsigned char                        *alpha;
			Genode::Texture<Genode::Pixel_rgb565> texture;

			Fb_texture(unsigned w, unsigned h, bool config_alpha)
			:
				w(w), h(h),
				ds(Genode::env()->ram_session(), w*h*sizeof(Genode::Pixel_rgb565)),
				pixel(ds.local_addr<Genode::Pixel_rgb565>()),
				alpha((unsigned char *)Genode::env()->heap()->alloc(w*h)),
				texture(pixel, alpha, Scout::Area(w, h))
			{
				int alpha_min = config_alpha ? 0 : 255;

				/* init alpha channel */
				for (unsigned y = 0; y < h; y++)
					for (unsigned x = 0; x < w; x++) {

						int v = (x * y + (w*h)/4) / w;
						v = v + (x + y)/2;
						int a = v & 0xff;
						if (v & 0x100)
							a = 255 - a;

						a += (Genode::Dither_matrix::value(x, y) - 127) >> 4;

						alpha[y*w + x] = Genode::max(alpha_min, Genode::min(a, 255));
					}
			}

			~Fb_texture()
			{
				Genode::env()->heap()->free(alpha, w*h);
			}

		};

		bool                              _config_alpha;
		Content_event_handler             _ev_handler;
		Fb_texture                       *_fb;

		/**
		 * Size of the framebuffer handed out by the next call of 'dataspace'
		 */
		Scout::Area _next_size;

		/**
		 * Most current designated size of the framebuffer as defined by the
		 * user.
		 *
		 * The '_designated_size' may be updated any time when the user drags
		 * the resize handle of the window. It is propagated to '_next_size'
		 * not before the framebuffer client requests the current mode. Once
		 * the mode information is passed to the client, it is locked until
		 * the client requests the mode again.
		 */
		Scout::Area _designated_size;

		Genode::Signal_context_capability _mode_sigh;

	public:

		Window_content(unsigned fb_w, unsigned fb_h, Event_queue *ev_queue,
		               bool config_alpha)
		:
			_config_alpha(config_alpha),
			_ev_handler(ev_queue, this),
			_fb(new (Genode::env()->heap()) Fb_texture(fb_w, fb_h, _config_alpha)),
			_next_size(fb_w, fb_h),
			_designated_size(_next_size)
		{
			_min_size = Scout::Area(_fb->w, _fb->h);

			event_handler(&_ev_handler);
		}

		Genode::Dataspace_capability fb_ds_cap() { return _fb->ds.cap(); }

		unsigned fb_w() { return _fb->w; }
		unsigned fb_h() { return _fb->h; }

		Scout::Area mode_size()
		{
			_next_size = _designated_size;
			return _next_size;
		}

		void mode_sigh(Genode::Signal_context_capability sigh)
		{
			_mode_sigh = sigh;
		}

		void realloc_framebuffer()
		{
			/* skip reallocation if size has not changed */
			if (_next_size.w() == _fb->w && _next_size.h() == _fb->h)
				return;

			Genode::destroy(Genode::env()->heap(), _fb);

			_fb = new (Genode::env()->heap())
			      Fb_texture(_next_size.w(), _next_size.h(), _config_alpha);
		}

		/**
		 * Element interface
		 */
		void draw(Scout::Canvas_base &canvas, Scout::Point abs_position)
		{
			canvas.draw_texture(abs_position + _position, _fb->texture);
		}

		void format_fixed_size(Scout::Area size)
		{
			_designated_size = size;

			/* notify framebuffer client about mode change */
			if (_mode_sigh.valid())
				Genode::Signal_transmitter(_mode_sigh).submit();
		}
};


static Window_content *_window_content;

Scout::Element *window_content() { return _window_content; }


/***********************************************
 ** Implementation of the framebuffer service **
 ***********************************************/

namespace Framebuffer
{
	class Session_component : public Genode::Rpc_object<Session>
	{
		private:

			Window_content &_window_content;

		public:

			Session_component(Window_content &window_content)
			: _window_content(window_content) { }

			Genode::Dataspace_capability dataspace()
			{
				_window_content.realloc_framebuffer();
				return _window_content.fb_ds_cap();
			}

			Mode mode() const
			{
				return Mode(_window_content.mode_size().w(),
				            _window_content.mode_size().h(), Mode::RGB565);
			}

			void mode_sigh(Genode::Signal_context_capability sigh) {
				_window_content.mode_sigh(sigh); }

			void refresh(int x, int y, int w, int h)
			{
				_window_content.redraw_area(x, y, w, h);
			}
	};


	class Root : public Genode::Root_component<Session_component>
	{
		private:

			Window_content &_window_content;

		protected:

			Session_component *_create_session(const char *args) {
				return new (md_alloc()) Session_component(_window_content); }

		public:

			Root(Genode::Rpc_entrypoint *session_ep,
			     Genode::Allocator      *md_alloc,
			     Window_content         &window_content)
			:
				Genode::Root_component<Session_component>(session_ep, md_alloc),
				_window_content(window_content)
			{ }
	};
}


void init_window_content(unsigned fb_w, unsigned fb_h, bool config_alpha)
{
	static Window_content content(fb_w, fb_h, &_ev_queue, config_alpha);
	_window_content = &content;
}


void init_services(Genode::Rpc_entrypoint &ep)
{
	using namespace Genode;

	/*
	 * Let the entry point serve the framebuffer and input root interfaces
	 */
	static Framebuffer::Root    fb_root(&ep, env()->heap(), *_window_content);
	static       Input::Root input_root(&ep, env()->heap());

	/*
	 * Now, the root interfaces are ready to accept requests.
	 * This is the right time to tell mummy about our services.
	 */
	env()->parent()->announce(ep.manage(&fb_root));
	env()->parent()->announce(ep.manage(&input_root));
}
