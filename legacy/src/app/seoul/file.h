/*
 * \brief  File handling
 * \author Alexander Boettcher
 * \date   2025-11-28
 */

/*
 * Copyright (C) 2025 Alexander Boettcher
 *
 * This file is part of Seoul, which is distributed
 * under the terms of the GNU General Public License version 2.
 */


#include <util/bit_allocator.h>
#include <file_system_session/connection.h>


namespace Seoul {
	class Filesystem;
	class File;

	using namespace Genode;
	using namespace File_system;

	typedef File_system::Packet_descriptor Packet;
	typedef File_system::Session           Session;
}


class Seoul::File
{
	private:

		File_system::Connection &_fs;
		File_handle              _file_handle;
		uint64_t                 _fs_offset { 0 };
		size_t                   _pos       { 0 };

		uint16_t                 _cnt_flush_pending { 0 };
		uint16_t                 _cnt_flush_last { 0 };
		uint16_t                 _cnt_packet_err { 0 };
		uint64_t                 _cnt_data_lost { 0 };
		uint64_t                 _cnt_data_last { 0 };

		bool                     _flush_pending { false };
		size_t const             _max;
		char                     _buffer [8192];

	public:

		File (File_system::Connection &fs, char const *file, size_t max)
		:
			_fs(fs),
			_file_handle { _fs.file(_fs.dir("/", false), file,
			                        READ_WRITE, true /* create */) },
			_max(max > sizeof(_buffer) ? sizeof(_buffer) : max)
		{ }

		bool write(void *data, size_t const size)
		{
			(void)data;
			(void)size;
#if 0
			if (!size || size > _max || _pos + size >= _max) {
				_cnt_data_lost += size;

				if (!_cnt_data_last || (_cnt_data_last + 10000 < _cnt_data_lost)) {
					_cnt_data_last = _cnt_data_lost;
					Genode::warning("file ", _file_handle, " - lost=", _cnt_data_lost);
				}
				return false;
			}

			memcpy(_buffer + _pos, data, size);

			_pos       += size;
			_fs_offset += size;
#endif

			return true;
		}

		void flush_data(Session::Tx::Source &tx)
		{
#if 0
			if (empty()) return;

			try {
				Packet packet { tx.alloc_packet(_pos), _file_handle,
				                Packet::WRITE, _pos, _fs_offset - _pos};

				memcpy(((char *)tx.packet_content(packet)), _buffer, _pos);

#if 0
				error(this, " fs_offset=", _fs_offset, " send=", _pos);
#endif
				_pos = 0;

				tx.submit_packet(packet);
			} catch (Session::Tx::Source::Packet_alloc_failed) {
				_cnt_packet_err ++;
				if (_cnt_packet_err % 10 == 1)
					error("file ", _file_handle, " - ", _cnt_packet_err, ". packet error, lost=", _cnt_data_lost, " pending flush=", pending());
			}
#endif
		}

		bool flush(size_t const space) const { return _pos + space >= _max; }
		bool empty() const { return _pos == 0; }

		bool pending() const { return _flush_pending; }
		void flush_pending() { _cnt_flush_pending ++; _flush_pending = true; }
		void reset_pending() { _flush_pending = false; }

		void stat_pending_cnt()
		{
			if (_cnt_flush_last + 10 > _cnt_flush_pending) return;

			_cnt_flush_last = _cnt_flush_pending;

			Genode::log("file ", _file_handle, " - ", _cnt_flush_pending, " lost=", _cnt_data_lost);
		}
};

class Seoul::Filesystem : public StaticReceiver<Filesystem>
{
	private:

		Env                     &env;
		Heap                     heap  { env.ram(), env.rm() };
		Allocator_avl            alloc { &heap };
		File_system::Connection  fs    { env, alloc, "/", false };
		Dir_handle               dir   { fs.dir("/", false) };
		Session::Tx::Source     &tx    { *fs.tx() };

		size_t const _packet_max { tx.bulk_buffer_size() / Session::TX_QUEUE_SIZE };

		Signal_handler<Filesystem> _handler { env.ep(), *this, &Filesystem::_handle_submit };

		void _handle_ack()
		{
			error(__func__);

			while (tx.ack_avail()) {
				auto packet = tx.get_acked_packet();

#if 0
		Node_handle handle()    const { return _handle;   }
		Opcode      operation() const { return _op;       }
		seek_off_t  position()  const { return _op != Opcode::WRITE_TIMESTAMP ? _position : 0; }
		size_t      length()    const { return _op != Opcode::WRITE_TIMESTAMP ? _length : 0;   }
		bool        succeeded() const { return _success;  }
#endif

				switch (packet.operation()) {
				case Packet::READ_READY:
					error(__func__, " READ READY");
					break;

				case Packet::READ:
				{
					bool wakeup = false;

					error(__func__, " READ ", packet.succeeded() ? "succeeded" : "failure", " handle=", packet.handle(), " len=", packet.length(), "/?", sizeof(Directory_entry), " node handle ", packet.handle().value, " offset=", packet.offset());
					if (packet.succeeded()) {
						auto const count = packet.length() / sizeof(Directory_entry);
						auto entries = reinterpret_cast<Directory_entry *>(tx.packet_content(packet));
						for (unsigned i = 0; i < count; i++) {
							error("dir: ", (char const *)((entries + i)->name.buf));
						}
					}
					if (with_packet(packet, [&](auto) {
						error("success");
						if (!packet.succeeded() || !_queue_read_dir) {
							error("queue read dir failure");
							return;
						}

						MessageFs * r_msg = reinterpret_cast<MessageFs *>(_queue_read_dir);

						auto const count = packet.length() / sizeof(Directory_entry);
						auto entries = reinterpret_cast<Directory_entry *>(tx.packet_content(packet));
						for (unsigned i = 0; i < count; i++) {
							auto name = reinterpret_cast<char const *>((entries + i)->name.buf);
							auto name_len = strnlen(name, MAX_NAME_LEN);
							error("dir ", i, " : ", name, " ", name_len);
							if (!r_msg->add_read_dir(name, unsigned(name_len)))
								error("queue read failure");
							else {
								if (_queue_reads) {
									_queue_reads --;
									if (!_queue_reads)
										wakeup = true;
								}
							}
						}
					})) {
						if (wakeup)
							_queue_read_block.wakeup();
						continue;
					}
					else
						error("unknown read");

					break;
				}
				case Packet::WRITE:
					error(__func__, " WRITE");
					break;

				case Packet::SYNC:
					error(__func__, " SYNC");
					break;

				case Packet::CONTENT_CHANGED:
					error(__func__, " CONTENT CHANGED");
					break;

				case Packet::WRITE_TIMESTAMP:
					error(__func__, " WRITE TIMESTAMP");
					break;
				};

				tx.release_packet(packet);
			}
		}

		void _handle_submit()
		{
			error(__func__);
			_handle_ack();

#if 0
			if (_data.pending()) {
				if (!tx.ready_to_submit()) {
					Genode::error("no space for submitting new data ?");
					return;
				}
				_data.stat_pending_cnt();
				_data.flush_data(tx);
				_data.reset_pending();
			}

			if (_subject.pending()) {
				if (!tx.ready_to_submit()) {
					Genode::error("no space for submitting new subject ?");
					return;
				}
				_subject.stat_pending_cnt();
				_subject.flush_data(tx);
				_subject.reset_pending();
			}

			if (_select.pending()) {
				if (!tx.ready_to_submit()) {
					Genode::error("no space for submitting new select ?");
					return;
				}
				_select.stat_pending_cnt();
				_select.flush_data(tx);
				_select.reset_pending();
			}
#endif
		}

		template <typename T>
		void _write(T value, Seoul::File &file)
		{
			file.write(&value, sizeof(value));

			/* ask for whether flushing is appropriate */
			if (!file.flush(sizeof(value) * 2)) return;

			if (!tx.ready_to_submit())
			{
				/* check for available acks */
				_handle_ack();
				if (!tx.ready_to_submit()) {
					/* remember that we could not send data */
					file.flush_pending();
					return;
				}
			}
			if (file.pending()) {
				Genode::warning("pending but got not processed before next write ... ");
				file.reset_pending();
			}

			file.flush_data(tx);
		}

		Bit_allocator<64> _idx_alloc   { }; 
		Packet            _packets[64] { };

		uintptr_t   _queue_read_dir   { };
		Blockade    _queue_read_block { };
		unsigned    _queue_reads      { };

		bool with_new_packet(auto const &fn)
		{
			return _idx_alloc.alloc().convert<bool>([&](auto const &id) {
				fn(id);
				return true;
			}, [](auto) { return false; });
		}

		bool with_packet(auto &packet, auto const &fn)
		{
			for (unsigned idx = 0; idx < 64; idx++) {
				auto &p = _packets[idx];

				if (p.offset() != packet.offset() || p.size() != packet.size())
					continue;

				fn(packet);

				_idx_alloc.free(idx);

				tx.release_packet(packet);

				return true;
			}

			return false;
		}

	public:

		Filesystem(Env &env, Motherboard &mb) : env(env)
		{
			fs.sigh(_handler);

			mb.bus_fs.add(this, receive_static<MessageFs>);

#if 0
			queue_read_dir(dir);

			tx.wakeup();
#endif

/*
	file_size_t   size;
	Node_type     type;
	Node_rwx      rwx;
	unsigned long inode;
	Timestamp     modification_time;
*/
		}

		bool receive(MessageFs &msg)
		{
			bool wakeup = false;

			/* XXX fs_id check ? */
			error("MesageFs fs frontend");

			switch(msg.type) {
			case MessageFs::OPEN_DIR:
				error("read open dir ", dir.value, " ");
				msg.fh = dir.value;
				break;
			case MessageFs::READ_DIR:
				error("read dir ", msg.fh, " ");
				if (_queue_read_dir) {
					error("unexpected read dir state");
					return false;
				}

				_queue_read_dir = uintptr_t(&msg);

				Dir_handle dir { msg.fh };
				queue_read_dir(dir, _queue_reads);

				tx.wakeup();

				_queue_read_block.block();

				_queue_read_dir = { };

				error("read dir continue ");

				break;
			}

			if (wakeup)
				tx.wakeup();

			return true;
		}

		void queue_read_dir(Dir_handle const &dir_handle, unsigned &count)
		{
			auto num_entries = fs.num_entries(dir_handle);

			if (!num_entries)
				return;

			count = num_entries;

			File_handle file_handle { fs.file(dir_handle, ".",
			                          File_system::Mode::READ_ONLY,
			                          false /* create */) };

			/* XXX what if too many entries for buffer */
			auto packet_size = sizeof(Directory_entry);

			/* XXX alloc packet failures */
			for (unsigned i = 0; i < num_entries; i++) {

				with_new_packet([&](auto &idx) {

					auto &packet = _packets[idx];

					packet = Packet(tx.alloc_packet(packet_size),
					                dir_handle, Packet::READ,
					                packet_size, i * packet_size);

					if (!tx.ready_to_submit())
						error("read_dir: not ready to submit");

					if (!tx.try_submit_packet(packet))
						error("read_dir: could not submit");
				});
			}

			fs.close(file_handle);
			/* XXX file_handle close ? */
		}
};
